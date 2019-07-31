/**************************************************************************
 * Perpendicular Laplacian inversion. Serial code using FFT
 * and tridiagonal solver.
 *
 **************************************************************************
 * Copyright 2010 B.D.Dudson, S.Farley, M.V.Umansky, X.Q.Xu
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
#include "parallel_tri.hxx"

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

LaplaceParallelTri::LaplaceParallelTri(Options *opt, CELL_LOC loc, Mesh *mesh_in)
    : Laplacian(opt, loc, mesh_in), A(0.0), C(1.0), D(1.0), ipt_mean_its(0.), ncalls(0), Borig(50.) {
  A.setLocation(location);
  C.setLocation(location);
  D.setLocation(location);

  OPTION(opt, rtol, 1.e-7);
  OPTION(opt, atol, 1.e-20);
  OPTION(opt, maxits, 100);
  OPTION(opt, B, 1000.0);
  OPTION(opt, om, 1.0);

  static int ipt_solver_count = 1;
  bout::globals::dump.addRepeat(ipt_mean_its,
      "ipt_solver"+std::to_string(ipt_solver_count)+"_mean_its");
  ++ipt_solver_count;

  Borig = B;

}

FieldPerp LaplaceParallelTri::solve(const FieldPerp& b) { return solve(b, b); }

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
 * \param[in] b0    Initial guess for solution x (probably better named x0 but this is
 *                  taken!) Default value 0.
 *
 * \return          The inverted variable.
 */
FieldPerp LaplaceParallelTri::solve(const FieldPerp& b, const FieldPerp& x0) {

  Timer timer("invert"); ///< Start timer

  ASSERT1(localmesh == b.getMesh() && localmesh == x0.getMesh());
  ASSERT1(b.getLocation() == location);
  ASSERT1(x0.getLocation() == location);
  TRACE("LaplaceParallelTri::solve(const, const)");

  FieldPerp x{emptyFrom(b)};

  //FieldPerp tmpreal = 0.0; //{emptyFrom(b)};
  //FieldPerp tmpimag = 0.0; //{emptyFrom(b)};
  FieldPerp tmpreal{emptyFrom(b)};
  FieldPerp tmpimag{emptyFrom(b)};
  FieldPerp imdone{emptyFrom(b)};

  int jy = b.getIndex();

  int ncz = localmesh->LocalNz; // No of z pnts
  int ncx = localmesh->LocalNx; // No of x pnts

  BoutReal kwaveFactor = 2.0 * PI / coords->zlength();

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
   *        LaplaceParallelTri::solve()
   * bk1d = The 1d array of bk
   * xk   = The fourier transformed of x, where x the output of
   *        LaplaceParallelTri::solve()
   * xk1d = The 1d array of xk
   */
  auto bk = Matrix<dcomplex>(ncx, ncz / 2 + 1);
  auto bk1d = Array<dcomplex>(ncx);
  auto xk = Matrix<dcomplex>(ncx, ncz / 2 + 1);
  auto xk1d = Array<dcomplex>(ncx);
  auto xk1dlast = Array<dcomplex>(ncx);
  auto error = Array<dcomplex>(ncx);
  BoutReal error_rel = 1e20, error_abs=1e20, last_error=error_abs;

  // Initialise xk to 0 as we only visit 0<= kz <= maxmode in solve
  for (int ix = 0; ix < ncx; ix++) {
    for (int kz = maxmode + 1; kz < ncz / 2 + 1; kz++) {
      xk(ix, kz) = 0.0; //b0(ix, kz);
    }
  }

  /* Coefficents in the tridiagonal solver matrix
   * Following the notation in "Numerical recipes"
   * avec is the lower diagonal of the matrix
   * bvec is the diagonal of the matrix
   * cvec is the upper diagonal of the matrix
   * NOTE: Do not confuse avec, bvec and cvec with the A, C, and D coefficients
   *       above
   */
  auto avec = Array<dcomplex>(ncx);
  auto bvec = Array<dcomplex>(ncx);
  auto cvec = Array<dcomplex>(ncx);

  BOUT_OMP(parallel for)
  for (int ix = 0; ix < ncx; ix++) {
    /* This for loop will set the bk (initialized by the constructor)
     * bk is the z fourier modes of b in z
     * If the INVERT_SET flag is set (meaning that x0 will be used to set the
     * bounadry values),
     */
    if (((ix < inbndry) && (inner_boundary_flags & INVERT_SET)) ||
        ((ncx - 1 - ix < outbndry) && (outer_boundary_flags & INVERT_SET))) {
      // Use the values in x0 in the boundary

      // x0 is the input
      // bk is the output
      rfft(x0[ix], ncz, &bk(ix, 0));

    } else {
      // b is the input
      // bk is the output
      rfft(b[ix], ncz, &bk(ix, 0));
    }
  }

  /* Solve differential equation in x for each fourier mode
   * Note that only the non-degenerate fourier modes are being used (i.e. the
   * offset and all the modes up to the Nyquist frequency)
   */
  for (int kz = 0; kz <= maxmode; kz++) {

    // set bk1d
    for (int ix = 0; ix < ncx; ix++) {
      // Get bk of the current fourier mode
      bk1d[ix] = bk(ix, kz);
      xk1d[ix] = 0.0;
      xk1dlast[ix] = 0.0;
    }

    int count = 0;
    // Guard cells of imdone signal if a proc has converged in the in/out direction 
    imdone = 0.0;
    // Boundary values are "converged" at the start
    if(localmesh->lastX()) { 
      imdone(localmesh->xend+1,kz) = 1.0;
    }
    if(localmesh->firstX()) { 
      imdone(localmesh->xstart-1,kz) = 1.0;
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
    tridagMatrix(std::begin(avec), std::begin(bvec), std::begin(cvec), std::begin(bk1d),
                 jy,
                 // wave number index
                 kz,
                 // wave number (different from kz only if we are taking a part
                 // of the z-domain [and not from 0 to 2*pi])
                 kz * kwaveFactor, global_flags, inner_boundary_flags,
                 outer_boundary_flags, &A, &C, &D);

//    if( BoutComm::rank() == 0 ) {
//      output << kz << endl;
//      for(int ix = 0; ix<localmesh->LocalNx ; ix++) {
//	output << avec[ix] << " " << bvec[ix] << " " << cvec[ix] << endl;
//      }
//    }
//

    BoutReal omorig = om ;
//    if( kz == 0 ) {
//      om = 1.1433;
//    }
//    else {
//      om = 1.0;
//    }	
//    output << om << " " << omorig << endl;
//  Over/under-relaxation
//    for(int ix = 0; ix<localmesh->LocalNx ; ix++) {
//      avec[ix] = om*avec[ix];
//      cvec[ix] = om*cvec[ix];
//    }

    ///////// PERFORM INVERSION /////////
    if (!localmesh->periodicX) {

      // Call tridiagonal solver
      //for(int it = 0; it < maxits; it++){ 
      BoutReal error_last = 1e20;
      int sub_it = 0;
      auto lh = Matrix<dcomplex>(3,ncx);
      auto rh = Matrix<dcomplex>(3,ncx);

//      if( Bvals(0,0,kz) < 0.0 ) {
//	B = Borig;
//      }
//      else {
//	B = Bvals(0,0,kz);
//      }
      if( kz == 0 ) {
	B = Borig;
      }
      else {
	B = 10.0;
      }
      //B = Borig;
      bool allow_B_change = true;


      while(true){ 

	// Patch up internal boundaries
	if(not localmesh->lastX()) { 
	  for(int ix = localmesh->xend+1; ix<localmesh->LocalNx ; ix++) {
	    avec[ix] = 0;
	    bvec[ix] = 1;
	    cvec[ix] = 0;
	    rh(sub_it,ix) = xk1d[ix];
	    if( sub_it == 2 ) {
	      xk1d[ix] = (rh(2,ix) - rh(0,ix)*exp(-B))/(1.0 - exp(-B));
	      rh(0,ix) = xk1d[ix];
	    }
	    bk1d[ix] = xk1d[ix];
	  }
	} 
	if(not localmesh->firstX()) { 
	  for(int ix = 0; ix<localmesh->xstart ; ix++) {
	    avec[ix] = 0;
	    bvec[ix] = 1;
	    cvec[ix] = 0;
	    lh(sub_it,ix) = xk1d[ix];
	    if( sub_it == 2 ) {
	      xk1d[ix] = (lh(2,ix) - lh(0,ix)*exp(-B))/(1.0 - exp(-B));
	      lh(0,ix) = xk1d[ix];
	    }
	    bk1d[ix] = xk1d[ix];
	  }
	}

//	for(int ix = localmesh->xstart; ix < localmesh->xend; ix++) {
//	  if( ix != localmesh->LocalNx-1) {
//	    bk1d[ix] = bk1d[ix] - (1.0-om)*avec[ix]*xk1d[ix+1];
//	  }
//	  if( ix != 0) {
//	    bk1d[ix] = bk1d[ix] - (1.0-om)*cvec[ix]*xk1d[ix-1];
//	  }
//	}

	sub_it += 1;
	if( sub_it == 3 ) {
	  sub_it = 0;
	  if( allow_B_change and count > 10 ) {
	    //if( count % 10 == 0 ) {
	      if(error_abs < last_error) {
		B *= 0.9;
		//output << jy << " " << kz << " " << last_error << " " << error_abs << " " << sub_it << " "  << count << " reducing B to" << B << endl;
	      }
	      else {
		B /= 0.9;
		allow_B_change = false;
		//output << jy << " " << kz << " " << last_error << " " << error_abs << " " << sub_it << " "  << count << " inceasing B to" << B << endl;
//		for(int ix = 0; ix<localmesh->LocalNx ; ix++) {
//		  xk1d[ix] = xk1dlast[ix];
//		  xk1dlast[ix] = 0.0;
//		}
	      }
	    //}
	    last_error = error_abs;
	  }
	}

	// Invert local matrices
        tridag(std::begin(avec), std::begin(bvec), std::begin(cvec), std::begin(bk1d),
             std::begin(xk1d), ncx);

	// Calculate errors
	error_abs = 0.0;
	BoutReal xmax = 0.0;
        for (int ix = 0; ix < ncx; ix++) {
	  BoutReal diff = abs(xk1d[ix] - xk1dlast[ix]);
	  BoutReal xabs = abs(xk1d[ix]);
	  if (diff > error_abs) {
	    error_abs = diff;
	  }
	  if (xabs > xmax) {
	    xmax = xabs;
	  }
	}
	error_rel = error_abs / xmax;

	TRACE("set comm flags pack");
	// Set communication flags
	if (error_rel<rtol or error_abs<atol) {
	  // In the next iteration this proc informs its neighbours that its halo cells
	  // will no longer be updated, then breaks.
	  imdone(localmesh->xstart,kz) = 1.0;
	  imdone(localmesh->xend,kz) = 1.0;
	}

	TRACE("buffer pack");
	// Pack buffers for communication
	if(imdone(localmesh->xstart-1, kz) == 0 or imdone(localmesh->xend+1, kz) == 0) {
	  for (int ix = 0; ix < ncx; ix++) {
	    tmpreal(ix,kz) = xk1d[ix].real();
	    tmpimag(ix,kz) = xk1d[ix].imag();
	  }
	}

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
	//   imdone(xstart - 1, kz) = whether this proc has in-converged
	//   imdone(xstart, kz)     = whether my in-neighbouring proc has out-converged
	//   imdone(xend + 1, kz)   = whether this proc has out-converged
	//   imdone(xend, kz)       = whether my out-neighbouring proc has in-converged
	//
	// If imdone(xstart-1, kz) != 0, I must have been told this by my neighbour. My 
	// neighbour has therefore done its one post-converged communication. My in-boundary
	// values are therefore correct, and I am in-converged. My neighbour is not 
	// expecting us to communicate.
	if(imdone(localmesh->xstart-1, kz) == 0) {
	  // Communicate in
	  localmesh->communicateXIn(imdone);
	  localmesh->communicateXIn(tmpreal);
	  localmesh->communicateXIn(tmpimag);
	  if(not localmesh->firstX()) { 
	    for(int ix = 0; ix<localmesh->xstart ; ix++) {
	      xk1d[ix] = dcomplex(tmpreal(ix,kz), tmpimag(ix,kz));
	    }
	  }
	}

	// Outward communication
	// See note above for inward communication.
	if(imdone(localmesh->xend+1, kz) == 0) {
	  // Communicate out
	  localmesh->communicateXOut(imdone);
	  localmesh->communicateXOut(tmpreal);
	  localmesh->communicateXOut(tmpimag);
	  if(not localmesh->lastX()) { 
	    for(int ix = localmesh->xend+1; ix<localmesh->LocalNx ; ix++) {
	      xk1d[ix] = dcomplex(tmpreal(ix,kz), tmpimag(ix,kz));
	    }
	  }
	}

	// Now I've done my communication, exit if I am both in- and out-converged
	if( imdone(localmesh->xend, kz) != 0.0 and imdone(localmesh->xstart, kz) != 0.0 ) {
          //output<<"Breaking, proc "<< BoutComm::rank() << ", count "<<count<<endl<<std::flush;
	  break;
	}

	// If my neighbour has converged, I know that I am also converged on that
	// boundary. Set this flag after the break loop above, to ensure we do one
	// iteration using our neighbour's converged value.
	if(imdone(localmesh->xstart-1, kz) != 0) {
	  imdone(localmesh->xstart, kz) = 1.0;
	}
	if(imdone(localmesh->xend+1, kz) != 0) {
	  imdone(localmesh->xend, kz) = 1.0;
	}

	++count;
	if (count>maxits) {
	  break;
	  //throw BoutException("LaplaceParallelTri error: Not converged within maxits=%i iterations.", maxits);
	}

        for (int ix = 0; ix < ncx; ix++) {
	  xk1dlast[ix] = xk1d[ix];
	}
	error_last = error_abs;
	
      }

    } else {
      // Periodic in X, so cyclic tridiagonal

      int xs = localmesh->xstart;
      cyclic_tridag(&avec[xs], &bvec[xs], &cvec[xs], &bk1d[xs], &xk1d[xs], ncx - 2 * xs);

      // Copy boundary regions
      for (int ix = 0; ix < xs; ix++) {
        xk1d[ix] = xk1d[ncx - 2 * xs + ix];
        xk1d[ncx - xs + ix] = xk1d[xs + ix];
      }
    }

    ++ncalls;
    ipt_mean_its = (ipt_mean_its * BoutReal(ncalls-1)
	+ BoutReal(count))/BoutReal(ncalls);
    output << jy << " " << kz << " " << count << " " << ncalls << " " << ipt_mean_its << " " << B << endl;
    om = omorig;
    //Bvals(0,0,kz) = B;

    // If the global flag is set to INVERT_KX_ZERO
    if ((global_flags & INVERT_KX_ZERO) && (kz == 0)) {
      dcomplex offset(0.0);
      for (int ix = localmesh->xstart; ix <= localmesh->xend; ix++) {
        offset += xk1d[ix];
      }
      offset /= static_cast<BoutReal>(localmesh->xend - localmesh->xstart + 1);
      for (int ix = localmesh->xstart; ix <= localmesh->xend; ix++) {
        xk1d[ix] -= offset;
      }
    }

    // Store the solution xk for the current fourier mode in a 2D array
    for (int ix = 0; ix < ncx; ix++) {
      xk(ix, kz) = xk1d[ix];
    }
  }

  //std::cout<<"end"<<endl;

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

  return x; // Result of the inversion
}
