/*!
 * \file naulin_laplace.cxx
 *
 * \brief Iterative solver to handle non-constant-in-z coefficients
 *
 * Scheme suggested by Volker Naulin: solve
 * Delp2(phi[i+1]) + DC(A/D)*phi[i+1] = rhs(phi[i]) + DC(A/D)*phi[i]
 * using standard FFT-based solver, iterating to include other terms by
 * evaluating them on rhs using phi from previous iteration.
 * DC part (i.e. Field2D part) of A/D is kept in the FFT inversion so that all
 * Neumann boundary conditions can be used at least when DC(A/D)!=0.
 *
 * CHANGELOG
 * =========
 *
 **************************************************************************
 * Copyright 2018 B.D.Dudson, M. Loiten, J. Omotani
 *
 * Contact: Ben Dudson, benjamin.dudson@york.ac.uk
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
 * ## Explanation of the procedure:
 * A way to invert the equation
 * \f$\Omega^D = \nabla\cdot(n\nabla_\perp \phi)\f$
 * invented by Naulin, V.
 * In an orthogonal system, we have that:
 *
 * \f{eqnarray}{
 * \Omega^D &=& \nabla\cdot(n\nabla_\perp \phi)\\
 *       &=& n \nabla_\perp^2 \phi + \nabla n\cdot\nabla_\perp \phi\\
 *       &=& n \Omega + \nabla n\cdot\nabla_\perp \phi\\
 *       &=& n \Omega + \nabla_\perp n\cdot\nabla_\perp \phi
 * \f}
 *
 * Rearranging gives
 *
 * \f{eqnarray}{
 * \Omega  &=& \frac{\Omega^D}{n} - \nabla_\perp \ln(n)\cdot\nabla_\perp \phi\\
 * \nabla_\perp^2 \phi
 * &=& \frac{\Omega^D}{n} - \nabla_\perp \ln(n)\cdot\nabla_\perp \phi
 * \f}
 *
 * In fact we allow for the slightly more general form
 *
 * \f{eqnarray}{
 * \nabla_\perp^2 \phi + <\frac{1}{D\,C1}> \nabla_\perp <C2>\cdot\nabla_\perp \phi + <\frac{A}{D}>\phi
 * &=& rhs/D - \left(\frac{1}{D\,C1} \nabla_\perp C2 - <\frac{1}{D\,C1}> \nabla_\perp <C2> \right)\cdot\nabla_\perp \phi
 * &&  - (\frac{A}{D} - <\frac{A}{D}>)*\phi
 * \f}
 *
 * The iteration now works as follows:
 *      1. Get the vorticity from
 *         \code{.cpp}
 *         vort = (vortD/n) - grad_perp(ln_n)*grad_perp(phiCur)
 *         [Delp2(phiNext) + 1/DC(D*C1)*grad_perp(DC(C2))*grad_perp(phiNext) + DC(A/D)*phiNext
 *          = b(phiCur)
 *          = (rhs/D) - (1/(D*C1)*grad_perp(C2) - 1/DC(D*C1)*grad_perp(DC(C2)))*grad_perp(phiCur)
*             - (A/D - DC(A/D))*phiCur]
 *         \endcode
 *         where phiCur is phi of the current iteration
 *         [and DC(f) is the constant-in-z component of f]
 *      2. Invert \f$phi\f$ to find the voricity using
 *         \code{.cpp}
 *         phiNext = invert_laplace_perp(vort)
 *         [set Acoef of laplace_perp solver to DC(A/D)
 *         phiNext = invert_laplace_perp(b)]
 *         \endcode
 *         where phiNext is the newly obtained \f$phi\f$
 *      3. Calculate the error at phi=phiNext
 *         \code{.cpp}
 *         error3D = Delp2(phiNext) + 1/C1*grad_perp(C2)*grad_perp(phiNext) + A/D*phiNext - rhs/D
 *                 = b(phiCur) - b(phiNext)
 *         \endcode
 *         as
 *         \code{.cpp}
 *         b(phiCur) = Delp2(phiNext) + 1/(DC(D*C1))*grad_perp(DC(C2))*grad_perp(phiNext) + DC(A/D)*phiNext
 *         \endcode
 *         up to rounding errors
 *      4. Calculate the infinity norms of the error
 *         \code{.cpp}
 *         EAbsLInf = max(error3D)
 *         ERelLInf = EAbsLInf/sqrt( max((rhs/D)^2) )
 *         \endcode
 *      5. Check whether
 *         \code{.cpp}
 *         EAbsLInf > atol
 *         \endcode
 *          * If yes
 *              * Check whether
 *                \code{.cpp}
 *                ERelLInf > rtol
 *                \endcode
 *              * If yes
 *                  * Set
 *                    \code{.cpp}
 *                    phiCur = phiNext
 *                    \endcode
 *                    increase curCount and start from step 1
 *                  * If number of iteration is above maxit, throw exception
 *              * If no
 *                  * Stop: Function returns phiNext
 *          * if no
 *              * Stop: Function returns phiNext
 */

#include <boutexception.hxx>
#include "bout/constants.hxx"
#include <bout/mesh.hxx>
#include <bout/coordinates.hxx>
#include <bout/sys/timer.hxx>
#include <derivs.hxx>
#include <difops.hxx>
#include "fft.hxx"
#include <globals.hxx>
#include <output.hxx>

#include "naulin_laplace.hxx"

LaplaceNaulin::LaplaceNaulin(Options *opt, const CELL_LOC loc, Mesh *mesh_in)
    : Laplacian(opt, loc, mesh_in), Acoef(0.0), C1coef(1.0), C2coef(0.0), Dcoef(1.0),
      delp2solver(nullptr), naulinsolver_mean_its(0.), ncalls(0) {

  ASSERT1(opt != nullptr); // An Options pointer should always be passed in by LaplaceFactory

  Acoef.setLocation(location);
  C1coef.setLocation(location);
  C2coef.setLocation(location);
  Dcoef.setLocation(location);

  // Get options
  OPTION(opt, rtol, 1.e-7);
  OPTION(opt, atol, 1.e-20);
  OPTION(opt, maxits, 100);
  delp2solver = create(opt->getSection("delp2solver"), location, localmesh);
  std::string delp2type;
  opt->getSection("delp2solver")->get("type", delp2type, "cyclic");
  // Check delp2solver is using an FFT scheme, otherwise it will not exactly
  // invert Delp2 and we will not converge
  ASSERT0( delp2type=="cyclic" || delp2type=="spt" || delp2type=="tri" );
  // Use same flags for FFT solver as for NaulinSolver
  delp2solver->setGlobalFlags(global_flags);
  delp2solver->setInnerBoundaryFlags(inner_boundary_flags);
  delp2solver->setOuterBoundaryFlags(outer_boundary_flags);

  static bool first = true;
  if (first) {
    SAVE_REPEAT(naulinsolver_mean_its);
    first = false;
  }
}

LaplaceNaulin::~LaplaceNaulin() {
  delete delp2solver;
}

const Field3D LaplaceNaulin::solve(const Field3D &rhs, const Field3D &x0) {
  // Rearrange equation so first term is just Delp2(x):
  //   D*Delp2(x) + 1/C1*Grad_perp(C2).Grad_perp(phi) = rhs
  //   -> Delp2(x) + 1/(C1*D)*Grad_perp(C2).Grad_perp(phi) = rhs/D

  Timer timer("invert"); ///< Start timer

  ASSERT1(rhs.getLocation() == location);
  ASSERT1(x0.getLocation() == location);
  ASSERT1(Dcoef.getLocation() == location);
  ASSERT1(C1coef.getLocation() == location);
  ASSERT1(C2coef.getLocation() == location);
  ASSERT1(Acoef.getLocation() == location);
  ASSERT1(localmesh == rhs.getMesh() && localmesh == x0.getMesh());

  Field3D x(x0); // Result

  Field3D rhsOverD = rhs/Dcoef;

  // x-component of 1./(C1*D) * Grad_perp(C2)
  Field3D coef_x = DDX(C2coef, location, DIFF_C2)/C1coef/Dcoef;

  // z-component of 1./(C1*D) * Grad_perp(C2)
  Field3D coef_z = DDZ(C2coef, location, DIFF_FFT)/C1coef/Dcoef;

  Field3D AOverD = Acoef/Dcoef;


  // Split coefficients into DC and AC parts so that delp2solver can use DC part.
  // This allows all-Neumann boundary conditions as long as AOverD_DC is non-zero

  Field2D C1coefTimesD_DC = DC(C1coef*Dcoef);
  Field2D C2coef_DC = DC(C2coef);

  // Our naming is slightly misleading here, as coef_x_AC may actually have a
  // DC component, as the AC components of C2coef and C1coefTimesD are not
  // necessarily in phase.
  // This is the piece that cannot be passed to an FFT-based Laplacian solver
  // (through our current interface).
  Field3D coef_x_AC = coef_x - DDX(C2coef_DC, location, DIFF_C2)/C1coefTimesD_DC;

  // coef_z is a z-derivative so must already have zero DC component

  Field2D AOverD_DC = DC(AOverD);
  Field3D AOverD_AC = AOverD - AOverD_DC;


  delp2solver->setCoefA(AOverD_DC);
  delp2solver->setCoefC1(C1coefTimesD_DC);
  delp2solver->setCoefC2(C2coef_DC);

  // Use this below to normalize error for relative error estimate
  BoutReal RMS_rhsOverD = sqrt(mean(SQ(rhsOverD), true, RGN_NOBNDRY)); // use sqrt(mean(SQ)) to make sure we do not divide by zero at a point

  BoutReal error_rel = 1e20, error_abs=1e20;
  int count = 0;

  // Initial values for derivatives of x
  Field3D ddx_x = DDX(x, location, DIFF_C2);
  Field3D ddz_x = DDZ(x, location, DIFF_FFT);
  Field3D b = rhsOverD - (coords->g11*coef_x_AC*ddx_x + coords->g33*coef_z*ddz_x + coords->g13*(coef_x_AC*ddz_x + coef_z*ddx_x)) - AOverD_AC*x;

  while (error_rel>rtol && error_abs>atol) {

    if ( (inner_boundary_flags & INVERT_SET) || (outer_boundary_flags & INVERT_SET) )
      // This passes in the boundary conditions from x0's guard cells
      copy_x_boundaries(x, x0, localmesh);

    // NB need to pass x in case boundary flags require 'x0', even if
    // delp2solver is not iterative and does not use an initial guess
    x = delp2solver->solve(b, x);
    localmesh->communicate(x);

    // re-calculate the rhs from the new solution
    // Use here to calculate an error, can also use for the next iteration
    ddx_x = DDX(x, location, DIFF_C2); // can be used also for the next iteration
    ddz_x = DDZ(x, location, DIFF_FFT);
    Field3D bnew = rhsOverD - (coords->g11*coef_x_AC*ddx_x + coords->g33*coef_z*ddz_x + coords->g13*(coef_x_AC*ddz_x + coef_z*ddx_x)) - AOverD_AC*x;

    Field3D error3D = b - bnew;
    error_abs = max(abs(error3D, RGN_NOBNDRY), true, RGN_NOBNDRY);
    error_rel = error_abs / RMS_rhsOverD;

    b = bnew;

    count++;
    if (count>maxits)
      throw BoutException("LaplaceNaulin error: Took more than maxits=%i iterations to converge.", maxits);
  }

  ncalls++;
  naulinsolver_mean_its = (naulinsolver_mean_its*BoutReal(ncalls-1) + BoutReal(count))/BoutReal(ncalls);

  return x;
}

const FieldPerp LaplaceNaulin::solve(const FieldPerp &rhs, const FieldPerp &x0) {
  // Rearrange equation so first term is just Delp2(x):
  //   D*Delp2(x) + 1/C1*Grad_perp(C2).Grad_perp(phi) = rhs
  //   -> Delp2(x) + 1/(C1*D)*Grad_perp(C2).Grad_perp(phi) = rhs/D

  Timer timer("invert"); ///< Start timer

  ASSERT1(rhs.getLocation() == location);
  ASSERT1(x0.getLocation() == location);
  ASSERT1(rhs.getIndex() == x0.getIndex());
  ASSERT1(Dcoef.getLocation() == location);
  ASSERT1(C1coef.getLocation() == location);
  ASSERT1(C2coef.getLocation() == location);
  ASSERT1(Acoef.getLocation() == location);
  ASSERT1(localmesh == rhs.getMesh() && localmesh == x0.getMesh());

  FieldPerp x(x0); // Result

  int yind = rhs.getIndex();
  auto coords = rhs.getCoordinates();

  FieldPerp rhsOverD = rhs/sliceXZ(Dcoef, yind);

  Field3D  AOverD = Acoef/Dcoef;


  // Split coefficients into DC and AC parts so that delp2solver can use DC part.
  // This allows all-Neumann boundary conditions as long as AOverD_DC is non-zero
  // Not optimal to re-calculate DC(...) for each call to solve(FieldPerp,FieldPerp).

  Field2D C1coefTimesD_DC = DC(C1coef*Dcoef);
  Field2D C2coef_DC = DC(C2coef);

  // x-component of 1./(C1*D) * Grad_perp(C2)
  FieldPerp coef_x{emptyFrom(rhs)};
  // z-component of 1./(C1*D) * Grad_perp(C2)
  FieldPerp coef_z{emptyFrom(rhs)};
  // Our naming is slightly misleading here, as coef_x_AC may actually have a
  // DC component, as the AC components of C2coef and C1coefTimesD are not
  // necessarily in phase.
  // This is the piece that cannot be passed to an FFT-based Laplacian solver
  // (through our current interface).
  FieldPerp coef_x_AC{emptyFrom(rhs)};
  // coef_z is a z-derivative so must already have zero DC component
  Field2D AOverD_DC = DC(AOverD);
  FieldPerp AOverD_AC{emptyFrom(rhs)};
  // Initial values for derivatives of x
  Field3D ddx_C2 = DDX(C2coef, location, "C2");
  Field3D ddz_C2 = DDZ(C2coef, location, "FFT");
  Field2D ddx_C2_DC = DDX(C2coef_DC, location, "C2");
  FieldPerp ddx_x{emptyFrom(rhs)};
  FieldPerp ddz_x{emptyFrom(rhs)};
  const int nz = localmesh->LocalNz;
  coef_x = (sliceXZ(ddx_C2, yind)/C1coef)/Dcoef;
  coef_z = (sliceXZ(ddz_C2, yind)/C1coef)/Dcoef;
  coef_x_AC = coef_x - ddx_C2_DC/C1coefTimesD_DC;
  AOverD_AC = sliceXZ(AOverD, yind) - AOverD_DC;
  BOUT_FOR(i, coef_x.getRegion("RGN_NOBNDRY")) {
    const auto i2d = localmesh->indPerpto2D(i, yind);
    ddx_x[i] = (x[i.xp()] - x[i.xm()])/2./coords->dx[i2d];
  }
  BOUT_OMP(parallel)
  {
    Array<dcomplex> cv(nz / 2 + 1);
    BOUT_OMP(for)
    for (int ix = localmesh->xstart; ix <= localmesh->xend; ix++) {
      // Take FFT in Z direction
      rfft(x[ix], nz, cv.begin()); // Forward FFT

      for (int jz = 0; jz < nz / 2 + 1; jz++) {
        const BoutReal kwave = jz * TWOPI / nz; // wave number is 1/[rad]
        cv[jz] *= dcomplex(0, kwave);
      }

      irfft(cv.begin(), nz, ddz_x[ix]); // Reverse FFT
    }
  }
  ddz_x /= coords->dz;

  delp2solver->setCoefA(AOverD_DC);
  delp2solver->setCoefC1(C1coefTimesD_DC);
  delp2solver->setCoefC2(C2coef_DC);

  // Use this below to normalize error for relative error estimate
  BoutReal RMS_rhsOverD = sqrt(mean(SQ(rhsOverD), true, RGN_NOBNDRY)); // use sqrt(mean(SQ)) to make sure we do not divide by zero at a point

  BoutReal error_rel = 1e20, error_abs=1e20;
  int count = 0;

  FieldPerp b = rhsOverD - (coords->g11*coef_x_AC*ddx_x + coords->g33*coef_z*ddz_x + coords->g13*(coef_x_AC*ddz_x + coef_z*ddx_x)) - AOverD_AC*x;

  while (error_rel>rtol && error_abs>atol) {

    if ( (inner_boundary_flags & INVERT_SET) || (outer_boundary_flags & INVERT_SET) )
      // This passes in the boundary conditions from x0's guard cells
      copy_x_boundaries(x, x0, localmesh);

    // NB need to pass x in case boundary flags require 'x0', even if
    // delp2solver is not iterative and does not use an initial guess
    x = delp2solver->solve(b, x);
    localmesh->communicate(x);

    // re-calculate the rhs from the new solution
    // Use here to calculate an error, can also use for the next iteration
    BOUT_FOR(i, coef_x.getRegion("RGN_NOBNDRY")) {
      int ix = i.x();
      ddx_x[i] = (x[i.xp()] - x[i.xm()])/2./coords->dx(ix, yind);
    }
    BOUT_OMP(parallel)
    {
      Array<dcomplex> cv(nz / 2 + 1);
      BOUT_OMP(for)
      for (int ix = localmesh->xstart; ix <= localmesh->xend; ix++) {
        // Take FFT in Z direction
        rfft(x[ix], nz, cv.begin()); // Forward FFT

        for (int jz = 0; jz < nz / 2 + 1; jz++) {
          const BoutReal kwave = jz * TWOPI / nz; // wave number is 1/[rad]
          cv[jz] *= dcomplex(0, kwave);
        }

        irfft(cv.begin(), nz, ddz_x[ix]); // Reverse FFT
      }
    }
    ddz_x /= coords->dz;

    FieldPerp bnew = rhsOverD - (coords->g11*coef_x_AC*ddx_x + coords->g33*coef_z*ddz_x + coords->g13*(coef_x_AC*ddz_x + coef_z*ddx_x)) - AOverD_AC*x;

    FieldPerp errorPerp = b - bnew;
    error_abs = max(abs(errorPerp, RGN_NOBNDRY), true, RGN_NOBNDRY);
    error_rel = error_abs / RMS_rhsOverD;

    b = bnew;

    count++;
    if (count>maxits)
      throw BoutException("LaplaceNaulin error: Took more than maxits=%i iterations to converge.", maxits);
  }

  ncalls++;
  naulinsolver_mean_its = (naulinsolver_mean_its*BoutReal(ncalls-1) + BoutReal(count))/BoutReal(ncalls);

  return x;
}

void LaplaceNaulin::copy_x_boundaries(Field3D &x, const Field3D &x0, Mesh *localmesh) {
  ASSERT1(areFieldsCompatible(x, x0));

  if (localmesh->firstX()) {
    for (int i=localmesh->xstart-1; i>=0; i--)
      for (int j=localmesh->ystart; j<=localmesh->yend; j++)
        for (int k=0; k<localmesh->LocalNz; k++)
          x(i, j, k) = x0(i, j, k);
  }
  if (localmesh->lastX()) {
    for (int i=localmesh->xend+1; i<localmesh->LocalNx; i++)
      for (int j=localmesh->ystart; j<=localmesh->yend; j++)
        for (int k=0; k<localmesh->LocalNz; k++)
          x(i, j, k) = x0(i, j, k);
  }
}

void LaplaceNaulin::copy_x_boundaries(FieldPerp &x, const FieldPerp &x0, Mesh *localmesh) {
  ASSERT1(areFieldsCompatible(x, x0));

  if (localmesh->firstX()) {
    for (int i=localmesh->xstart-1; i>=0; i--)
      for (int k=0; k<localmesh->LocalNz; k++)
        x(i, k) = x0(i, k);
  }
  if (localmesh->lastX()) {
    for (int i=localmesh->xend+1; i<localmesh->LocalNx; i++)
      for (int k=0; k<localmesh->LocalNz; k++)
        x(i, k) = x0(i, k);
  }
}
