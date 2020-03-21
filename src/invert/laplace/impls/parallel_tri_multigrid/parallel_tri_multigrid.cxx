/**************************************************************************
 * Perpendicular Laplacian inversion. Serial code using FFT
 * and tridiagonal solver.
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

#include "globals.hxx"
#include "parallel_tri_multigrid.hxx"

#include <bout/mesh.hxx>
#include <boutexception.hxx>
#include <utils.hxx>
#include <fft.hxx>
#include <lapack_routines.hxx>
#include <bout/constants.hxx>
#include <bout/openmpwrap.hxx>
#include <cmath>
#include <bout/sys/timer.hxx>

#include <output.hxx>
#include "boutcomm.hxx"

#include <bout/scorepwrapper.hxx>

LaplaceParallelTriMG::LaplaceParallelTriMG(Options *opt, CELL_LOC loc, Mesh *mesh_in)
    : Laplacian(opt, loc, mesh_in), A(0.0), C(1.0), D(1.0), ipt_mean_its(0.), ncalls(0) {
  A.setLocation(location);
  C.setLocation(location);
  D.setLocation(location);

  OPTION(opt, rtol, 1.e-7);
  OPTION(opt, atol, 1.e-20);
  OPTION(opt, maxits, 100);
  OPTION(opt, new_method, false);
  OPTION(opt, use_previous_timestep, false);

  static int ipt_solver_count = 1;
  bout::globals::dump.addRepeat(ipt_mean_its,
      "ipt_solver"+std::to_string(ipt_solver_count)+"_mean_its");
  ++ipt_solver_count;

  first_call = Matrix<bool>(localmesh->LocalNy,localmesh->LocalNz / 2 + 1);

  //upperGuardVector = Tensor<dcomplex>(localmesh->LocalNx, localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
  //lowerGuardVector = Tensor<dcomplex>(localmesh->LocalNx, localmesh->LocalNy, localmesh->LocalNz / 2 + 1);

///  al = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
///  bl = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
///  au = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
///  bu = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);

///  alold = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
///  blold = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
///  auold = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
///  buold = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);

///  r1 = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
///  r2 = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
///  r3 = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
///  r4 = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
///  r5 = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
///  r6 = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
///  r7 = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
///  r8 = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);

  x0saved = Tensor<dcomplex>(localmesh->LocalNx, localmesh->LocalNy, localmesh->LocalNz / 2 + 1);

  resetSolver();

}

/*
 * Reset the solver to its initial state
 */
void LaplaceParallelTriMG::resetSolver(){
  x0saved = 0.0;
  for(int jy=0; jy<localmesh->LocalNy; jy++){
    for(int kz=0; kz<localmesh->LocalNz / 2 + 1; kz++){
      first_call(jy,kz) = true;
    }
  }
  resetMeanIterations();
}

/*
 * Get an initial guess for the solution x by solving the system neglecting
 * coupling terms. This may be considered a form of preconditioning.
 * Note that coupling terms are not neglected when they are known from the
 * boundary conditions; consequently this gives the exact solution when using
 * two processors.
 */
void LaplaceParallelTriMG::get_initial_guess(const int jy, const int kz, Matrix<dcomplex> &minvb,
					      Tensor<dcomplex> &lowerGuardVector, Tensor<dcomplex> &upperGuardVector,
					      Matrix<dcomplex> &xk1d) {

SCOREP0();

  int xs = localmesh->xstart;
  int xe = localmesh->xend;

  Array<dcomplex> sendvec, recvec;
  sendvec = Array<dcomplex>(2);
  recvec = Array<dcomplex>(2);

  // If not on innermost boundary, get information from neighbouring proc and
  // calculate value of solution in halo cell
  if(!localmesh->firstX()) {

    comm_handle recv[1];
    recv[0] = localmesh->irecvXIn(&recvec[0], 2, 0);

    sendvec[0] = lowerGuardVector(xs,jy,kz);  // element from operator inverse required by neighbour
    sendvec[1] = minvb(kz,xs); // element from RHS required by neighbour
    // If last processor, include known boundary terms
    if(localmesh->lastX()) {
      sendvec[1] += lowerGuardVector(xs,jy,kz)*xk1d(kz,xe+1);
    }

    localmesh->sendXIn(&sendvec[0],2,1);
    localmesh->wait(recv[0]);

    xk1d(kz,xs-1) = ( recvec[1] + recvec[0]*minvb(kz,xs) )/(1.0 - sendvec[0]*recvec[0]);

  }

  // If not on outermost boundary, get information from neighbouring proc and
  // calculate value of solution in halo cell
  if(!localmesh->lastX()) {

    comm_handle recv[1];
    recv[0] = localmesh->irecvXOut(&recvec[0], 2, 1);

    sendvec[0] = upperGuardVector(xe,jy,kz);
    sendvec[1] = minvb(kz,xe);
    // If first processor, include known boundary terms
    if(localmesh->firstX()) {
      sendvec[1] += upperGuardVector(xe,jy,kz)*xk1d(kz,xs-1);
    }

    localmesh->sendXOut(&sendvec[0],2,0);
    localmesh->wait(recv[0]);

    xk1d(kz,xe+1) = ( recvec[1] + recvec[0]*minvb(kz,xe) )/(1.0 - sendvec[0]*recvec[0]);

  }

  for(int i=0; i<localmesh->LocalNx; i++){
    xk1d(kz,i) = minvb(kz,i);
  }
  if(not localmesh->lastX()) {
    for(int i=0; i<localmesh->LocalNx; i++){
      xk1d(kz,i) += upperGuardVector(i,jy,kz)*xk1d(kz,xe+1);
    }
  }
  if(not localmesh->firstX()) {
    for(int i=0; i<localmesh->LocalNx; i++){
      xk1d(kz,i) += lowerGuardVector(i,jy,kz)*xk1d(kz,xs-1);
    }
  }
}

/*
 * Check whether the reduced matrix is diagonally dominant, i.e. whether for every row the absolute
 * value of the diagonal element is greater-or-equal-to the sum of the absolute values
 * of the other elements. Being diagonally dominant is sufficient (but not necessary) for
 * the Jacobi iteration to converge.
 */
bool LaplaceParallelTriMG::is_diagonally_dominant(const dcomplex al, const dcomplex au, const dcomplex bl, const dcomplex bu, const int jy, const int kz) {

  bool is_dd = true;
  if(std::fabs(al)+std::fabs(bl)>1.0){
    output<<BoutComm::rank()<<" jy="<<jy<<", kz="<<kz<<", lower row not diagonally dominant"<<endl;
    is_dd = false;
  }
  if(std::fabs(au)+std::fabs(bu)>1.0){
    output<<BoutComm::rank()<<" jy="<<jy<<", kz="<<kz<<", upper row not diagonally dominant"<<endl;
    is_dd = false;
  }
  return is_dd;
}

/*
 * Calculate the absolute and relative errors at an x grid point.
 */
void LaplaceParallelTriMG::get_errors(Array<BoutReal> &error_rel, Array<BoutReal> &error_abs, const Matrix<dcomplex> x, const Matrix<dcomplex> xlast){

  for(int kz = 0; kz < nmode; kz++){
    error_abs[kz] = abs(x(1,kz) - xlast(1,kz)) + abs(x(2,kz) - xlast(2,kz));
    BoutReal xabs = fabs(x(1,kz));
    if( fabs(x(2,kz)) < xabs ){
      xabs = fabs(x(2,kz));
    }
    if( xabs > 0.0 ){
      error_rel[kz] = error_abs[kz] / xabs;
    }
    else{
      error_rel[kz] = error_abs[kz];
    }
  }
}

bool LaplaceParallelTriMG::all(const Array<bool> a){
  for(int i=0; i<a.size(); i++){
    if(a[i]==false){
      return false;
    }
  }
  return true;
}

bool LaplaceParallelTriMG::any(const Array<bool> a){
  for(int i=0; i<a.size(); i++){
    if(a[i]==true){
      return true;
    }
  }
  return false;
}

FieldPerp LaplaceParallelTriMG::solve(const FieldPerp& b) { return solve(b, b); }

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
//FieldPerp LaplaceParallelTriMG::solve(const FieldPerp& b, const FieldPerp& x0, const FieldPerp& b0) {
FieldPerp LaplaceParallelTriMG::solve(const FieldPerp& b, const FieldPerp& x0) {

  SCOREP0();
  Timer timer("invert"); ///< Start timer

  ///SCOREP_USER_REGION_DEFINE(initvars);
  ///SCOREP_USER_REGION_BEGIN(initvars, "init vars",SCOREP_USER_REGION_TYPE_COMMON);

  ASSERT1(localmesh == b.getMesh() && localmesh == x0.getMesh());
  ASSERT1(b.getLocation() == location);
  ASSERT1(x0.getLocation() == location);
  TRACE("LaplaceParallelTriMG::solve(const, const)");

  FieldPerp x{emptyFrom(b)};
  int xproc = localmesh->getXProcIndex();
  int yproc = localmesh->getYProcIndex();
  int myproc = yproc * localmesh->getNXPE() + xproc;
  proc_in = myproc - 1;
  proc_out = myproc + 1;
  nmode = maxmode + 1;

  Array<Level> levels;
  levels = Array<Level>(2);

  // Calculation variables
  // proc:       p-1   |          p          |       p+1
  // xloc:     xloc[0] | xloc[1]     xloc[2] | xloc[3]    ...
  // In this method, each processor solves equations on its processor
  // interfaces.  Its lower interface equation (for xloc[1]) is coupled to
  // xloc[0] on the processor below and its upper interface variable xloc[2].
  // Its upper interface equation (for xloc[2]) is coupled to its lower
  // interface variable xloc[1] and xloc[3] processor above.
  // We use these local variables rather than calculate with xk1d directly
  // as the elements of xloc can refer to different elements of xk1d depending
  // on the method used.
  // For example, in the original iteration we have:
  // xloc[0] = xk1d[xstart-1], xloc[1] = xk1d[xstart],
  // xloc[2] = xk1d[xend],     xloc[3] = xk1d[xend+1],
  // but if this is found to be unstable, he must change this to
  // xloc[0] = xk1d[xstart], xloc[1] = xk1d[xstart-1],
  // xloc[2] = xk1d[xend+1], xloc[3] = xk1d[xend].
  // It is easier to change the meaning of local variables and keep the
  // structure of the calculation/communication than it is to change the
  // indexing of xk1d to cover all possible cases.
  //
  auto xloc = Matrix<dcomplex>(4,nmode);
  auto xloclast = Matrix<dcomplex>(4,nmode);
  auto rl = Array<dcomplex>(nmode);
  auto ru = Array<dcomplex>(nmode);
  auto rlold = Array<dcomplex>(nmode);
  auto ruold = Array<dcomplex>(nmode);

  // Convergence flags
  auto self_in = Array<bool>(nmode);
  auto self_out = Array<bool>(nmode);
  auto neighbour_in = Array<bool>(nmode);
  auto neighbour_out = Array<bool>(nmode);

  int jy = b.getIndex();

  int ncz = localmesh->LocalNz; // Number of local z points
  int ncx = localmesh->LocalNx; // Number of local x points

  int xs = localmesh->xstart;
  int xe = localmesh->xend;

  BoutReal kwaveFactor = 2.0 * PI / coords->zlength();

  // Should we store coefficients?
  store_coefficients = not (inner_boundary_flags & INVERT_AC_GRAD);
  store_coefficients = store_coefficients && not (outer_boundary_flags & INVERT_AC_GRAD);
  store_coefficients = store_coefficients && not (inner_boundary_flags & INVERT_SET);
  store_coefficients = store_coefficients && not (outer_boundary_flags & INVERT_SET);
  //store_coefficients = false;

  // Setting the width of the boundary.
  // NOTE: The default is a width of 2 guard cells
  int inbndry = localmesh->xstart, outbndry=localmesh->xstart;

  // If the flags to assign that only one guard cell should be used is set
  if((global_flags & INVERT_BOTH_BNDRY_ONE) || (localmesh->xstart < 2))  {
    inbndry = outbndry = 1;
  }
  if (inner_boundary_flags & INVERT_BNDRY_ONE)
    inbndry = 1;
  if (outer_boundary_flags & INVERT_BNDRY_ONE)
    outbndry = 1;

  /* Allocation fo
  * bk   = The fourier transformed of b, where b is one of the inputs in
  *        LaplaceParallelTriMG::solve()
  * bk1d = The 1d array of bk
  * xk   = The fourier transformed of x, where x the output of
  *        LaplaceParallelTriMG::solve()
  * xk1d = The 1d array of xk
  */
  auto bk = Matrix<dcomplex>(ncx, ncz / 2 + 1);
  auto bk1d = Array<dcomplex>(ncx);
  auto xk = Matrix<dcomplex>(ncx, ncz / 2 + 1);
  auto xk1d = Matrix<dcomplex>(ncz/2+1,ncx);
  auto xk1dlast = Matrix<dcomplex>(ncz/2+1,ncx);
  auto error_rel = Array<BoutReal>(ncz/2+1);
  auto error_abs = Array<BoutReal>(ncz/2+1);
  /*
  // Down and up coefficients
  dcomplex Bd, Ad;
  dcomplex Bu, Au;
  dcomplex Btmp, Atmp;
  auto Rd = Array<dcomplex>(ncz/2+1);
  auto Ru = Array<dcomplex>(ncz/2+1);
  auto Rsendup = Array<dcomplex>(ncz+2);
  auto Rsenddown = Array<dcomplex>(ncz+2);
  auto Rrecvup = Array<dcomplex>(ncz+2);
  auto Rrecvdown = Array<dcomplex>(ncz+2);
  */

  // Define indexing of xloc that depends on method. Doing this now removes
  // branch in tight loops
  index_in = 1;
  index_out = 2;
  if(new_method){
    index_in = 2;
    index_out = 1;
  }

  ///SCOREP_USER_REGION_END(initvars);
  ///SCOREP_USER_REGION_DEFINE(initloop);
  ///SCOREP_USER_REGION_BEGIN(initloop, "init xk loop",SCOREP_USER_REGION_TYPE_COMMON);

  // Initialise xk to 0 as we only visit 0<= kz <= maxmode in solve
  for (int ix = 0; ix < ncx; ix++) {
    for (int kz = maxmode + 1; kz < ncz / 2 + 1; kz++) {
      xk(ix, kz) = 0.0;
    }
  }
  ///SCOREP_USER_REGION_END(initloop);
  ///SCOREP_USER_REGION_DEFINE(fftloop);
  ///SCOREP_USER_REGION_BEGIN(fftloop, "init fft loop",SCOREP_USER_REGION_TYPE_COMMON);

  /* Coefficents in the tridiagonal solver matrix
  * Following the notation in "Numerical recipes"
  * avec is the lower diagonal of the matrix
  * bvec is the diagonal of the matrix
  * cvec is the upper diagonal of the matrix
  * NOTE: Do not confuse avec, bvec and cvec with the A, C, and D coefficients
  *       above
  */
  auto avec = Matrix<dcomplex>(nmode,ncx);
  auto bvec = Matrix<dcomplex>(nmode,ncx);
  auto cvec = Matrix<dcomplex>(nmode,ncx);
  auto bcmplx = Matrix<dcomplex>(nmode,ncx);

  BOUT_OMP(parallel for)
  for (int ix = 0; ix < ncx; ix++) {
    /* This for loop will set the bk (initialized by the constructor)
    * bk is the z fourier modes of b in z
    * If the INVERT_SET flag is set (meaning that x0 will be used to set the
    * bounadry values),
    */
    if (((ix < inbndry) && (inner_boundary_flags & INVERT_SET) && localmesh->firstX()) ||
    ((ncx - ix - 1 < outbndry) && (outer_boundary_flags & INVERT_SET) &&
    localmesh->lastX())) {
      // Use the values in x0 in the boundary

      // x0 is the input
      // bk is the output
      rfft(x0[ix], ncz, &bk(ix, 0));

    } else {
      // b is the input
      // bk is the output
      rfft(b[ix], ncz, &bk(ix, 0));
      //rfft(x0[ix], ncz, &xk(ix, 0));
    }
  }
  ///SCOREP_USER_REGION_END(fftloop);

  /* Solve differential equation in x for each fourier mode
  * Note that only the non-degenerate fourier modes are being used (i.e. the
  * offset and all the modes up to the Nyquist frequency)
  */
  for (int kz = 0; kz <= maxmode; kz++) {
    ///SCOREP_USER_REGION_DEFINE(kzinit);
    ///SCOREP_USER_REGION_BEGIN(kzinit, "kz init",SCOREP_USER_REGION_TYPE_COMMON);

    // set bk1d
    for (int ix = 0; ix < ncx; ix++) {
      // Get bk of the current fourier mode
      bcmplx(kz,ix) = bk(ix, kz);
      xk1d(kz,ix) = x0saved(ix, jy, kz);
      xk1dlast(kz,ix) = x0saved(ix, jy, kz);
    }

    // Set all convergence flags to false
    self_in[kz] = false;
    self_out[kz] = false;
    neighbour_in[kz] = false;
    neighbour_out[kz] = false;

    // Boundary values are "converged" at the start
    // Note: set neighbour's flag (not self's flag) to ensure we do at least
    // one iteration
    if(localmesh->lastX()) { 
      neighbour_out[kz] = true;
    }
    if(localmesh->firstX()) { 
      neighbour_in[kz] = true;
    }

    /* Set the matrix A used in the inversion of Ax=b
    * by calling tridagCoef and setting the BC
    *
    * Note that A, C and D in
    *
    * D*Laplace_perp(x) + (1/C)Grad_perp(C)*Grad_perp(x) + Ax = B
    *
    * has nothing to do with
    * avec - the lower diagonal of the tridiagonal matrix
    * bvec - the main diagonal
    * cvec - the upper diagonal
    */
    tridagMatrix(&avec(kz,0), &bvec(kz,0), &cvec(kz,0), &bcmplx(kz,0),
    jy,
    // wave number index
    kz,
    // wave number (different from kz only if we are taking a part
    // of the z-domain [and not from 0 to 2*pi])
    kz * kwaveFactor, global_flags, inner_boundary_flags,
    outer_boundary_flags, &A, &C, &D);

    // Patch up internal boundaries
    if(not localmesh->lastX()) { 
      for(int ix = localmesh->xend+1; ix<localmesh->LocalNx ; ix++) {
	avec(kz,ix) = 0;
	bvec(kz,ix) = 1;
	cvec(kz,ix) = 0;
	bcmplx(kz,ix) = 0;
      }
    } 
    if(not localmesh->firstX()) { 
      for(int ix = 0; ix<localmesh->xstart ; ix++) {
	avec(kz,ix) = 0;
	bvec(kz,ix) = 1;
	cvec(kz,ix) = 0;
	bcmplx(kz,ix) = 0;
      }
    }
  }

  output<<"Level 0"<<endl;
  for(int ix = 0; ix<ncx ; ix++) {
    int kz=0;
    if(kz==0){
      output<<avec(kz,ix)<<" "<<bvec(kz,ix)<<" "<<cvec(kz,ix)<<" "<<bcmplx(kz,ix)<<endl;
    }
  }
  output<<"Level 1"<<endl;

  levels[0].xs = xs;
  levels[0].xe = xe;
  init(levels[0], ncx, jy, avec, bvec, cvec, bcmplx);

  int ncx_coarse = (xe-xs+1)/2 + xs + ncx - xe - 1;
  levels[1].xs = xs;
  levels[1].xe = ncx_coarse - 2;  //FIXME assumes mgy=2
  auto atmp = Matrix<dcomplex>(nmode,ncx_coarse);
  auto btmp = Matrix<dcomplex>(nmode,ncx_coarse);
  auto ctmp = Matrix<dcomplex>(nmode,ncx_coarse);
  auto rtmp = Matrix<dcomplex>(nmode,ncx_coarse);
  for(int kz = 0; kz < nmode; kz++){
    for(int ix = 0; ix<xs; ix++){
      // Lower end always agrees with finer grid in guard cells
      atmp(kz,ix) = avec(kz,ix);
      btmp(kz,ix) = bvec(kz,ix);
      ctmp(kz,ix) = cvec(kz,ix);
      rtmp(kz,ix) = bcmplx(kz,ix);
      if(kz==0){
	output<<atmp(kz,ix)<<" "<<btmp(kz,ix)<<" "<<ctmp(kz,ix)<<" "<<rtmp(kz,ix)<<endl;
      }
    }
    for(int ix = xs; ix<ncx_coarse - (ncx-xe)+1; ix++){
      if(localmesh->firstX() and ix == xs){
	// No lumping in avec for first interior point:
	// The gap between this point and the first guard cell is NOT doubled when the mesh is refined. 
	atmp(kz,ix) = avec(kz, 2*(ix-xs)+xs);
	ctmp(kz,ix) = cvec(kz, 2*(ix-xs)+xs)/2.0;
	btmp(kz,ix) = bvec(kz, 2*(ix-xs)+xs) + ctmp(kz,ix);
	rtmp(kz,ix) = bcmplx(kz, 2*(ix-xs)+xs);
      }
      else{
	atmp(kz,ix) = avec(kz, 2*(ix-xs)+xs)/2.0;
	ctmp(kz,ix) = cvec(kz, 2*(ix-xs)+xs)/2.0;
	btmp(kz,ix) = bvec(kz, 2*(ix-xs)+xs) + atmp(kz,ix) + ctmp(kz,ix);
	rtmp(kz,ix) = bcmplx(kz, 2*(ix-xs)+xs);
      }
      if(kz==0){
	output<<atmp(kz,ix)<<" "<<btmp(kz,ix)<<" "<<ctmp(kz,ix)<<" "<<rtmp(kz,ix)<<endl;
      }
    }
    for(int ix = ncx_coarse - (ncx-xe)+1; ix<ncx_coarse; ix++){
      if( localmesh->lastX() and ix == ncx_coarse - (ncx-xe)+1){
	// Lump avec on first physical boundary point:
	// The grid spacing has been doubled here
	atmp(kz,ix) = 0.5*avec(kz,ix+ncx-ncx_coarse);
	btmp(kz,ix) = atmp(kz,ix) + bvec(kz,ix+ncx-ncx_coarse);
	ctmp(kz,ix) = cvec(kz,ix+ncx-ncx_coarse);
	rtmp(kz,ix) = bcmplx(kz,ix+ncx-ncx_coarse);
      }
      else{
	atmp(kz,ix) = avec(kz,ix+ncx-ncx_coarse);
	btmp(kz,ix) = bvec(kz,ix+ncx-ncx_coarse);
	ctmp(kz,ix) = cvec(kz,ix+ncx-ncx_coarse);
	rtmp(kz,ix) = bcmplx(kz,ix+ncx-ncx_coarse);
      }
      if(kz==0){
	output<<atmp(kz,ix)<<" "<<btmp(kz,ix)<<" "<<ctmp(kz,ix)<<" "<<rtmp(kz,ix)<<endl;
      }
    }
  }

  init(levels[1], ncx_coarse, jy, atmp, btmp, ctmp, rtmp);

  for (int kz = 0; kz <= maxmode; kz++) {
    //if( first_call(jy,kz) or not use_previous_timestep ){
    get_initial_guess(jy,kz,levels[0].minvb,levels[0].lowerGuardVector,levels[0].upperGuardVector,xk1d);
    //}

    // Original method:
    xloclast(0,kz) = xk1d(kz,xs-1);
    xloclast(1,kz) = xk1d(kz,xs);
    xloclast(2,kz) = xk1d(kz,xe);
    xloclast(3,kz) = xk1d(kz,xe+1);
    // Without this, xloc(0,kz), xloc(3,kz) never set correctly on boundary processors
    xloc(0,kz) = xk1d(kz,xs-1);
    xloc(1,kz) = xk1d(kz,xs);
    xloc(2,kz) = xk1d(kz,xe);
    xloc(3,kz) = xk1d(kz,xe+1);
    //output<<"initial "<<xloclast(0,kz)<<" "<<xloclast(1,kz)<<" "<<xloclast(2,kz)<<" "<<xloclast(3,kz)<<endl;

  }

  ///SCOREP_USER_REGION_DEFINE(whileloop);
  ///SCOREP_USER_REGION_BEGIN(whileloop, "while loop",SCOREP_USER_REGION_TYPE_COMMON);

  int count = 0;
  int subcount = 0;
  int current_level = 0;
  while(true){

    int kz=0;
    //jacobi(levels[current_level], jy, ncx, xloc, xloclast, error_rel, error_abs );
    jacobi_full_system(levels[current_level], jy, ncx, xk1d, xk1dlast, error_rel, error_abs );
    //output<<count<<" "<<current_level<<" "<<error_rel[0]<<" "<<error_abs[0]<<" "<<xloc(0,kz)<<" "<<xloc(1,kz)<<" "<<xloc(2,kz)<<" "<<xloc(3,kz)<<endl;
    output<<count<<" "<<current_level<<" "<<error_rel[0]<<" "<<error_abs[0]<<" "<<xk1d(kz,0)<<" "<<xk1d(kz,1)<<" "<<xk1d(kz,2)<<" "<<xk1d(kz,3)<<" "<<xk1d(kz,4)<<" "<<xk1d(kz,5)<<" "<<xk1d(kz,6)<<" "<<xk1d(kz,7)<<endl;
    //
    if(subcount > 9993){
      if(current_level==0){
	if(kz==0){
      for(int i=0; i<ncx; i++){
	xk1d(kz,i) = levels[0].minvb(kz,i);
      }
      if(!localmesh->lastX()){
	for(int i=0; i<ncx; i++){
	  xk1d(kz,i) += levels[0].upperGuardVector(i,jy,kz)*xloclast(3,kz);
	}
      }
      if(!localmesh->firstX()){
	for(int i=0; i<ncx; i++){
	  xk1d(kz,i) += levels[0].lowerGuardVector(i,jy,kz)*xloclast(0,kz);
	}
      }
      for(int i=0; i<ncx; i++){
	output<<i<<" "<<xk1d(kz,i)<<endl;
      }
	}
	output<<"coarsen!"<<endl;
	// Coarsening requires data from the grid BEFORE it is made coarser
	coarsen(levels[current_level],xloc,xloclast,jy);
	current_level = 1;
	output<<"xloc "<<count<<" "<<current_level<<" "<<error_rel[0]<<" "<<error_abs[0]<<" "<<xloc(0,kz)<<" "<<xloc(1,kz)<<" "<<xloc(2,kz)<<" "<<xloc(3,kz)<<endl;
	output<<"xloclast "<<count<<" "<<current_level<<" "<<error_rel[0]<<" "<<error_abs[0]<<" "<<xloclast(0,kz)<<" "<<xloclast(1,kz)<<" "<<xloclast(2,kz)<<" "<<xloclast(3,kz)<<endl;
      }
      else{
	current_level = 0;
	refine(xloc,xloclast);
	output<<"refine!"<<endl;
	output<<"xloc "<<count<<" "<<current_level<<" "<<error_rel[0]<<" "<<error_abs[0]<<" "<<xloc(0,kz)<<" "<<xloc(1,kz)<<" "<<xloc(2,kz)<<" "<<xloc(3,kz)<<endl;
	output<<"xloclast "<<count<<" "<<current_level<<" "<<error_rel[0]<<" "<<error_abs[0]<<" "<<xloclast(0,kz)<<" "<<xloclast(1,kz)<<" "<<xloclast(2,kz)<<" "<<xloclast(3,kz)<<endl;
      }
      subcount=0;
    }

    ++count;
    ++subcount;
    ///SCOREP_USER_REGION_END(comms_after_break);
    if (count>maxits) {
      break;
      /*
      // Maximum number of allowed iterations reached.
      // If the iteration matrix is diagonally-dominant, then convergence is
      // guaranteed, so maxits is set too low.
      // Otherwise, the method may or may not converge.
      for (int kz = 0; kz <= maxmode; kz++) {
	if(!is_diagonally_dominant(al(jy,kz),au(jy,kz),bl(jy,kz),bu(jy,kz),jy,kz)){
	  throw BoutException("LaplaceParallelTriMG error: Not converged within maxits=%i iterations. The iteration matrix is not diagonally dominant on processor %i, so there is no guarantee this method will converge. Consider increasing maxits or using a different solver.",maxits,BoutComm::rank());
	}
      }
      throw BoutException("LaplaceParallelTriMG error: Not converged within maxits=%i iterations. The iteration matrix is diagonally dominant on processor %i and convergence is guaranteed (if all processors are diagonally dominant). Please increase maxits and retry.",maxits,BoutComm::rank());
      //output<<alold(jy,kz)<<" "<<blold(jy,kz)<<" "<<auold(jy,kz)<<" "<<buold(jy,kz)<<endl;
      //output<<al(jy,kz)<<" "<<bl(jy,kz)<<" "<<au(jy,kz)<<" "<<bu(jy,kz)<<endl;
      //output<<Ad<<" "<<Bd<<" "<<Au<<" "<<Bu<<endl;
      */
    }

    ///SCOREP_USER_REGION_DEFINE(copylast);
    ///SCOREP_USER_REGION_BEGIN(copylast, "copy to last",SCOREP_USER_REGION_TYPE_COMMON);
    //output<<"xloc "<<maxmode<<" "<<kz<<" "<<xloc(kz,0)<<" "<<xloc(kz,1)<<" "<<xloc(kz,2)<<" "<<xloc(kz,3)<<endl;
    //output<<"xloclast "<<kz<<" "<<xloclast(kz,0)<<" "<<xloclast(kz,1)<<" "<<xloclast(kz,2)<<" "<<xloclast(kz,3)<<endl;
    for (int kz = 0; kz <= maxmode; kz++) {
      //if(!(self_in[kz] and self_out[kz])){
        for (int ix = 0; ix < 4; ix++) {
	  xloclast(ix,kz) = xloc(ix,kz);
        }
        for (int ix = 0; ix < ncx; ix++) {
	  xk1dlast(kz,ix) = xk1d(kz,ix);
        }
      //}
    }
    ///SCOREP_USER_REGION_END(copylast);

  }
//  output<<"after iteration"<<endl;
  ///SCOREP_USER_REGION_END(whileloop);

  //throw BoutException("LaplaceParallelTriMG error: periodic boundary conditions not supported");

  ///SCOREP_USER_REGION_DEFINE(afterloop);
  ///SCOREP_USER_REGION_BEGIN(afterloop, "after faff",SCOREP_USER_REGION_TYPE_COMMON);
  ++ncalls;
  ipt_mean_its = (ipt_mean_its * BoutReal(ncalls-1)
  + BoutReal(count))/BoutReal(ncalls);

  for (int kz = 0; kz <= maxmode; kz++) {
    // Original method:
    xk1d(kz,xs-1) = xloc(0,kz);
    xk1d(kz,xs)   = xloc(1,kz);
    xk1dlast(kz,xs-1) = xloclast(0,kz);
    xk1dlast(kz,xs)   = xloclast(1,kz);
    xk1d(kz,xe)   = xloc(2,kz);
    xk1d(kz,xe+1) = xloc(3,kz);
    xk1dlast(kz,xe)   = xloclast(2,kz);
    xk1dlast(kz,xe+1) = xloclast(3,kz);

        for (int ix = 0; ix < 4; ix++) {
	  output<<"final "<<ix<<" "<<xloclast(ix,0) <<" "<< xloc(ix,0)<<endl;
        }

    /*
    if(new_method){
      dcomplex d = 1.0/(buold(jy,kz)*alold(jy,kz) - blold(jy,kz)*auold(jy,kz));
      // If boundary processor, halo cell is already correct, and d is undefined.
      // Lower boundary proc => al = au = 0
      // Upper boundary proc => bl = bu = 0
      if(not localmesh->firstX() and not localmesh->lastX()){
	// General case
	xk1dlast(kz,xs-1) =  d*(buold(jy,kz)*(xk1dlast(kz,xs)-rlold[kz]) - blold(jy,kz)*(xk1dlast(kz,xe)-ruold[kz]));
	xk1dlast(kz,xe+1) = -d*(auold(jy,kz)*(xk1dlast(kz,xs)-rlold[kz]) - alold(jy,kz)*(xk1dlast(kz,xe)-ruold[kz]));
      } else if(localmesh->firstX() and not localmesh->lastX()) {
	// Lower boundary but not upper boundary
	// xk1dlast[xs-1] = already correct
	xk1dlast(kz,xe+1) = (xk1dlast(kz,xe)-ruold[kz])/buold(jy,kz);
      } else if(localmesh->lastX() and not localmesh->firstX()){
	// Upper boundary but not lower boundary
	// xk1dlast[xe+1] = already correct
	xk1dlast(kz,xs-1) = (xk1dlast(kz,xs)-rlold[kz])/alold(jy,kz);
      } 
      // No "else" case. If both upper and lower boundaries, both xs-1 and xe+1
      // are already correct
    }
    */

    // Now that halo cells are converged, use these to calculate whole solution
    for(int i=0; i<ncx; i++){
      xk1d(kz,i) = levels[0].minvb(kz,i);
    }
    if(not localmesh->lastX()) { 
      for(int i=0; i<ncx; i++){
	xk1d(kz,i) += levels[0].upperGuardVector(i,jy,kz)*xk1dlast(kz,xe+1);
      }
    }
    if(not localmesh->firstX()) { 
      for(int i=0; i<ncx; i++){
	xk1d(kz,i) += levels[0].lowerGuardVector(i,jy,kz)*xk1dlast(kz,xs-1);
      }
    } 

        for (int ix = 0; ix < ncx; ix++) {
	  output<<"xk1d "<<ix<<" "<<xk1d(0,ix) <<" "<< xk1d(0,ix)<<endl;
        }

    // If the global flag is set to INVERT_KX_ZERO
    if ((global_flags & INVERT_KX_ZERO) && (kz == 0)) {
      dcomplex offset(0.0);
      for (int ix = localmesh->xstart; ix <= localmesh->xend; ix++) {
	offset += xk1d(kz,ix);
      }
      offset /= static_cast<BoutReal>(localmesh->xend - localmesh->xstart + 1);
      for (int ix = localmesh->xstart; ix <= localmesh->xend; ix++) {
	xk1d(kz,ix) -= offset;
      }
    }

    // Store the solution xk for the current fourier mode in a 2D array
    for (int ix = 0; ix < ncx; ix++) {
      xk(ix, kz) = xk1d(kz,ix);
    }
    first_call(jy,kz) = false;
  }
//  output<<"after loop"<<endl;
  ///SCOREP_USER_REGION_END(afterloop);

  ///SCOREP_USER_REGION_DEFINE(fftback);
  ///SCOREP_USER_REGION_BEGIN(fftback, "fft back",SCOREP_USER_REGION_TYPE_COMMON);
  // Done inversion, transform back
  for (int ix = 0; ix < ncx; ix++) {

    if(global_flags & INVERT_ZERO_DC)
      xk(ix, 0) = 0.0;

    irfft(&xk(ix, 0), ncz, x[ix]);

#if CHECK > 2
    for(int kz=0;kz<ncz;kz++)
      if(!finite(x(ix,kz)))
	throw BoutException("Non-finite at %d, %d, %d", ix, jy, kz);
#endif
  }
//  output<<"end"<<endl;
  ///SCOREP_USER_REGION_END(fftback);
  return x; // Result of the inversion
}

void LaplaceParallelTriMG::jacobi(Level &l, const int jy, const int ncx, Matrix<dcomplex> &xloc, Matrix<dcomplex> &xloclast, Array<BoutReal> &error_rel, Array<BoutReal> &error_abs){

  struct Message { dcomplex value; bool done; };
  Array<Message> message_send, message_recv;
  message_send = Array<Message>(nmode);
  message_recv = Array<Message>(nmode);
  MPI_Comm comm = BoutComm::get();
  int err;

  // Only need to update interior points
  for (int kz = 0; kz <= maxmode; kz++) {
    //if(not(self_in[kz] and self_out[kz])){
    // TODO guard work for converged kz
    if( localmesh->firstX() ){
      xloc(0,kz) = ( l.minvb(kz,l.xs-1) + l.upperGuardVector(l.xs-1,jy,kz)*xloclast(3,kz) ) / (1.0 - l.lowerGuardVector(l.xs-1,jy,kz));
    }
    xloc(1,kz) = l.rl[kz] + l.al(jy,kz)*xloclast(0,kz) + l.bl(jy,kz)*xloclast(3,kz);
    xloc(2,kz) = l.ru[kz] + l.au(jy,kz)*xloclast(0,kz) + l.bu(jy,kz)*xloclast(3,kz);
    if( localmesh->lastX() ){
      xloc(3,kz) = ( l.minvb(kz,l.xe+2) + l.lowerGuardVector(l.xe+2,jy,kz)*xloclast(0,kz) ) / (1.0 - l.upperGuardVector(l.xe+2,jy,kz));
    }

///        // Set communication flags
///        if ( count > 0 && (
///          ((error_rel_lower<rtol or error_abs_lower<atol) and
///          (error_rel_upper<rtol or error_abs_upper<atol) ))) {
///	  // In the next iteration this proc informs its neighbours that its halo cells
///	  // will no longer be updated, then breaks.
///	  self_in[kz] = true;
///	  self_out[kz] = true;
///        }
///        ///SCOREP_USER_REGION_END(errors);
///      }
  }

  // Calcalate errors on interior points only
  get_errors(error_rel,error_abs,xloc,xloclast);

    //output<<"after work jy, count "<<jy<<" "<<count<<endl;
    ///SCOREP_USER_REGION_END(workanderror);
    ///SCOREP_USER_REGION_DEFINE(comms);
    ///SCOREP_USER_REGION_BEGIN(comms, "communication",SCOREP_USER_REGION_TYPE_COMMON);

    // Communication
    // A proc is finished when it is both in- and out-converged.
    // Once this happens, that processor communicates once, then breaks.
    //
    // A proc can be converged in only one of the directions. This happens
    // if it has not met the error tolerance, but one of its neighbours has
    // converged. In this case, that boundary will no longer update (and
    // communication in that direction should stop), but the other boundary
    // may still be changing.
    //
    // There are four values to consider:
    //   neighbour_in  = whether my in-neighbouring proc has out-converged
    //   self_in       = whether this proc has in-converged
    //   self_out      = whether this proc has out-converged
    //   neighbour_out = whether my out-neighbouring proc has in-converged
    //
    // If neighbour_in = true, I must have been told this by my neighbour. My
    // neighbour has therefore done its one post-converged communication. My in-boundary
    // values are therefore correct, and I am in-converged. My neighbour is not
    // expecting us to communicate.
    //
    // Communicate in
//    if(count>422 and count<428){
//      for(int kz=0; kz<3;kz++){
//	output<<"before "<<kz<<" "<<neighbour_in[kz]<<" "<<self_in[kz]<<" "<<self_out[kz]<<" "<<neighbour_out[kz]<<endl;
//      } 
//    }
//    TODO Guard comms
///    if(!all(neighbour_in)) {
      //output<<"neighbour_in proc "<<BoutComm::rank()<<endl;

      // TODO These for loops do buffer (un)packing for data we don't care about
      // Guard? Or move to work loop?
      if(!localmesh->firstX()){
      for (int kz = 0; kz <= maxmode; kz++) {
///	if(!neighbour_in[kz]){
	  message_send[kz].value = xloc(index_in,kz);
///	  message_send[kz].done  = self_in[kz];
///	}
      }
      err = MPI_Sendrecv(&message_send[0], nmode*sizeof(Message), MPI_BYTE, proc_in, 1, &message_recv[0], nmode*sizeof(Message), MPI_BYTE, proc_in, 0, comm, MPI_STATUS_IGNORE);
      for (int kz = 0; kz <= maxmode; kz++) {
///	if(!self_in[kz]){
	  xloc(0,kz) = message_recv[kz].value;
///	  neighbour_in[kz] = message_recv[kz].done;
///	}
      }
///    }
}

    // Communicate out
    // See note above for inward communication.
//    TODO Guard comms
///    if(!all(neighbour_out)) {
      //output<<"neighbour_out proc "<<BoutComm::rank()<<endl;
      if(!localmesh->lastX()){
      for (int kz = 0; kz <= maxmode; kz++) {
	message_send[kz].value = xloc(index_out,kz);
///	message_send[kz].done  = self_out[kz];
      }
      err = MPI_Sendrecv(&message_send[0], nmode*sizeof(Message), MPI_BYTE, proc_out, 0, &message_recv[0], nmode*sizeof(Message), MPI_BYTE, proc_out, 1, comm, MPI_STATUS_IGNORE);
      for (int kz = 0; kz < nmode; kz++) {
	xloc(3,kz) = message_recv[kz].value;
///	neighbour_out[kz] = message_recv[kz].done;
      }
      }
///    }
    ///SCOREP_USER_REGION_END(comms);

    // Now I've done my communication, exit if I am both in- and out-converged
///    if( all(self_in) and all(self_out) ) {
///      break;
///    }
    ///SCOREP_USER_REGION_DEFINE(comms_after_break);
    ///SCOREP_USER_REGION_BEGIN(comms_after_break, "comms after break",SCOREP_USER_REGION_TYPE_COMMON);

    // If my neighbour has converged, I know that I am also converged on that
    // boundary. Set this flag after the break loop above, to ensure we do one
    // iteration using our neighbour's converged value.
///    for (int kz = 0; kz <= maxmode; kz++) {
///      self_in[kz] = neighbour_in[kz] = (self_in[kz] or neighbour_in[kz]);
///      self_out[kz] = neighbour_out[kz] = (self_out[kz] or neighbour_out[kz]);
///    }
//

}  

// Perform a Jacobi iteration explicitly on the full system 
void LaplaceParallelTriMG::jacobi_full_system(Level &l, const int jy, const int ncx, Matrix<dcomplex> &xk1d, Matrix<dcomplex> &xk1dlast, Array<BoutReal> &error_rel, Array<BoutReal> &error_abs){

  struct Message { dcomplex value; bool done; };
  Array<Message> message_send, message_recv;
  message_send = Array<Message>(nmode);
  message_recv = Array<Message>(nmode);
  MPI_Comm comm = BoutComm::get();
  int err;

  for (int kz = 0; kz <= maxmode; kz++) {
    // TODO guard work for converged kz
    for (int ix = 1; ix < ncx-1; ix++) {
      xk1d(kz,ix) = ( l.rvec(kz,ix) - l.avec(kz,ix)*xk1dlast(kz,ix-1) - l.cvec(kz,ix)*xk1dlast(kz,ix+1) ) / l.bvec(kz,ix);
    }
  }

  // Calcalate errors on interior points only
  get_errors(error_rel,error_abs,xk1d,xk1dlast);

    if(!localmesh->firstX()){
      for (int kz = 0; kz <= maxmode; kz++) {
///	if(!neighbour_in[kz]){
	  message_send[kz].value = xk1d(kz,l.xs);
///	  message_send[kz].done  = self_in[kz];
///	}
      }
      err = MPI_Sendrecv(&message_send[0], nmode*sizeof(Message), MPI_BYTE, proc_in, 1, &message_recv[0], nmode*sizeof(Message), MPI_BYTE, proc_in, 0, comm, MPI_STATUS_IGNORE);
      for (int kz = 0; kz <= maxmode; kz++) {
///	if(!self_in[kz]){
	  xk1d(kz,l.xs-1) = message_recv[kz].value;
///	  neighbour_in[kz] = message_recv[kz].done;
///	}
      }
///    }
}

    // Communicate out
    // See note above for inward communication.
//    TODO Guard comms
///    if(!all(neighbour_out)) {
      //output<<"neighbour_out proc "<<BoutComm::rank()<<endl;
    if(!localmesh->lastX()){
      for (int kz = 0; kz <= maxmode; kz++) {
	message_send[kz].value = xk1d(kz,l.xe);
///	message_send[kz].done  = self_out[kz];
      }
      err = MPI_Sendrecv(&message_send[0], nmode*sizeof(Message), MPI_BYTE, proc_out, 0, &message_recv[0], nmode*sizeof(Message), MPI_BYTE, proc_out, 1, comm, MPI_STATUS_IGNORE);
      for (int kz = 0; kz < nmode; kz++) {
	xk1d(kz,l.xe+1) = message_recv[kz].value;
///	neighbour_out[kz] = message_recv[kz].done;
      }
      }

}  

void LaplaceParallelTriMG::init(Level &l, const int ncx, const int jy, const Matrix<dcomplex> avec, const Matrix<dcomplex> bvec, const Matrix<dcomplex> cvec, const Matrix<dcomplex> bcmplx){

  auto rlold = Array<dcomplex>(nmode);
  auto ruold = Array<dcomplex>(nmode);
  //auto Rd = Array<dcomplex>(ncz/2+1);
  //auto Ru = Array<dcomplex>(ncz/2+1);
  //auto Rsendup = Array<dcomplex>(ncz+2);
  //auto Rsenddown = Array<dcomplex>(ncz+2);
  //auto Rrecvup = Array<dcomplex>(ncz+2);
  //auto Rrecvdown = Array<dcomplex>(ncz+2);
  auto evec = Array<dcomplex>(ncx);
  auto tmp = Array<dcomplex>(ncx);

  // Define sizes of local coefficients
  l.minvb = Matrix<dcomplex>(nmode,ncx);
  l.upperGuardVector = Tensor<dcomplex>(localmesh->LocalNx, localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
  l.lowerGuardVector = Tensor<dcomplex>(localmesh->LocalNx, localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
  l.al = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
  l.bl = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
  l.au = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
  l.bu = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);

  l.alold = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
  l.blold = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
  l.auold = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);
  l.buold = Matrix<dcomplex>(localmesh->LocalNy, localmesh->LocalNz / 2 + 1);

  l.rl = Array<dcomplex>(localmesh->LocalNz / 2 + 1);
  l.ru = Array<dcomplex>(localmesh->LocalNz / 2 + 1);
  l.rlold = Array<dcomplex>(localmesh->LocalNz / 2 + 1);
  l.ruold = Array<dcomplex>(localmesh->LocalNz / 2 + 1);

  l.avec = Matrix<dcomplex>(nmode,ncx);
  l.bvec = Matrix<dcomplex>(nmode,ncx);
  l.cvec = Matrix<dcomplex>(nmode,ncx);
  l.rvec = Matrix<dcomplex>(nmode,ncx);
  for(int kz=0; kz<nmode; kz++){
    for(int ix=0; ix<ncx; ix++){
     l.avec(kz,ix) = avec(kz,ix); 
     l.bvec(kz,ix) = bvec(kz,ix); 
     l.cvec(kz,ix) = cvec(kz,ix); 
     l.rvec(kz,ix) = bcmplx(kz,ix); 
    }
  }

  for (int kz = 0; kz <= maxmode; kz++) {

    ///SCOREP_USER_REGION_END(kzinit);
    ///SCOREP_USER_REGION_DEFINE(invert);
    ///SCOREP_USER_REGION_BEGIN(invert, "invert local matrices",SCOREP_USER_REGION_TYPE_COMMON);

    // Invert local matrices
    // Calculate Minv*b
    tridag(&avec(kz,0), &bvec(kz,0), &cvec(kz,0), &bcmplx(kz,0),
    &l.minvb(kz,0), ncx);
    // Now minvb is a constant vector throughout the iterations

    // TODO GUARD?
    //if(first_call(jy,kz) || not store_coefficients ){
      // If not already stored, find edge update vectors
      //
      // Upper interface
      if(not localmesh->lastX()) { 
	// Need the xend-th element
	for(int i=0; i<ncx; i++){
	  evec[i] = 0.0;
	}
	evec[l.xe+1] = 1.0;
	tridag(&avec(kz,0), &bvec(kz,0), &cvec(kz,0), std::begin(evec),
	std::begin(tmp), ncx);
	for(int i=0; i<ncx; i++){
	  l.upperGuardVector(i,jy,kz) = tmp[i];
	}
      } else {
	for(int i=0; i<ncx; i++){
	  l.upperGuardVector(i,jy,kz) = 0.0;
	}
      }

      // Lower interface
      if(not localmesh->firstX()) { 
	for(int i=0; i<ncx; i++){
	  evec[i] = 0.0;
	}
	evec[l.xs-1] = 1.0;
	tridag(&avec(kz,0), &bvec(kz,0), &cvec(kz,0), std::begin(evec),
	std::begin(tmp), ncx);
	for(int i=0; i<ncx; i++){
	  l.lowerGuardVector(i,jy,kz) = tmp[i];
	}
      } else {
	for(int i=0; i<ncx; i++){
	  l.lowerGuardVector(i,jy,kz) = 0.0;
	}
      }
///    }

    ///SCOREP_USER_REGION_END(invert);
    ///SCOREP_USER_REGION_DEFINE(coefs);
    ///SCOREP_USER_REGION_BEGIN(coefs, "calculate coefs",SCOREP_USER_REGION_TYPE_COMMON);

      // TODO Guard?
///    if( first_call(jy,kz) or not store_coefficients ){
      l.bl(jy,kz) = l.upperGuardVector(l.xs,jy,kz);
      l.al(jy,kz) = l.lowerGuardVector(l.xs,jy,kz);

      l.bu(jy,kz) = l.upperGuardVector(l.xe,jy,kz);
      l.au(jy,kz) = l.lowerGuardVector(l.xe,jy,kz);

      l.alold(jy,kz) = l.al(jy,kz);
      l.auold(jy,kz) = l.au(jy,kz);
      l.blold(jy,kz) = l.bl(jy,kz);
      l.buold(jy,kz) = l.bu(jy,kz);
///    }

    l.rl[kz] = l.minvb(kz,l.xs);
    l.ru[kz] = l.minvb(kz,l.xe);
    l.rlold[kz] = l.rl[kz];
    l.ruold[kz] = l.ru[kz];

    // New method - connect to more distant points
    /*
    if(new_method){

      // First compute coefficients that depend on the matrix to be inverted
      // and which therefore might be constant throughout a run.
      // TODO Guard?
      //if( first_call(jy,kz) or not store_coefficients){

	// Boundary processor values to be overwritten when relevant
	Ad = 1.0;
	Bd = 0.0;
	Au = 0.0;
	Bu = 1.0;
	if(not localmesh->firstX()){
	  // Send coefficients down
	  Atmp = al(jy,kz);
	  Btmp = 0.0;
	  if( std::fabs(bu(jy,kz)) > 1e-14 ){
	    Btmp = bl(jy,kz)/bu(jy,kz);
	    Atmp -= Btmp*au(jy,kz);
	  }
	  // Send these
	  Ad = localmesh->communicateXIn(Atmp);
	  Bd = localmesh->communicateXIn(Btmp);
	}
	if(not localmesh->lastX()){
	  // Send coefficients up
	  Atmp = 0.0;
	  Btmp = bu(jy,kz);
	  if( std::fabs(al(jy,kz)) > 1e-14 ){
	    Atmp = au(jy,kz)/al(jy,kz);
	    Btmp -= Atmp*bl(jy,kz);
	  }
	  // Send these
	  Au = localmesh->communicateXOut(Atmp);
	  Bu = localmesh->communicateXOut(Btmp);
	}

	dcomplex Delta;
	Delta = 1.0 - al(jy,kz)*Bd - bu(jy,kz)*Au + (al(jy,kz)*bu(jy,kz) - au(jy,kz)*bl(jy,kz))*Bd*Au;
	Delta = 1.0 / Delta;
	al(jy,kz) = Delta*( alold(jy,kz) + (auold(jy,kz)*blold(jy,kz) - alold(jy,kz)*buold(jy,kz))*Au )*Ad;
	bl(jy,kz) = Delta * blold(jy,kz) * Bu ;
	au(jy,kz) = Delta * auold(jy,kz) * Ad ;
	bu(jy,kz) = Delta*( buold(jy,kz) + (auold(jy,kz)*blold(jy,kz) - alold(jy,kz)*buold(jy,kz))*Bd )*Bu;

	dcomplex d = auold(jy,kz)*blold(jy,kz) - alold(jy,kz)*buold(jy,kz);
	r1(jy,kz) = Delta*(alold(jy,kz) + d*Au);
	r2(jy,kz) = Delta*( 1.0 - buold(jy,kz)*Au );
	r3(jy,kz) = Delta*blold(jy,kz)*Au;
	r4(jy,kz) = Delta*blold(jy,kz);
	r5(jy,kz) = Delta*auold(jy,kz);
	r6(jy,kz) = Delta*auold(jy,kz)*Bd;
	r7(jy,kz) = Delta*( 1.0 - alold(jy,kz)*Bd );
	r8(jy,kz) = Delta*(buold(jy,kz) + d*Bd);

///      }

      // Now compute coefficients that depend on the right-hand side and
      // which therefore change every time.

      // Boundary processor values to be overwritten when relevant
      Rd[kz] = 0.0;
      Ru[kz] = 0.0;
      if(not localmesh->firstX()){
	// Send coefficients down
	Rsenddown[kz] = rl[kz];
	if( std::fabs(buold(jy,kz)) > 1e-14 ){
	  Rsenddown[kz] -= ru[kz]*blold(jy,kz)/buold(jy,kz);
	}
	Rd[kz] = localmesh->communicateXIn(Rsenddown[kz]);
      }
      if(not localmesh->lastX()){
	// Send coefficients up
	Rsendup[kz] = ru[kz];
	if( std::fabs(alold(jy,kz)) > 1e-14 ){
	  Rsendup[kz] -= rl[kz]*auold(jy,kz)/alold(jy,kz);
	}
	Ru[kz] = localmesh->communicateXOut(Rsendup[kz]);
      }
    } // new method
  */
    ///SCOREP_USER_REGION_END(coefs);
  } // end of kz loop

  ///SCOREP_USER_REGION_DEFINE(comm_coefs);
  ///SCOREP_USER_REGION_BEGIN(comm_coefs, "comm coefs",SCOREP_USER_REGION_TYPE_COMMON);
  // Communicate vector in kz
  /*
  if(new_method){
    if(not localmesh->firstX()){
      for (int kz = 0; kz <= maxmode; kz++) {
	// TODO
        //Rsenddown[kz+maxmode] = xloclast(2,kz);
      }
    }
    if(not localmesh->lastX()){
      for (int kz = 0; kz <= maxmode; kz++) {
	// TODO
        //Rsendup[kz+maxmode] = xloclast(1,kz);
      }
    }

    if(not localmesh->firstX()){
      err = MPI_Sendrecv(&Rsenddown[0], 2*nmode, MPI_DOUBLE_COMPLEX, proc_in, 1, &Rrecvdown[0], 2*nmode, MPI_DOUBLE_COMPLEX, proc_in, 0, comm, MPI_STATUS_IGNORE);
    }
    if(not localmesh->lastX()){
      err = MPI_Sendrecv(&Rsendup[0], 2*nmode, MPI_DOUBLE_COMPLEX, proc_out, 0, &Rrecvup[0], 2*nmode, MPI_DOUBLE_COMPLEX, proc_out, 1, comm, MPI_STATUS_IGNORE);
    }

    if(not localmesh->firstX()){
      for (int kz = 0; kz <= maxmode; kz++) {
        Rd[kz] = Rrecvdown[kz];
	// TODO
        //xloclast(0,kz) = Rrecvdown[kz+maxmode];
      }
    }
    if(not localmesh->lastX()){
      for (int kz = 0; kz <= maxmode; kz++) {
        Ru[kz] = Rrecvup[kz];
	// TODO
        //xloclast(3,kz) = Rrecvup[kz+maxmode];
      }
    }

    for (int kz = 0; kz <= maxmode; kz++) {
      rl[kz] = r1(jy,kz)*Rd[kz] + r2(jy,kz)*rlold[kz] + r3(jy,kz)*ruold[kz] + r4(jy,kz)*Ru[kz] ;
      ru[kz] = r5(jy,kz)*Rd[kz] + r6(jy,kz)*rlold[kz] + r7(jy,kz)*ruold[kz] + r8(jy,kz)*Ru[kz] ;
    }
  }
  */

}

void LaplaceParallelTriMG::coarsen(const Level l, Matrix<dcomplex> &xloc, Matrix<dcomplex> &xloclast, int jy){

  MPI_Comm comm = BoutComm::get();
  Array<dcomplex> tmpsend, tmprecv;
  MPI_Request request[1];
  tmpsend = Array<dcomplex>(2*nmode);
  tmprecv = Array<dcomplex>(2*nmode);

  if(!localmesh->firstX()){
    MPI_Irecv(&tmprecv[0], 2*nmode, MPI_DOUBLE_COMPLEX, proc_in, 0, comm, &request[0]);
  }

  for(int kz=0; kz<nmode; kz++){

    // Reconstruct required x point
    // xloc[1] and xloc[3] are the same point, no manipulation needed
    // An xloc[0] on a physical boundary does not move, no manipulation
    // Otherwise xloc[0] must be received from the processor below
    // xloc[2] always moves 1 (fine) grid point to the left, so must be recalculated and sent upwards
    xloc(2,kz) = l.minvb(kz,l.xe-1);
    xloclast(2,kz) = l.minvb(kz,l.xe-1);
    if(!localmesh->lastX()){
      xloc(2,kz) += l.upperGuardVector(l.xe-1,jy,kz)*xloc(3,kz);
      xloclast(2,kz) += l.upperGuardVector(l.xe-1,jy,kz)*xloclast(3,kz);
    }
    if(!localmesh->firstX()){
      xloc(2,kz) += l.lowerGuardVector(l.xe-1,jy,kz)*xloc(0,kz);
      xloclast(2,kz) += l.lowerGuardVector(l.xe-1,jy,kz)*xloclast(0,kz);
    }
  }
  if(!localmesh->lastX()){
    // Send upwards
    for(int kz=0; kz<nmode; kz++){
      tmpsend[kz] = xloc(2,kz);
    }
    for(int kz=0; kz<nmode; kz++){
      tmpsend[nmode+kz] = xloclast(2,kz);
    }
    MPI_Isend(&tmpsend[0], 2*nmode, MPI_DOUBLE_COMPLEX, proc_out, 0, comm, &request[0]);
  }
  if(!localmesh->firstX()){
    MPI_Wait(&request[0],MPI_STATUS_IGNORE);
    for(int kz=0; kz<nmode; kz++){
      xloc(0,kz) = tmprecv[kz];
    }
    for(int kz=0; kz<nmode; kz++){
      xloclast(0,kz) = tmprecv[nmode+kz];
    }
  }
}

void LaplaceParallelTriMG::refine(Matrix<dcomplex> &xloc, Matrix<dcomplex> &xloclast){

  // xloc[1] and xloc[3] don't change
  // xloc[0] unchanged if firstX, otherwise interpolated
  // xloc[2] always interpolated
  for(int kz=0; kz<nmode; kz++){
    if(!localmesh->firstX()){
      xloc(0,kz) = 0.5*(xloc(0,kz)+xloc(1,kz));
      xloclast(0,kz) = 0.5*(xloclast(0,kz)+xloclast(1,kz));
    }
    xloc(2,kz) = 0.5*(xloc(2,kz)+xloc(3,kz));

    xloclast(2,kz) = 0.5*(xloclast(2,kz)+xloclast(3,kz));
  }
}
