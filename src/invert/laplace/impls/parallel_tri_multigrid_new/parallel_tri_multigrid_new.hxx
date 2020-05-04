/**************************************************************************
 * Perpendicular Laplacian inversion. Parallel code using FFT
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

class LaplaceParallelTriMGNew;

#ifndef __PARALLEL_TRI_MG_NEW_H__
#define __PARALLEL_TRI_MG_NEW_H__

#include <invert_laplace.hxx>
#include <dcomplex.hxx>
#include <options.hxx>

class LaplaceParallelTriMGNew : public Laplacian {
public:
  LaplaceParallelTriMGNew(Options *opt = nullptr, const CELL_LOC loc = CELL_CENTRE, Mesh *mesh_in = nullptr);
  ~LaplaceParallelTriMGNew(){};

  using Laplacian::setCoefA;
  void setCoefA(const Field2D &val) override {
    ASSERT1(val.getLocation() == location);
    ASSERT1(localmesh == val.getMesh());
    A = val;
  }
  using Laplacian::setCoefC;
  void setCoefC(const Field2D &val) override {
    ASSERT1(val.getLocation() == location);
    ASSERT1(localmesh == val.getMesh());
    C = val;
  }
  using Laplacian::setCoefD;
  void setCoefD(const Field2D &val) override {
    ASSERT1(val.getLocation() == location);
    ASSERT1(localmesh == val.getMesh());
    D = val;
  }
  using Laplacian::setCoefEx;
  void setCoefEx(const Field2D &UNUSED(val)) override {
    throw BoutException("LaplaceParallelTriMG does not have Ex coefficient");
  }
  using Laplacian::setCoefEz;
  void setCoefEz(const Field2D &UNUSED(val)) override {
    throw BoutException("LaplaceParallelTriMG does not have Ez coefficient");
  }

  using Laplacian::solve;
  FieldPerp solve(const FieldPerp &b) override;
  FieldPerp solve(const FieldPerp &b, const FieldPerp &x0) override;
  //FieldPerp solve(const FieldPerp &b, const FieldPerp &x0, const FieldPerp &b0 = 0.0);

  BoutReal getMeanIterations() const { return ipt_mean_its; }
  void resetMeanIterations() { ipt_mean_its = 0; }

  void get_initial_guess(const int jy, const int kz, Matrix<dcomplex> &r,
      Tensor<dcomplex> &lowerGuardVector, Tensor<dcomplex> &upperGuardVector,
      Matrix<dcomplex> &xk1d);

  void resetSolver();

  bool all(const Array<bool>);
  bool any(const Array<bool>);
  BoutReal max(const Array<BoutReal>);
  int maxloc(const Array<BoutReal>);

  struct Level {

  public:

    Tensor<dcomplex> upperGuardVector, lowerGuardVector;
    Matrix<dcomplex> al, bl, au, bu;
    Matrix<dcomplex> alold, blold, auold, buold;
    Matrix<dcomplex> xloc;
    Matrix<dcomplex> r1, r2;
    Array<dcomplex> rl, ru;
    Array<dcomplex> rlold, ruold;
    Matrix<dcomplex> minvb;
    Matrix<dcomplex> residual;
    Tensor<dcomplex> avec, bvec, cvec;
    Tensor<dcomplex> ar, br, cr;
    Matrix<dcomplex> rr;

    int err;
    MPI_Comm comm;
    int xproc;
    int yproc;
    int myproc;
    int proc_in;
    int proc_out;
    bool included;
    bool included_up;
    bool red, black;
    int xs, xe, ncx;
    int current_level;

  };

  void calculate_residual(Level &level, const Array<bool> &converged, const int jy);
  void calculate_total_residual(Array<BoutReal> &total, Array<BoutReal> &globalmaxsol, Array<bool> &converged, Level &level);
  void coarsen(Level &level, const Matrix<dcomplex> &fine_residual, const Array<bool> &converged);
  void gauss_seidel_red_black(Level &level, const Array<bool> &converged, const int jy);
  void init(Level &level, const Level lup, const int ncx, const int xs, const int xe, const int current_level, const int jy);
  void init(Level &level, const int ncx, const int jy, const Matrix<dcomplex> avec, const Matrix<dcomplex> bvec, const Matrix<dcomplex> cvec, const int xs, const int xe);
  void init_rhs(Level &level, const int jy, const Matrix<dcomplex> bcmplx);
  bool is_diagonally_dominant(const Level &coarsest_level, const int jy, const int kz);
  void levels_info(const Level l, const int jy);
  void reconstruct_full_solution(Matrix<dcomplex> &xk1d, const Level &level, const int jy);
  void refine(Level &level, Level &level_up, Matrix<dcomplex> &fine_error, const Array<bool> &converged);
  void synchronize_reduced_field(const Level &l, Matrix<dcomplex> &field);
  void update_solution(Level &l, const Matrix<dcomplex> &fine_error, const Array<bool> &converged);

private:

  // Information about the grids
  std::vector<Level> levels;

  // The coefficents in
  // D*grad_perp^2(x) + (1/C)*(grad_perp(C))*grad_perp(x) + A*x = b
  Field2D A, C, D;

  // Flag to state whether this is the first time the solver is called
  // on the point (jy,kz).
  Matrix<bool> first_call;

  // Save previous x in Fourier space
  Tensor<dcomplex> x0saved;

  /// Solver tolerances
  BoutReal rtol, atol;

  /// Maximum number of iterations
  int maxits;

  /// Maximum number of coarse grids
  int max_level;

  /// Maximum number of iterations per grid
  int max_cycle;

  /// Mean number of iterations taken by the solver
  BoutReal ipt_mean_its;

  /// Counter for the number of times the solver has been called
  int ncalls;

  /// If true, use previous timestep's solution as initial guess for next step
  /// If false, use the approximate solution of the system (neglecting the
  /// coupling terms between processors) as the initial guess.
  /// The first timestep always uses the approximate solution.
  bool use_previous_timestep;

  //Tensor<dcomplex> upperGuardVector, lowerGuardVector;
///  Matrix<dcomplex> al, bl, au, bu;
///  Matrix<dcomplex> alold, blold, auold, buold;
///  Matrix<dcomplex> r1, r2, r3, r4, r5, r6, r7, r8;
  bool store_coefficients;

  int nmode;
  int proc_in;
  int proc_out;
  int myproc;
  int nproc;

};


#endif // __PARALLEL_TRI_NEW_H__
