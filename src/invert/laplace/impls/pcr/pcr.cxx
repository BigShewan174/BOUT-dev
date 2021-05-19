/**************************************************************************
 * Perpendicular Laplacian inversion. Parallel code using FFTs in z
 * and parallel cyclic reduction in x.
 *
 **************************************************************************
 * Copyright 2020 Joseph Parker
 *
 * Contact: Ben Dudson, bd512@york.ac.uk
 *
 * This file is part of BOUT++.
 *
 * BOUT++ is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BOUT++ is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with BOUT++.  If not, see <http://www.gnu.org/licenses/>.
 *
 **************************************************************************/

#include "pcr.hxx"
#include "globals.hxx"

#include <bout/constants.hxx>
#include <bout/mesh.hxx>
#include <bout/openmpwrap.hxx>
#include <bout/sys/timer.hxx>
#include <boutexception.hxx>
#include <cmath>
#include <fft.hxx>
#include <lapack_routines.hxx>
#include <utils.hxx>

#include "boutcomm.hxx"
#include <output.hxx>

#include <bout/scorepwrapper.hxx>

#include <cmath>
#include <mpi.h>
#include <cstdlib>
#include <algorithm>

using namespace std;


LaplacePCR::LaplacePCR(Options* opt, CELL_LOC loc, Mesh* mesh_in)
    : Laplacian(opt, loc, mesh_in),
      Acoef(0.0, localmesh), C1coef(1.0, localmesh), C2coef(1.0, localmesh), Dcoef(1.0, localmesh), nmode(maxmode + 1),
      ncx(localmesh->LocalNx), ny(localmesh->LocalNy), avec(ny, nmode, ncx),
      bvec(ny, nmode, ncx), cvec(ny, nmode, ncx) {

  Acoef.setLocation(location);
  C1coef.setLocation(location);
  C2coef.setLocation(location);
  Dcoef.setLocation(location);

  // Number of X procs must be a power of 2
  const int nxpe = localmesh->getNXPE();
  if (!is_pow2(nxpe)) {
    throw BoutException("LaplacePCR error: NXPE must be a power of 2");
  }

  // Number of Y procs must be 1 
  // TODO relax this constrant - requires reworking comms in PCR
  const int nype = localmesh->getNYPE();
  if (nype != 1) {
    throw BoutException("LaplacePCR error: NYPE must equal 1");
  }

  // Cannot be run in serial
  if(localmesh->firstX() && localmesh->lastX()) {
    throw BoutException("Error: PCR method only works for NXPE > 1. Suggest using cyclic solver for NXPE = 1.\n");
  }

  // Number of x points must be a power of 2
  if (!is_pow2(localmesh->GlobalNx-4)) {
    throw BoutException("LaplacePCR error: GlobalNx must be a power of 2");
  }

  Acoef.setLocation(location);
  C1coef.setLocation(location);
  C2coef.setLocation(location);
  Dcoef.setLocation(location);

  // Get options

  OPTION(opt, dst, false);

  if(dst) {
    nmode = localmesh->LocalNz-2;
  }else
    nmode = maxmode+1; // Number of Z modes. maxmode set in invert_laplace.cxx from options

  // Note nmode == nsys of cyclic_reduction

  // Allocate arrays

  xs = localmesh->xstart; // Starting X index
  if(localmesh->firstX() && !localmesh->periodicX){ // Only want to include guard cells at boundaries (unless periodic in x)
	  xs = 0;
  }
  xe = localmesh->xend;   // Last X index
  if(localmesh->lastX() && !localmesh->periodicX){ // Only want to include guard cells at boundaries (unless periodic in x)
	  xe = localmesh->LocalNx-1;
  }
  int n = xe - xs + 1;  // Number of X points on this processor,
                        // including boundaries but not guard cells

  a.reallocate(nmode, n);
  b.reallocate(nmode, n);
  c.reallocate(nmode, n);
  xcmplx.reallocate(nmode, n);
  bcmplx.reallocate(nmode, n);

  setup(localmesh->GlobalNx-4, localmesh->getNXPE(), localmesh->getXProcIndex());

}

/// Calculate the transpose of \p m in the pre-allocated \p m_t
namespace {
void transpose(Matrix<dcomplex>& m_t, const Matrix<dcomplex>& m) {
  SCOREP0();
  const auto n1 = std::get<1>(m.shape());
  const auto n2 = std::get<0>(m.shape());
  for (int i1 = 0; i1 < n1; i1++) {
    for (int i2 = 0; i2 < n2; i2++) {
      m_t(i1, i2) = m(i2, i1);
    }
  }
}
} // namespace

/*!
 * Solve Ax=b for x given b
 *
 * This function will
 *      1. Take the fourier transform of the y-slice given in the input
 *      2. For each fourier mode
 *          a) Set up the tridiagonal matrix
 *          b) Call the solver which inverts the matrix Ax_mode = b_mode
 *      3. Collect all the modes in a 2D array
 *      4. Back transform the y-slice
 *
 * Input:
 * \param[in] b     A 2D variable that will be fourier decomposed, each fourier
 *                  mode of this variable is going to be the right hand side of
 *                  the equation Ax = b
 * \param[in] x0    Variable used to set BC (if the right flags are set, see
 *                  the user manual)
 *
 * \return          The inverted variable.
 */
///FieldPerp LaplacePCR::solve(const FieldPerp& b, const FieldPerp& x0) {
///
///  SCOREP0();
///  Timer timer("invert"); ///< Start timer
///
///  /// SCOREP_USER_REGION_DEFINE(initvars);
///  /// SCOREP_USER_REGION_BEGIN(initvars, "init vars",///SCOREP_USER_REGION_TYPE_COMMON);
///
///  ASSERT1(localmesh == b.getMesh() && localmesh == x0.getMesh());
///  ASSERT1(b.getLocation() == location);
///  ASSERT1(x0.getLocation() == location);
///  TRACE("LaplacePCR::solve(const, const)");
///
///  FieldPerp x{emptyFrom(b)};
///
///  // Info for halo swaps
///  const int xproc = localmesh->getXProcIndex();
///  const int yproc = localmesh->getYProcIndex();
///  nproc = localmesh->getNXPE();
///  myproc = yproc * nproc + xproc;
///  proc_in = myproc - 1;
///  proc_out = myproc + 1;
///
///  jy = b.getIndex();
///
///  const int ncz = localmesh->LocalNz; // Number of local z points
///
///  xs = localmesh->xstart; // First interior point
///  xe = localmesh->xend;   // Last interior point
///
///  const BoutReal kwaveFactor = 2.0 * PI / coords->zlength();
///
///  // Setting the width of the boundary.
///  // NOTE: The default is a width of 2 guard cells
///  const bool both_use_one_guard =
///      isGlobalFlagSet(INVERT_BOTH_BNDRY_ONE) || (localmesh->xstart < 2);
///  const int inbndry = (both_use_one_guard or isInnerBoundaryFlagSet(INVERT_BNDRY_ONE))
///                          ? 1
///                          : localmesh->xstart;
///  const int outbndry = (both_use_one_guard or isOuterBoundaryFlagSet(INVERT_BNDRY_ONE))
///                           ? 1
///                           : localmesh->xstart;
///
///  /* Allocation for
///   * bk   = The fourier transformed of b, where b is one of the inputs in
///   *        LaplacePCR::solve()
///   * bk1d = The 1d array of bk
///   * xk   = The fourier transformed of x, where x the output of
///   *        LaplacePCR::solve()
///   * xk1d = The 1d array of xk
///   */
///  auto bk = Matrix<dcomplex>(ncx, nmode);
///  auto bk1d = Array<dcomplex>(ncx);
///  auto xk = Matrix<dcomplex>(ncx, nmode);
///  auto xk1d = Matrix<dcomplex>(nmode, ncx);
///
///  /// SCOREP_USER_REGION_END(initvars);
///  /// SCOREP_USER_REGION_DEFINE(fftloop);
///  /// SCOREP_USER_REGION_BEGIN(fftloop, "init fft loop",SCOREP_USER_REGION_TYPE_COMMON);
///
///  /* Coefficents in the tridiagonal solver matrix
///   * Following the notation in "Numerical recipes"
///   * avec is the lower diagonal of the matrix
///   * bvec is the diagonal of the matrix
///   * cvec is the upper diagonal of the matrix
///   * NOTE: Do not confuse avec, bvec and cvec with the A, C, and D coefficients
///   *       above
///   */
///  auto bcmplx = Matrix<dcomplex>(ncz / 2 + 1, ncx);
///
///  const bool invert_inner_boundary =
///      isInnerBoundaryFlagSet(INVERT_SET) and localmesh->firstX();
///  const bool invert_outer_boundary =
///      isOuterBoundaryFlagSet(INVERT_SET) and localmesh->lastX();
///
///  BOUT_OMP(parallel for)
///  for (int ix = 0; ix < ncx; ix++) {
///    /* This for loop will set the bk (initialized by the constructor)
///     * bk is the z fourier modes of b in z
///     * If the INVERT_SET flag is set (meaning that x0 will be used to set the
///     * boundary values),
///     */
///    if ((invert_inner_boundary and (ix < inbndry))
///        or (invert_outer_boundary and (ncx - ix - 1 < outbndry))) {
///      // Use the values in x0 in the boundary
///      rfft(x0[ix], ncz, &bk(ix, 0));
///    } else {
///      rfft(b[ix], ncz, &bk(ix, 0));
///    }
///  }
///  /// SCOREP_USER_REGION_END(fftloop);
///  /// SCOREP_USER_REGION_DEFINE(kzinit);
///  /// SCOREP_USER_REGION_BEGIN(kzinit, "kz init",///SCOREP_USER_REGION_TYPE_COMMON);
///
///  /* Solve differential equation in x for each fourier mode, so transpose to make x the
///   * fastest moving index. Note that only the non-degenerate fourier modes are used (i.e.
///   * the offset and all the modes up to the Nyquist frequency), so we only copy up to
///   * `nmode` in the transpose.
///   */
///  //output.write("Before transpose\n");
///  //output.write("ncx {}, nmode {}, ncz {}, ncz / 2 + 1 {}, xe-xs+1 {}  \n",ncx,nmode,ncz,ncz / 2 + 1,xe-xs+1);
///  transpose(bcmplx, bk);
///
///  /* Set the matrix A used in the inversion of Ax=b
///   * by calling tridagCoef and setting the BC
///   *
///   * Note that A, C and D in
///   *
///   * D*Laplace_perp(x) + (1/C)Grad_perp(C)*Grad_perp(x) + Ax = B
///   *
///   * has nothing to do with
///   * avec - the lower diagonal of the tridiagonal matrix
///   * bvec - the main diagonal
///   * cvec - the upper diagonal
///   *
///   */
///  //output.write("Before coefs\n");
///  for (int kz = 0; kz < nmode; kz++) {
///    // Note that this is called every time to deal with bcmplx and could mostly
///    // be skipped when storing coefficients.
///    tridagMatrix(&avec(jy, kz, 0), &bvec(jy, kz, 0), &cvec(jy, kz, 0), &bcmplx(kz, 0), jy,
///                 // wave number index
///                 kz,
///                 // wave number (different from kz only if we are taking a part
///                 // of the z-domain [and not from 0 to 2*pi])
///                 kz * kwaveFactor, global_flags, inner_boundary_flags,
///                 outer_boundary_flags, &A, &C, &D);
///
//////    // Patch up internal boundaries
//////    if (not localmesh->lastX()) {
//////      for (int ix = localmesh->xend + 1; ix < localmesh->LocalNx; ix++) {
//////        avec(jy, kz, ix) = 0;
//////        bvec(jy, kz, ix) = 1;
//////        cvec(jy, kz, ix) = 0;
//////        bcmplx(kz, ix) = 0;
//////      }
//////    }
//////    if (not localmesh->firstX()) {
//////      for (int ix = 0; ix < localmesh->xstart; ix++) {
//////        avec(jy, kz, ix) = 0;
//////        bvec(jy, kz, ix) = 1;
//////        cvec(jy, kz, ix) = 0;
//////        bcmplx(kz, ix) = 0;
//////      }
//////    }
///  }
///  /// SCOREP_USER_REGION_END(kzinit);
///  /// SCOREP_USER_REGION_DEFINE(initlevels);
///  /// SCOREP_USER_REGION_BEGIN(initlevels, "init
///  /// levels",///SCOREP_USER_REGION_TYPE_COMMON);
///
//////  output.write("before\n");
//////  for(int kz=0;kz<nmode;kz++){
//////    for(int ix=0;ix<localmesh->LocalNx;ix++){
//////      output.write("{} ",avec(jy,kz,ix).real());
//////    }
//////    output.write("\n");
//////    for(int ix=0;ix<localmesh->LocalNx;ix++){
//////      output.write("{} ",bvec(jy,kz,ix).real());
//////    }
//////    output.write("\n");
//////    for(int ix=0;ix<localmesh->LocalNx;ix++){
//////      output.write("{} ",cvec(jy,kz,ix).real());
//////    }
//////    output.write("\n");
//////    for(int ix=0;ix<localmesh->LocalNx;ix++){
//////      output.write("{} ",bcmplx(kz,ix).real());
//////    }
//////    output.write("\n");
//////  }
///
///  // eliminate boundary rows - this is necessary to ensure we solve a square
///  // system of interior rows
///  if (localmesh->firstX()) {
///    for (int kz = 0; kz < nmode; kz++) {
///      bvec(jy,kz,xs) = bvec(jy,kz,xs) - cvec(jy, kz, xs-1) * avec(jy,kz,xs) / bvec(jy, kz, xs-1);
///    }
///  }
///  if (localmesh->lastX()) {
///    for (int kz = 0; kz < nmode; kz++) {
///      bvec(jy,kz,xe) = bvec(jy,kz,xe) - cvec(jy, kz, xe) * avec(jy,kz,xe+1) / bvec(jy, kz, xe+1);
///    }
///  }
///
///  // Perform the parallel triadiagonal solver
///  // Note the API switches sub and super diagonals
///  //output.write("Before solve\n");
///  cr_pcr_solver(cvec,bvec,avec,bcmplx,xk1d,jy);
///
///  //output.write("Before bcs\n");
///  // apply boundary conditions
///  if (localmesh->firstX()) {
///    for (int kz = 0; kz < nmode; kz++) {
///      for(int ix = xs-1; ix >= 0; ix--){
///        xk1d(kz,ix) = (bcmplx(kz, ix) 
///                      - cvec(jy, kz, ix) * xk1d(kz,ix+1)) / bvec(jy, kz, ix);
///      }
///    }
///  }
///  if (localmesh->lastX()) {
///    for (int kz = 0; kz < nmode; kz++) {
///      for(int ix = xe+1; ix < localmesh->LocalNx; ix++){
///        xk1d(kz,ix) = (bcmplx(kz,ix) 
///		      - avec(jy, kz, ix) * xk1d(kz,ix-1)) / bvec(jy, kz, ix);
///      }
///    }
///  }
///
//////  output.write("after\n");
//////  for(int kz=0;kz<nmode;kz++){
//////    for(int ix=0;ix<localmesh->LocalNx;ix++){
//////      output.write("{} ",avec(jy,kz,ix).real());
//////    }
//////    output.write("\n");
//////    for(int ix=0;ix<localmesh->LocalNx;ix++){
//////      output.write("{} ",bvec(jy,kz,ix).real());
//////    }
//////    output.write("\n");
//////    for(int ix=0;ix<localmesh->LocalNx;ix++){
//////      output.write("{} ",cvec(jy,kz,ix).real());
//////    }
//////    output.write("\n");
//////    for(int ix=0;ix<localmesh->LocalNx;ix++){
//////      output.write("{} ",bcmplx(kz,ix).real());
//////    }
//////    output.write("\n");
//////  }
///
///  //output.write("After bcs\n");
///
///#if CHECK > 2
///  for (int ix = 0; ix < ncx; ix++) {
///    for (int kz = 0; kz < nmode; kz++) {
///      if (!finite(xk1d(kz, ix).real()) or !finite(xk1d(kz, ix).imag()))
///        throw BoutException("Non-finite xloc at {:d}, {:d}, {:d}", ix, jy, kz);
///    }
///  }
///#endif
///
///  // If the global flag is set to INVERT_KX_ZERO
///  if (isGlobalFlagSet(INVERT_KX_ZERO)) {
///    dcomplex offset(0.0);
///    for (int ix = localmesh->xstart; ix <= localmesh->xend; ix++) {
///      offset += xk1d(0, ix);
///    }
///    offset /= static_cast<BoutReal>(localmesh->xend - localmesh->xstart + 1);
///    for (int ix = localmesh->xstart; ix <= localmesh->xend; ix++) {
///      xk1d(0, ix) -= offset;
///    }
///  }
///
///  // Store the solution xk for the current fourier mode in a 2D array
///  transpose(xk, xk1d);
///  //output.write("After transpose 2\n");
///
///  /// SCOREP_USER_REGION_END(afterloop);
///
///  /// SCOREP_USER_REGION_DEFINE(fftback);
///  /// SCOREP_USER_REGION_BEGIN(fftback, "fft back",///SCOREP_USER_REGION_TYPE_COMMON);
///  // Done inversion, transform back
///  for (int ix = 0; ix < ncx; ix++) {
///
///    if (isGlobalFlagSet(INVERT_ZERO_DC)) {
///      xk(ix, 0) = 0.0;
///    }
///
///    irfft(&xk(ix, 0), ncz, x[ix]);
///
///#if CHECK > 2
///    for (int kz = 0; kz < ncz; kz++)
///      if (!finite(x(ix, kz)))
///        throw BoutException("Non-finite at {:d}, {:d}, {:d}", ix, jy, kz);
///#endif
///  }
///
///  //output.write("end\n");
///
///  /// SCOREP_USER_REGION_END(fftback);
///  return x; // Result of the inversion
///}

FieldPerp LaplacePCR::solve(const FieldPerp& rhs, const FieldPerp& x0) {
  output.write("LaplacePCR::solve(const FieldPerp, const FieldPerp)");
  ASSERT1(localmesh == rhs.getMesh() && localmesh == x0.getMesh());
  ASSERT1(rhs.getLocation() == location);
  ASSERT1(x0.getLocation() == location);

  FieldPerp x{emptyFrom(rhs)}; // Result

  int jy = rhs.getIndex();  // Get the Y index
  x.setIndex(jy);

  // Get the width of the boundary

  // If the flags to assign that only one guard cell should be used is set
  int inbndry = localmesh->xstart, outbndry=localmesh->xstart;
  if((global_flags & INVERT_BOTH_BNDRY_ONE) || (localmesh->xstart < 2))  {
    inbndry = outbndry = 1;
  }
  if(inner_boundary_flags & INVERT_BNDRY_ONE)
    inbndry = 1;
  if(outer_boundary_flags & INVERT_BNDRY_ONE)
    outbndry = 1;

  if(dst) {
    BOUT_OMP(parallel) {
      /// Create a local thread-scope working array
      auto k1d =
          Array<dcomplex>(localmesh->LocalNz); // ZFFT routine expects input of this length

      // Loop over X indices, including boundaries but not guard cells. (unless periodic
      // in x)
      BOUT_OMP(for)
      for (int ix = xs; ix <= xe; ix++) {
        // Take DST in Z direction and put result in k1d

        if (((ix < inbndry) && (inner_boundary_flags & INVERT_SET) && localmesh->firstX()) ||
            ((localmesh->LocalNx - ix - 1 < outbndry) && (outer_boundary_flags & INVERT_SET) &&
             localmesh->lastX())) {
          // Use the values in x0 in the boundary
          DST(x0[ix] + 1, localmesh->LocalNz - 2, std::begin(k1d));
        } else {
          DST(rhs[ix] + 1, localmesh->LocalNz - 2, std::begin(k1d));
        }

        // Copy into array, transposing so kz is first index
        for (int kz = 0; kz < nmode; kz++)
          bcmplx(kz, ix - xs) = k1d[kz];
      }

      // Get elements of the tridiagonal matrix
      // including boundary conditions
      BOUT_OMP(for nowait)
      for (int kz = 0; kz < nmode; kz++) {
        BoutReal zlen = coords->dz * (localmesh->LocalNz - 3);
        BoutReal kwave =
            kz * 2.0 * PI / (2. * zlen); // wave number is 1/[rad]; DST has extra 2.

        tridagMatrix(&a(kz, 0), &b(kz, 0), &c(kz, 0), &bcmplx(kz, 0), jy,
                     kz,    // wave number index
                     kwave, // kwave (inverse wave length)
                     global_flags, inner_boundary_flags, outer_boundary_flags, &Acoef,
                     &C1coef, &C2coef, &Dcoef,
                     false); // Don't include guard cells in arrays
      }
    }

    // Solve tridiagonal systems
    //cr->setCoefs(a, b, c);
    //cr->solve(bcmplx, xcmplx);
  cr_pcr_solver(a,b,c,bcmplx,xcmplx);

    // FFT back to real space
    BOUT_OMP(parallel) {
      /// Create a local thread-scope working array
      auto k1d =
          Array<dcomplex>(localmesh->LocalNz); // ZFFT routine expects input of this length

      BOUT_OMP(for nowait)
      for (int ix = xs; ix <= xe; ix++) {
        for (int kz = 0; kz < nmode; kz++)
          k1d[kz] = xcmplx(kz, ix - xs);

        for (int kz = nmode; kz < (localmesh->LocalNz); kz++)
          k1d[kz] = 0.0; // Filtering out all higher harmonics

        DST_rev(std::begin(k1d), localmesh->LocalNz - 2, x[ix] + 1);

        x(ix, 0) = -x(ix, 2);
        x(ix, localmesh->LocalNz - 1) = -x(ix, localmesh->LocalNz - 3);
      }
    }
  }else {
    BOUT_OMP(parallel)
    {
      /// Create a local thread-scope working array
      auto k1d = Array<dcomplex>((localmesh->LocalNz) / 2 +
                                 1); // ZFFT routine expects input of this length

      // Loop over X indices, including boundaries but not guard cells (unless periodic in
      // x)
      BOUT_OMP(for)
      for (int ix = xs; ix <= xe; ix++) {
        // Take FFT in Z direction, apply shift, and put result in k1d

        if (((ix < inbndry) && (inner_boundary_flags & INVERT_SET) && localmesh->firstX()) ||
            ((localmesh->LocalNx - ix - 1 < outbndry) && (outer_boundary_flags & INVERT_SET) &&
             localmesh->lastX())) {
          // Use the values in x0 in the boundary
          rfft(x0[ix], localmesh->LocalNz, std::begin(k1d));
        } else {
          rfft(rhs[ix], localmesh->LocalNz, std::begin(k1d));
        }

        // Copy into array, transposing so kz is first index
        for (int kz = 0; kz < nmode; kz++)
          bcmplx(kz, ix - xs) = k1d[kz];
      }

      // Get elements of the tridiagonal matrix
      // including boundary conditions
      BOUT_OMP(for nowait)
      for (int kz = 0; kz < nmode; kz++) {
        BoutReal kwave = kz * 2.0 * PI / (coords->zlength()); // wave number is 1/[rad]
        tridagMatrix(&a(kz, 0), &b(kz, 0), &c(kz, 0), &bcmplx(kz, 0), jy,
                     kz,    // True for the component constant (DC) in Z
                     kwave, // Z wave number
                     global_flags, inner_boundary_flags, outer_boundary_flags, &Acoef,
                     &C1coef, &C2coef, &Dcoef,
                     false); // Don't include guard cells in arrays
      }
    }

    // Solve tridiagonal systems
    //cr->setCoefs(a, b, c);
    //cr->solve(bcmplx, xcmplx);
    cr_pcr_solver(a,b,c,bcmplx,xcmplx);

    // FFT back to real space
    BOUT_OMP(parallel)
    {
      /// Create a local thread-scope working array
      auto k1d = Array<dcomplex>((localmesh->LocalNz) / 2 +
                                 1); // ZFFT routine expects input of this length

      const bool zero_DC = global_flags & INVERT_ZERO_DC;

      BOUT_OMP(for nowait)
      for (int ix = xs; ix <= xe; ix++) {
        if (zero_DC) {
          k1d[0] = 0.;
        }

        for (int kz = zero_DC; kz < nmode; kz++)
          k1d[kz] = xcmplx(kz, ix - xs);

        for (int kz = nmode; kz < (localmesh->LocalNz) / 2 + 1; kz++)
          k1d[kz] = 0.0; // Filtering out all higher harmonics

        irfft(std::begin(k1d), localmesh->LocalNz, x[ix]);
      }
    }
  }

  checkData(x);

  return x;
}

Field3D LaplacePCR::solve(const Field3D& rhs, const Field3D& x0) {
  TRACE("LaplaceCyclic::solve(Field3D, Field3D)");
  output.write("LaplaceCyclic::solve(Field3D, Field3D)");

  ASSERT1(rhs.getLocation() == location);
  ASSERT1(x0.getLocation() == location);
  ASSERT1(localmesh == rhs.getMesh() && localmesh == x0.getMesh());

  Timer timer("invert");

  Field3D x{emptyFrom(rhs)}; // Result

  // Get the width of the boundary

  // If the flags to assign that only one guard cell should be used is set
  int inbndry = localmesh->xstart, outbndry = localmesh->xstart;
  if ((global_flags & INVERT_BOTH_BNDRY_ONE) || (localmesh->xstart < 2)) {
    inbndry = outbndry = 1;
  }
  if (inner_boundary_flags & INVERT_BNDRY_ONE)
    inbndry = 1;
  if (outer_boundary_flags & INVERT_BNDRY_ONE)
    outbndry = 1;

  int nx = xe - xs + 1; // Number of X points on this processor

  // Get range of Y indices
  int ys = localmesh->ystart, ye = localmesh->yend;

  if (localmesh->hasBndryLowerY()) {
    if (include_yguards)
      ys = 0; // Mesh contains a lower boundary and we are solving in the guard cells

    ys += extra_yguards_lower;
  }
  if (localmesh->hasBndryUpperY()) {
    if (include_yguards)
      ye = localmesh->LocalNy -
           1; // Contains upper boundary and we are solving in the guard cells

    ye -= extra_yguards_upper;
  }

  const int ny = (ye - ys + 1); // Number of Y points
  nsys = nmode * ny;  // Number of systems of equations to solve
  const int nxny = nx * ny;     // Number of points in X-Y

  auto a3D = Matrix<dcomplex>(nsys, nx);
  auto b3D = Matrix<dcomplex>(nsys, nx);
  auto c3D = Matrix<dcomplex>(nsys, nx);

  auto xcmplx3D = Matrix<dcomplex>(nsys, nx);
  auto bcmplx3D = Matrix<dcomplex>(nsys, nx);

///  output.write("LaplaceCyclic::solve before coefs\n");
  if (dst) {
    output.write("LaplaceCyclic::solve in DST\n");
    BOUT_OMP(parallel) {
      /// Create a local thread-scope working array
      auto k1d =
          Array<dcomplex>(localmesh->LocalNz); // ZFFT routine expects input of this length

      // Loop over X and Y indices, including boundaries but not guard cells.
      // (unless periodic in x)
      BOUT_OMP(for)
      for (int ind = 0; ind < nxny; ++ind) {
        // ind = (ix - xs)*(ye - ys + 1) + (iy - ys)
        int ix = xs + ind / ny;
        int iy = ys + ind % ny;

        // Take DST in Z direction and put result in k1d

        if (((ix < inbndry) && (inner_boundary_flags & INVERT_SET) && localmesh->firstX()) ||
            ((localmesh->LocalNx - ix - 1 < outbndry) && (outer_boundary_flags & INVERT_SET) &&
             localmesh->lastX())) {
          // Use the values in x0 in the boundary
          DST(x0(ix, iy) + 1, localmesh->LocalNz - 2, std::begin(k1d));
        } else {
          DST(rhs(ix, iy) + 1, localmesh->LocalNz - 2, std::begin(k1d));
        }

        // Copy into array, transposing so kz is first index
        for (int kz = 0; kz < nmode; kz++) {
          bcmplx3D((iy - ys) * nmode + kz, ix - xs) = k1d[kz];
        }
      }

      // Get elements of the tridiagonal matrix
      // including boundary conditions
      BOUT_OMP(for nowait)
      for (int ind = 0; ind < nsys; ind++) {
        // ind = (iy - ys) * nmode + kz
        int iy = ys + ind / nmode;
        int kz = ind % nmode;

        BoutReal zlen = coords->dz * (localmesh->LocalNz - 3);
        BoutReal kwave =
            kz * 2.0 * PI / (2. * zlen); // wave number is 1/[rad]; DST has extra 2.

        tridagMatrix(&a3D(ind, 0), &b3D(ind, 0), &c3D(ind, 0), &bcmplx3D(ind, 0), iy,
                     kz,    // wave number index
                     kwave, // kwave (inverse wave length)
                     global_flags, inner_boundary_flags, outer_boundary_flags, &Acoef,
                     &C1coef, &C2coef, &Dcoef,
                     false); // Don't include guard cells in arrays
      }
    }

    // Solve tridiagonal systems
    //cr->setCoefs(a3D, b3D, c3D);
    //cr->solve(bcmplx3D, xcmplx3D);
  // Perform the parallel triadiagonal solver
  // Note the API switches sub and super diagonals
///  output.write("LaplaceCyclic::solve before solve\n");
///  output.write("coefs before\n");
///  for(int kz=0;kz<nsys;kz++){
///    for(int ix=0;ix<localmesh->LocalNx;ix++){
///      output.write("{} ",a3D(kz,ix).real());
///    }
///    output.write("\n");
///    for(int ix=0;ix<localmesh->LocalNx;ix++){
///      output.write("{} ",b3D(kz,ix).real());
///    }
///    output.write("\n");
///    for(int ix=0;ix<localmesh->LocalNx;ix++){
///      output.write("{} ",c3D(kz,ix).real());
///    }
///    output.write("\n");
///    for(int ix=0;ix<localmesh->LocalNx;ix++){
///      output.write("{} ",bcmplx3D(kz,ix).real());
///    }
///    output.write("\n");
///  }
  cr_pcr_solver(a3D,b3D,c3D,bcmplx3D,xcmplx3D);

    // FFT back to real space
    BOUT_OMP(parallel) {
      /// Create a local thread-scope working array
      auto k1d =
          Array<dcomplex>(localmesh->LocalNz); // ZFFT routine expects input of this length

      BOUT_OMP(for nowait)
      for (int ind = 0; ind < nxny; ++ind) { // Loop over X and Y
        // ind = (ix - xs)*(ye - ys + 1) + (iy - ys)
        int ix = xs + ind / ny;
        int iy = ys + ind % ny;

        for (int kz = 0; kz < nmode; kz++) {
          k1d[kz] = xcmplx3D((iy - ys) * nmode + kz, ix - xs);
        }

        for (int kz = nmode; kz < localmesh->LocalNz; kz++)
          k1d[kz] = 0.0; // Filtering out all higher harmonics

        DST_rev(std::begin(k1d), localmesh->LocalNz - 2, &x(ix, iy, 1));

        x(ix, iy, 0) = -x(ix, iy, 2);
        x(ix, iy, localmesh->LocalNz - 1) = -x(ix, iy, localmesh->LocalNz - 3);
      }
    }
  } else {
    output.write("LaplaceCyclic::solve in NOT DST\n");
    BOUT_OMP(parallel) {
      /// Create a local thread-scope working array
      auto k1d = Array<dcomplex>(localmesh->LocalNz / 2 +
                                 1); // ZFFT routine expects input of this length

      // Loop over X and Y indices, including boundaries but not guard cells
      // (unless periodic in x)

      BOUT_OMP(for)
      for (int ind = 0; ind < nxny; ++ind) {
        // ind = (ix - xs)*(ye - ys + 1) + (iy - ys)
        int ix = xs + ind / ny;
        int iy = ys + ind % ny;

        // Take FFT in Z direction, apply shift, and put result in k1d

        if (((ix < inbndry) && (inner_boundary_flags & INVERT_SET) && localmesh->firstX()) ||
            ((localmesh->LocalNx - ix - 1 < outbndry) && (outer_boundary_flags & INVERT_SET) &&
             localmesh->lastX())) {
          // Use the values in x0 in the boundary
          rfft(x0(ix, iy), localmesh->LocalNz, std::begin(k1d));
        } else {
          rfft(rhs(ix, iy), localmesh->LocalNz, std::begin(k1d));
        }

        // Copy into array, transposing so kz is first index
        for (int kz = 0; kz < nmode; kz++)
          bcmplx3D((iy - ys) * nmode + kz, ix - xs) = k1d[kz];
      }

///      output.write("LaplaceCyclic::solve after ffts\n");

      // Get elements of the tridiagonal matrix
      // including boundary conditions
      BOUT_OMP(for nowait)
      for (int ind = 0; ind < nsys; ind++) {
        // ind = (iy - ys) * nmode + kz
        int iy = ys + ind / nmode;
        int kz = ind % nmode;

        BoutReal kwave = kz * 2.0 * PI / (coords->zlength()); // wave number is 1/[rad]
        //output.write("LaplaceCyclic::solve before tridag\n");
        tridagMatrix(&a3D(ind, 0), &b3D(ind, 0), &c3D(ind, 0), &bcmplx3D(ind, 0), iy,
                     kz,    // True for the component constant (DC) in Z
                     kwave, // Z wave number
                     global_flags, inner_boundary_flags, outer_boundary_flags, &Acoef,
                     &C1coef, &C2coef, &Dcoef,
                     false); // Don't include guard cells in arrays
      }
    }

    // Solve tridiagonal systems
    //cr->setCoefs(a3D, b3D, c3D);
    //cr->solve(bcmplx3D, xcmplx3D);
///    output.write("LaplaceCyclic::solve before solve\n");
///  output.write("coefs before\na3D ");
///  for(int kz=0;kz<nsys;kz++){
///    for(int ix=0;ix<nx;ix++){
///      output.write("{} ",a3D(kz,ix).real());
///    }
///    output.write("\nb3D ");
///    for(int ix=0;ix<nx;ix++){
///      output.write("{} ",b3D(kz,ix).real());
///    }
///    output.write("\nc3D ");
///    for(int ix=0;ix<nx;ix++){
///      output.write("{} ",c3D(kz,ix).real());
///    }
///    output.write("\nbcmplx3D ");
///    for(int ix=0;ix<nx;ix++){
///      output.write("{} ",bcmplx3D(kz,ix).real());
///    }
///    output.write("\n");
///  }
    cr_pcr_solver(a3D,b3D,c3D,bcmplx3D,xcmplx3D);
///    output.write("xcmplx3D ");
///  for(int kz=0;kz<nsys;kz++){
///    for(int ix=0;ix<nx;ix++){
///      output.write("{} ",xcmplx3D(kz,ix).real());
///    }
///    output.write("\n");
///  }
    verify_solution(a3D,b3D,c3D,bcmplx3D,xcmplx3D);

    // FFT back to real space
    BOUT_OMP(parallel) {
      /// Create a local thread-scope working array
      auto k1d = Array<dcomplex>((localmesh->LocalNz) / 2 +
                                 1); // ZFFT routine expects input of this length

      const bool zero_DC = global_flags & INVERT_ZERO_DC;

      BOUT_OMP(for nowait)
      for (int ind = 0; ind < nxny; ++ind) { // Loop over X and Y
        // ind = (ix - xs)*(ye - ys + 1) + (iy - ys)
        int ix = xs + ind / ny;
        int iy = ys + ind % ny;

        if (zero_DC) {
          k1d[0] = 0.;
        }

        for (int kz = zero_DC; kz < nmode; kz++)
          k1d[kz] = xcmplx3D((iy - ys) * nmode + kz, ix - xs);

        for (int kz = nmode; kz < localmesh->LocalNz / 2 + 1; kz++)
          k1d[kz] = 0.0; // Filtering out all higher harmonics

        irfft(std::begin(k1d), localmesh->LocalNz, x(ix, iy));
      }
    }
  }

  checkData(x);

  return x;
}


/** 
 * @brief   Initialize local private variables from global input parameters.
 * @param   n Size of global array
 * @param   np_world Number of MPI process
 * @param   rank_world rank ID in MPI_COMM_WORLD
*/
void LaplacePCR :: setup(int n, int np_world, int rank_world)
{
    nprocs = np_world;
    myrank = rank_world;
    n_mpi = n / nprocs;
    //output.write("n_mpi {}\n",n_mpi);
}
/** 
 * @brief   CR-PCR solver: cr_forward_multiple + pcr_forward_single + cr_backward_multiple
 * @param   a_mpi (input) Lower off-diagonal coeff., which is assigned to local private pointer a
 * @param   b_mpi (input) Diagonal coeff., which is assigned to local private pointer b
 * @param   c_mpi (input) Upper off-diagonal coeff.,, which is assigned to local private pointer c
 * @param   r_mpi (input) RHS vector, which is assigned to local private pointer r
 * @param   x_mpi (output) Solution vector, which is assigned to local private pointer x
*/
void LaplacePCR :: cr_pcr_solver(Matrix<dcomplex> &a_mpi, Matrix<dcomplex> &b_mpi, Matrix<dcomplex> &c_mpi, Matrix<dcomplex> &r_mpi, Matrix<dcomplex> &x_mpi)
{

  const int xstart = localmesh->xstart;
  const int xend = localmesh->xend;
  const int nx = xend-xstart+1; // number of interior points
  const int n_all = xe-xs+1; // number of interior points

  // Handle boundary points so that the PCR algorithm works with arrays of
  // the same size on each rank.
  // Note that this modifies the coefficients of b and r in the first and last
  // interior rows. We can continue to use b_mpi and r_mpi arrays directly as
  // their original values are no longer required.
  eliminate_boundary_rows(a_mpi, b_mpi, c_mpi, r_mpi);

  //nsys = nmode * ny;  // Number of systems of equations to solve
  aa.reallocate(nsys, nx+2);
  bb.reallocate(nsys, nx+2);
  cc.reallocate(nsys, nx+2);
  r.reallocate(nsys, nx+2);
  x.reallocate(nsys, nx+2);

  for(int kz=0; kz<nsys; kz++){
    aa(kz,0) = 0;
    bb(kz,0) = 1;
    cc(kz,0) = 0;
    r(kz,0) = 0;
    x(kz,0) = 0;
    for(int ix=0; ix<nx; ix++){
      // The offset xstart - xs ensures that this copies interior points.
      // If a proc has boundary points, these are included in *_mpi, but we
      // don't want to copy them.
      // xs = xstart if a proc has no boundary points
      // xs = 0 if a proc has boundary points
      aa(kz,ix+1) = a_mpi(kz,ix+xstart-xs);
      bb(kz,ix+1) = b_mpi(kz,ix+xstart-xs);
      cc(kz,ix+1) = c_mpi(kz,ix+xstart-xs);
      r(kz,ix+1) = r_mpi(kz,ix+xstart-xs);
      x(kz,ix+1) = x_mpi(kz,ix+xstart-xs);
    }
    aa(kz,nx+1) = 0;
    bb(kz,nx+1) = 1;
    cc(kz,nx+1) = 0;
    r(kz,nx+1) = 0;
    x(kz,nx+1) = 0;
  }

  // Perform parallel cyclic reduction
  cr_forward_multiple_row(aa,bb,cc,r);
  pcr_forward_single_row(aa,bb,cc,r,x);     // Including 2x2 solver
  cr_backward_multiple_row(aa,bb,cc,r,x);
  // End parallel cyclic reduction

  // Copy solution back to bout format - this is correct on interior rows, but
  // not boundary rows
  for(int kz=0; kz<nsys; kz++){
    for(int ix=0; ix<nx; ix++){
      x_mpi(kz,ix+xstart-xs) = x(kz,ix+1);
    }
  }

  // Ensure solution is also correct on boundary rows
  apply_boundary_conditions(a_mpi, b_mpi, c_mpi, r_mpi, x_mpi);

}

/** 
 * Eliminate boundary rows - perform row elimination to uncouple the first and
 * last interior rows from their respective boundary rows. This is necessary
 * to ensure we pass a square system of interior rows to the PCR library.
*/
void LaplacePCR :: eliminate_boundary_rows(const Matrix<dcomplex> &a, Matrix<dcomplex> &b, const Matrix<dcomplex> &c, Matrix<dcomplex> &r) {

  if (localmesh->firstX()) {
    // x index is first interior row
    const int xstart = localmesh->xstart;
    for (int kz = 0; kz < nsys; kz++) {
      b(kz,xstart) = b(kz,xstart) - c(kz, xstart-1) * a(kz,xstart) / b(kz, xstart-1);
      r(kz,xstart) = r(kz,xstart) - r(kz, xstart-1) * a(kz,xstart) / b(kz, xstart-1);
      // Row elimination would set a to zero, but value is unused:
      // a(kz,xstart) = 0.0;
    }
  }
  if (localmesh->lastX()) {
    int n = xe - xs + 1; // actual length of array
    int xind = n - localmesh->xstart - 1;
    for (int kz = 0; kz < nsys; kz++) {
      // x index is last interior row
      b(kz,xind) = b(kz,xind) - c(kz, xind) * a(kz,xind+1) / b(kz, xind+1);
      r(kz,xind) = r(kz,xind) - c(kz, xind) * r(kz,xind+1) / b(kz, xind+1);
      // Row elimination would set c to zero, but value is unused:
      // c(kz,xind) = 0.0;
    }
  }
}

/** 
 * Apply the boundary conditions on the first and last X processors
*/
void LaplacePCR :: apply_boundary_conditions(const Matrix<dcomplex> &a, const Matrix<dcomplex> &b, const Matrix<dcomplex> &c, const Matrix<dcomplex> &r,Matrix<dcomplex> &x) {

  if (localmesh->firstX()) {
    for (int kz = 0; kz < nsys; kz++) {
      for(int ix = localmesh->xstart-1; ix >= 0; ix--){
	x(kz,ix) = (r(kz, ix) - c(kz, ix) * x(kz,ix+1)) / b(kz, ix);
      }
    }
  }
  if (localmesh->lastX()) {
    int n = xe - xs + 1; // actual length of array
    for (int kz = 0; kz < nsys; kz++) {
      for(int ix = n - localmesh->xstart; ix < n; ix++){
	x(kz,ix) = (r(kz,ix) - a(kz, ix) * x(kz,ix-1)) / b(kz, ix);
      }
    }
  }
}

/** 
 * @brief   Forward elimination of CR until a single row per MPI process remains.
 * @details After a single row per MPI process remains, PCR or CR between a single row is performed.
*/
void LaplacePCR :: cr_forward_multiple_row(Matrix<dcomplex> &a,Matrix<dcomplex> &b,Matrix<dcomplex> &c,Matrix<dcomplex> &r)
{
    MPI_Comm comm = BoutComm::get();
    int i, l;
    int nlevel;
    int ip, in, start, dist_row, dist2_row;
    Array<dcomplex> alpha(nsys);
    Array<dcomplex> gamma(nsys);
    Array<dcomplex> sbuf(4*nsys);
    Array<dcomplex> rbuf(4*nsys);

    MPI_Status status, status1;
    Array<MPI_Request> request(2);

   //int nxloc = localmesh->xend-localmesh->xstart+1;
  //output.write("start\n");
///  for(int kz=0;kz<nmode;kz++){
///    for(int ix=0;ix<nxloc+2;ix++){
///      output.write("{} ",a(kz,ix).real());
///    }
///    output.write("\n");
///    for(int ix=0;ix<nxloc+2;ix++){
///      output.write("{} ",b(kz,ix).real());
///    }
///    output.write("\n");
///    for(int ix=0;ix<nxloc+2;ix++){
///      output.write("{} ",c(kz,ix).real());
///    }
///    output.write("\n");
///    for(int ix=0;ix<nxloc+2;ix++){
///      output.write("{} ",r(kz,ix).real());
///    }
///    output.write("\n");
///    for(int ix=0;ix<nxloc+2;ix++){
///      output.write("{} ",x(kz,ix).real());
///    }
///    output.write("\n");
///  }

    /// Variable nlevel is used to indicates when single row remains.
    nlevel    = log2(n_mpi);
    dist_row  = 1;
    dist2_row = 2;
    
    for(l=0;l<nlevel;l++) {
        //output.write("level {}, n_mpi {}, nlevel {}\n",l,n_mpi,nlevel);
        //output.write("myrank {}, nprocs {}\n",myrank,nprocs);
        start = dist2_row;
        /// Data exchange is performed using MPI send/recv for each succesive reduction
        if(myrank<nprocs-1) {
          //output.write("before Irecv\n");
          MPI_Irecv(&rbuf[0], 4*nsys, MPI_DOUBLE_COMPLEX, myrank+1, 0, comm, &request[0]);
          //output.write("after Irecv\n");
        }
        if(myrank>0) {
            //output.write("filling sbuf\n");
            for(int kz=0; kz<nsys; kz++){
              //output.write("myrank {}, kz {}\n",myrank, kz);
              sbuf[0+4*kz] = a(kz,dist_row);
              sbuf[1+4*kz] = b(kz,dist_row);
              sbuf[2+4*kz] = c(kz,dist_row);
              sbuf[3+4*kz] = r(kz,dist_row);
	    }
            //output.write("before isend\n");
            MPI_Isend(&sbuf[0], 4*nsys, MPI_DOUBLE_COMPLEX, myrank-1, 0, comm, &request[1]);
        }
        if(myrank<nprocs-1) {
            //output.write("before wait\n");
            MPI_Wait(&request[0], &status1);
            //output.write("after wait\n");
            for(int kz=0; kz<nsys; kz++){
	      //output.write("myrank {}, kz {}\n",myrank, kz);
              //output.write("a {}\n",a(kz,n_mpi+1).real());
              //output.write("rbuf {}\n",rbuf[0+4*kz].real());
              a(kz,n_mpi+1) = rbuf[0+4*kz];
              b(kz,n_mpi+1) = rbuf[1+4*kz];
              c(kz,n_mpi+1) = rbuf[2+4*kz];
              r(kz,n_mpi+1) = rbuf[3+4*kz];
	    }
        }

        //output.write("after sends\n");


        /// Odd rows of remained rows are reduced to even rows of remained rows in each reduction step.
        /// Index in of global last row is out of range, but we treat it as a = c = r = 0 and b = 1 in main function.
        for(i=start;i<=n_mpi;i+=dist2_row) {
            ip = i - dist_row;
            in = min(i + dist_row, n_mpi + 1);
            for(int kz=0; kz<nsys; kz++){
              alpha[kz] = -a(kz,i) / b(kz,ip);
              gamma[kz] = -c(kz,i) / b(kz,in);

              b(kz,i) += (alpha[kz] * c(kz,ip) + gamma[kz] * a(kz,in));
              a(kz,i) = alpha[kz] * a(kz,ip);
              c(kz,i) = gamma[kz] * c(kz,in);
              r(kz,i) += (alpha[kz] * r(kz,ip) + gamma[kz] * r(kz,in));
	    }

        }
        //output.write("after loop\n");
        /// As reduction continues, the indices of required coefficients doubles.
        dist2_row *= 2;
        dist_row *= 2;
        
        if(myrank>0) {
            //MPI_Wait(request+1, &status);
            MPI_Wait(&request[1], &status);
        }
    }

  //output.write("after loops\n");
///  for(int kz=0;kz<nsys;kz++){
///    for(int ix=0;ix<nxloc+2;ix++){
///      output.write("{} ",a(kz,ix).real());
///    }
///    output.write("\n");
///    for(int ix=0;ix<nxloc+2;ix++){
///      output.write("{} ",b(kz,ix).real());
///    }
///    output.write("\n");
///    for(int ix=0;ix<nxloc+2;ix++){
///      output.write("{} ",c(kz,ix).real());
///    }
///    output.write("\n");
///    for(int ix=0;ix<nxloc+2;ix++){
///      output.write("{} ",r(kz,ix).real());
///    }
///    output.write("\n");
///    for(int ix=0;ix<nxloc+2;ix++){
///      output.write("{} ",x(kz,ix).real());
///    }
///    output.write("\n");
///  }
}

/** 
 * @brief   Backward substitution of CR after single-row solution per MPI process is obtained.
*/
void LaplacePCR :: cr_backward_multiple_row(Matrix<dcomplex> &a,Matrix<dcomplex> &b,Matrix<dcomplex> &c,Matrix<dcomplex> &r,Matrix<dcomplex> &x)
{
    int i, l;
    int nlevel;
    int ip, in, dist_row, dist2_row;
    MPI_Comm comm = BoutComm::get();

    MPI_Status status;
    MPI_Request request[2];
    auto recvvec = Array<dcomplex>(nsys);
    auto sendvec = Array<dcomplex>(nsys);

    nlevel    = log2(n_mpi);
    dist_row = n_mpi/2;

    /// Each rank requires a solution on last row of previous rank.
    if(myrank>0) {
	//output.write("before irecv, myrank {}\n",myrank);
        MPI_Irecv(&recvvec[0], nsys, MPI_DOUBLE_COMPLEX, myrank-1, 100, comm, request);
    }
    if(myrank<nprocs-1) {
	//output.write("before isend\n");
        for(int kz=0; kz<nsys; kz++){
	  sendvec[kz] = x(kz,n_mpi);
	}
        MPI_Isend(&sendvec[0], nsys, MPI_DOUBLE_COMPLEX, myrank+1, 100, comm, request+1);
    }
    if(myrank>0) {
	//output.write("before wait\n");
        MPI_Wait(request, &status);
        for(int kz=0; kz<nsys; kz++){
	  x(kz,0) = recvvec[kz];
	}
	//output.write("after wait\n");
    }
    for(l=nlevel-1;l>=0;l--) {
        dist2_row = dist_row * 2;
        for(i=n_mpi-dist_row;i>=0;i-=dist2_row) {
            ip = i - dist_row;
            in = i + dist_row;
	    for(int kz=0;kz<nsys; kz++){
              x(kz,i) = r(kz,i)-c(kz,i)*x(kz,in)-a(kz,i)*x(kz,ip);
              x(kz,i) = x(kz,i)/b(kz,i);
	    }
        }
        dist_row = dist_row / 2;
    }
    if(myrank<nprocs-1) {
	//output.write("before wait\n");
        MPI_Wait(request+1, &status);
	//output.write("after wait\n");
    }
    //output.write("end part 3\n");
}

/** 
 * @brief   PCR between a single row per MPI process and 2x2 matrix solver between i and i+nprocs/2 rows. 
*/
void LaplacePCR :: pcr_forward_single_row(Matrix<dcomplex> &a,Matrix<dcomplex> &b,Matrix<dcomplex> &c,Matrix<dcomplex> &r,Matrix<dcomplex> &x)
{

    int i, l, nhprocs;
    int nlevel;
    int ip, in, dist_rank, dist2_rank;
    int myrank_level, nprocs_level;
    Array<dcomplex> alpha(nsys);
    Array<dcomplex> gamma(nsys);
    Array<dcomplex> sbuf(4*nsys);
    Array<dcomplex> rbuf0(4*nsys);
    Array<dcomplex> rbuf1(4*nsys);
    dcomplex det;

    MPI_Status status;
    Array<MPI_Request> request(4);
    MPI_Comm comm = BoutComm::get();

    nlevel      = log2(nprocs);
    nhprocs     = nprocs/2;
    dist_rank   = 1;
    dist2_rank  = 2;

    /// Parallel cyclic reduction continues until 2x2 matrix are made between a pair of rank, 
    /// (myrank, myrank+nhprocs).
    for(l=0;l<nlevel-1;l++) {

        /// Rank is newly calculated in each level to find communication pair.
        /// Nprocs is also newly calculated as myrank is changed.
        myrank_level = myrank / dist_rank;
        nprocs_level = nprocs / dist_rank;

        /// All rows exchange data for reduction and perform reduction successively.
        /// Coefficients are updated for every rows.
	for(int kz=0;kz<nsys;kz++){
          sbuf[0+4*kz] = a(kz,n_mpi);
          sbuf[1+4*kz] = b(kz,n_mpi);
          sbuf[2+4*kz] = c(kz,n_mpi);
          sbuf[3+4*kz] = r(kz,n_mpi);
	}

        if((myrank_level+1)%2 == 0) {
            if(myrank+dist_rank<nprocs) {
                MPI_Irecv(&rbuf1[0], 4*nsys, MPI_DOUBLE_COMPLEX, myrank+dist_rank, 202, comm, &request[0]);
                MPI_Isend(&sbuf[0], 4*nsys, MPI_DOUBLE_COMPLEX, myrank+dist_rank, 203, comm, &request[1]);
            }
            if(myrank-dist_rank>=0) {
                MPI_Irecv(&rbuf0[0], 4*nsys, MPI_DOUBLE_COMPLEX, myrank-dist_rank, 200, comm, &request[2]);
                MPI_Isend(&sbuf[0], 4*nsys, MPI_DOUBLE_COMPLEX, myrank-dist_rank, 201, comm, &request[3]);
            }
            if(myrank+dist_rank<nprocs) {
                MPI_Wait(&request[0], &status);
	        for(int kz=0;kz<nsys;kz++){
                  a(kz,n_mpi+1) = rbuf1[0+4*kz];
                  b(kz,n_mpi+1) = rbuf1[1+4*kz];
                  c(kz,n_mpi+1) = rbuf1[2+4*kz];
                  r(kz,n_mpi+1) = rbuf1[3+4*kz];
		}
                MPI_Wait(&request[1], &status);
            }
            if(myrank-dist_rank>=0) {
                MPI_Wait(&request[2], &status);
	        for(int kz=0;kz<nsys;kz++){
                  a(kz,0) = rbuf0[0+4*kz];
                  b(kz,0) = rbuf0[1+4*kz];
                  c(kz,0) = rbuf0[2+4*kz];
                  r(kz,0) = rbuf0[3+4*kz];
		}
                MPI_Wait(&request[3], &status);
            }
        }
        else if((myrank_level+1)%2 == 1) {
            if(myrank+dist_rank<nprocs) {
                MPI_Irecv(&rbuf1[0], 4*nsys, MPI_DOUBLE_COMPLEX, myrank+dist_rank, 201, comm, &request[0]);
                MPI_Isend(&sbuf[0], 4*nsys, MPI_DOUBLE_COMPLEX, myrank+dist_rank, 200, comm, &request[1]);
            }
            if(myrank-dist_rank>=0) {
                MPI_Irecv(&rbuf0[0], 4*nsys, MPI_DOUBLE_COMPLEX, myrank-dist_rank, 203, comm, &request[2]);
                MPI_Isend(&sbuf[0], 4*nsys, MPI_DOUBLE_COMPLEX, myrank-dist_rank, 202, comm, &request[3]);
            }
            if(myrank+dist_rank<nprocs) {
                MPI_Wait(&request[0], &status);
	        for(int kz=0;kz<nsys;kz++){
                  a(kz,n_mpi+1) = rbuf1[0+4*kz];
                  b(kz,n_mpi+1) = rbuf1[1+4*kz];
                  c(kz,n_mpi+1) = rbuf1[2+4*kz];
                  r(kz,n_mpi+1) = rbuf1[3+4*kz];
		}
                MPI_Wait(&request[1], &status);
            }
            if(myrank-dist_rank>=0) {
                MPI_Wait(&request[2], &status);
	        for(int kz=0;kz<nsys;kz++){
                  a(kz,0) = rbuf0[0+4*kz];
                  b(kz,0) = rbuf0[1+4*kz];
                  c(kz,0) = rbuf0[2+4*kz];
                  r(kz,0) = rbuf0[3+4*kz];
		}
                MPI_Wait(&request[3], &status);
            }
        }

        i = n_mpi;
        ip = 0;
        in = i + 1;
        if(myrank_level == 0) {
	  for(int kz=0;kz<nsys;kz++){
            alpha[kz] = 0.0;
	  }
        }
        else {
	  for(int kz=0;kz<nsys;kz++){
            alpha[kz] = -a(kz,i) / b(kz,ip);
	  }
        }
        if(myrank_level == nprocs_level-1) {
	  for(int kz=0;kz<nsys;kz++){
            gamma[kz] = 0.0;
	  }
        }
        else {
	  for(int kz=0;kz<nsys;kz++){
            gamma[kz] = -c(kz,i) / b(kz,in);
	  }
        }

	for(int kz=0;kz<nsys;kz++){
          b(kz,i) += (alpha[kz] * c(kz,ip) + gamma[kz] * a(kz,in));
          a(kz,i)  = alpha[kz] * a(kz,ip);
          c(kz,i)  = gamma[kz] * c(kz,in);
          r(kz,i) += (alpha[kz] * r(kz,ip) + gamma[kz] * r(kz,in));
	}

        dist_rank  *= 2;
        dist2_rank *= 2;
    }

    /// Solving 2x2 matrix. All pair of ranks, myrank and myrank+nhprocs, solves it simultaneously.
    for(int kz=0;kz<nsys;kz++){
      sbuf[0+4*kz] = a(kz,n_mpi);
      sbuf[1+4*kz] = b(kz,n_mpi);
      sbuf[2+4*kz] = c(kz,n_mpi);
      sbuf[3+4*kz] = r(kz,n_mpi);
    }
    if(myrank<nhprocs) {
        MPI_Irecv(&rbuf1[0], 4*nsys, MPI_DOUBLE_COMPLEX, myrank+nhprocs, 300, comm, &request[0]);
        MPI_Isend(&sbuf[0], 4*nsys, MPI_DOUBLE_COMPLEX, myrank+nhprocs, 301, comm, &request[1]);

        MPI_Wait(&request[0], &status);
        for(int kz=0;kz<nsys;kz++){
          a(kz,n_mpi+1) = rbuf1[0+4*kz];
          b(kz,n_mpi+1) = rbuf1[1+4*kz];
          c(kz,n_mpi+1) = rbuf1[2+4*kz];
          r(kz,n_mpi+1) = rbuf1[3+4*kz];
	}

        i = n_mpi;
        in = n_mpi+1;

        for(int kz=0;kz<nsys;kz++){
          det = b(kz,i)*b(kz,in) - c(kz,i)*a(kz,in);
          x(kz,i) = (r(kz,i)*b(kz,in) - r(kz,in)*c(kz,i))/det;
          x(kz,in) = (r(kz,in)*b(kz,i) - r(kz,i)*a(kz,in))/det;
	}
        MPI_Wait(&request[1], &status);

    }
    else if(myrank>=nhprocs) {
        MPI_Irecv(&rbuf0[0], 4*nsys, MPI_DOUBLE_COMPLEX, myrank-nhprocs, 301, comm, &request[2]);
        MPI_Isend(&sbuf[0], 4*nsys, MPI_DOUBLE_COMPLEX, myrank-nhprocs, 300, comm, &request[3]);

        MPI_Wait(&request[2], &status);
        for(int kz=0;kz<nsys;kz++){
          a(kz,0) = rbuf0[0+4*kz];
          b(kz,0) = rbuf0[1+4*kz];
          c(kz,0) = rbuf0[2+4*kz];
          r(kz,0) = rbuf0[3+4*kz];
	}

        ip = 0;
        i = n_mpi;

        for(int kz=0;kz<nsys;kz++){
          det = b(kz,ip)*b(kz,i) - c(kz,ip)*a(kz,i);
          x(kz,ip) = (r(kz,ip)*b(kz,i) - r(kz,i)*c(kz,ip))/det;
          x(kz,i) = (r(kz,i)*b(kz,ip) - r(kz,ip)*a(kz,i))/det;
	}
        MPI_Wait(&request[3], &status);
    }
}

/** 
 * @brief   Solution check
 * @param   *a_ver Coefficients of a with original values
 * @param   *b_ver Coefficients of b with original values
 * @param   *c_ver Coefficients of c with original values
 * @param   *r_ver RHS vector with original values
 * @param   *x_sol Solution vector
*/
void LaplacePCR :: verify_solution(const Matrix<dcomplex> &a_ver, const Matrix<dcomplex> &b_ver, const Matrix<dcomplex> &c_ver, const Matrix<dcomplex> &r_ver, const Matrix<dcomplex> &x_sol)
{
    output.write("Verify solution\n");
    const int xstart = localmesh->xstart;
    const int xend = localmesh->xend;
    const int nx = xe - xs + 1;  // Number of X points on this processor,
                                 // including boundaries but not guard cells
    const int myrank = localmesh->getXProcIndex();
    const int nprocs = localmesh->getNXPE();
    int i;
    Matrix<dcomplex> y_ver(nsys, nx+2);
    Matrix<dcomplex> error(nsys, nx+2);

    MPI_Status status;
    Array<MPI_Request> request(4);
    Array<dcomplex> sbufup(nsys);
    Array<dcomplex> sbufdown(nsys);
    Array<dcomplex> rbufup(nsys);
    Array<dcomplex> rbufdown(nsys);

    //nsys = nmode * ny;  // Number of systems of equations to solve
    Matrix<dcomplex> x_ver(nsys, nx+2);

    for(int kz=0; kz<nsys; kz++){
      for(int ix=0; ix<nx; ix++){
        x_ver(kz,ix+1) = x_sol(kz,ix);
      }
    }
    output.write("after data copy\n");

    if(myrank>0) {
      //output.write("in myrank > 0 \n");
      MPI_Irecv(&rbufdown[0], nsys, MPI_DOUBLE_COMPLEX, myrank-1, 901, MPI_COMM_WORLD, &request[1]);
      for(int kz=0;kz<nsys;kz++){
        sbufdown[kz] = x_ver(kz,1);
      }
      MPI_Isend(&sbufdown[0], nsys, MPI_DOUBLE_COMPLEX, myrank-1, 900, MPI_COMM_WORLD, &request[0]);
      //output.write("in myrank > 0 :: end\n");
    }
    if(myrank<nprocs-1) {
      //output.write("in myrank < nproc - 1 :: start\n");
      MPI_Irecv(&rbufup[0], nsys, MPI_DOUBLE_COMPLEX, myrank+1, 900, MPI_COMM_WORLD, &request[3]);
      for(int kz=0;kz<nsys;kz++){
        sbufup[kz] = x_ver(kz,nx);
      }
      MPI_Isend(&sbufup[0], nsys, MPI_DOUBLE_COMPLEX, myrank+1, 901, MPI_COMM_WORLD, &request[2]);
      //output.write("in myrank < nproc - 1 :: end\n");
    }

    if(myrank>0) {
        //output.write("in myrank > 0 :: before waits\n");
        MPI_Wait(&request[0], &status);
        MPI_Wait(&request[1], &status);
        for(int kz=0;kz<nsys;kz++){
          x_ver(kz,0) = rbufdown[kz];
        }
        //output.write("in myrank > 0 :: after waits\n");
    }
    if(myrank<nprocs-1) {
        //output.write("in myrank < nproc - 1 :: before waits\n");
        MPI_Wait(&request[2], &status);
        MPI_Wait(&request[3], &status);
        for(int kz=0;kz<nsys;kz++){
          x_ver(kz,nx+1) = rbufup[kz];
        }
        //output.write("in myrank < nproc - 1 :: after waits\n");
    }
    
    BoutReal max_error = 0.0;
    for(int kz=0;kz<nsys;kz++){
      for(i=0;i<nx;i++) {
        //output.write("kz {}, i {}\n",kz,i);
        //output.write("myrank = {}\n",myrank);
        //output.write("a={}\n",a_ver(kz,i).real());
        //output.write("b={}\n",b_ver(kz,i).real());
        //output.write("c={}\n",c_ver(kz,i).real());
        //output.write("x={}\n",x(kz,i-1).real());
        //output.write("x={}\n",x(kz,i).real());
        //output.write("x={}\n",x(kz,i+1).real());
        //output.write("r={}\n",r_ver(kz,i).real());
        y_ver(kz,i) = a_ver(kz,i)*x_ver(kz,i)+b_ver(kz,i)*x_ver(kz,i+1)+c_ver(kz,i)*x_ver(kz,i+2);
        error(kz,i) = y_ver(kz,i) - r_ver(kz,i);
	if(abs(error(kz,i)) > max_error){
	  max_error = abs(error(kz,i));
	}
        //output.write("y={}\n",y_ver(kz,i).real());
        output.write("abs error {}, r={}, y={}, kz {}, i {},  a={}, b={}, c={}, x-= {}, x={}, x+ = {}\n",error(kz,i).real(),r_ver(kz,i).real(),y_ver(kz,i).real(),kz,i,a_ver(kz,i).real(),b_ver(kz,i).real(),c_ver(kz,i).real(),x_ver(kz,i).real(),x_ver(kz,i+1).real(),x_ver(kz,i+2).real());
      }
    }
    output.write("max abs error {}\n", max_error);
}
