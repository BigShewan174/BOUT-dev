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

class LaplaceIPT;

#ifndef __IPT_H__
#define __IPT_H__

#include <invert_laplace.hxx>
#include <dcomplex.hxx>
#include <options.hxx>
#include <utils.hxx>

namespace {
RegisterLaplace<LaplaceIPT> registerlaplaceipt(LAPLACE_IPT);
}

class LaplaceIPT : public Laplacian {
public:
  LaplaceIPT(Options *opt = nullptr, const CELL_LOC loc = CELL_CENTRE, Mesh *mesh_in = nullptr);
  ~LaplaceIPT() = default;

  friend class Level;

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
  FieldPerp solve(const FieldPerp& b) override { return solve(b, b); }
  FieldPerp solve(const FieldPerp &b, const FieldPerp &x0) override;

  BoutReal getMeanIterations() const { return ipt_mean_its; }
  void resetMeanIterations() { ipt_mean_its = 0; }

  void get_initial_guess(int jy, int kz, Matrix<dcomplex> &r,
      Tensor<dcomplex> &lowerGuardVector, Tensor<dcomplex> &upperGuardVector,
      Matrix<dcomplex> &xk1d);

  void resetSolver();

  class Level {

  public:

    Matrix<dcomplex> xloc;
    Matrix<dcomplex> residual;
    Tensor<dcomplex> ar, br, cr, brinv;
    Matrix<dcomplex> rr;

    MPI_Comm comm;
    int xproc;
    int yproc;
    int myproc;
    int proc_in, proc_out;
    int proc_in_up, proc_out_up;
    bool included;
    bool included_up;
    bool red, black;
    int current_level;

    // indexing to remove branches from tight loops
    int index_start;
    int index_end;

    void calculate_residual(const LaplaceIPT& lap);
    void calculate_total_residual(LaplaceIPT& lap, Array<BoutReal> &total, Array<BoutReal> &globalmaxsol, Array<bool> &converged);
    void coarsen(const LaplaceIPT& lap, const Matrix<dcomplex> &fine_residual);
    void gauss_seidel_red_black(const LaplaceIPT& lap);
    void init(const LaplaceIPT &lap, const Level lup, int current_level);
    void init(LaplaceIPT &lap);
    void init_rhs(LaplaceIPT &lap, const Matrix<dcomplex> bcmplx);
    bool is_diagonally_dominant(const LaplaceIPT &lap);
    void reconstruct_full_solution(const LaplaceIPT &lap, Matrix<dcomplex> &xk1d);
    void refine(const LaplaceIPT &lap, Matrix<dcomplex> &fine_error);
    void synchronize_reduced_field(const LaplaceIPT &lap, Matrix<dcomplex> &field);
    void update_solution(const LaplaceIPT& lap);

  };

private:

  /// Information about the grids
  std::vector<Level> levels;

  /// Current y index
  int jy;

  /// The coefficents in
  /// $D*grad_perp^2(x) + (1/C)*(grad_perp(C))*grad_perp(x) + A*x = b$
  Field2D A, C, D;

  /// Lower-, on- and upper-diagonal terms of the operator matrix
  Tensor<dcomplex> avec, bvec, cvec;

  /// Coefficients for recovering the full solution from guard cells
  Tensor<dcomplex> upperGuardVector, lowerGuardVector;
  Matrix<dcomplex> al, bl, au, bu;
  Matrix<dcomplex> r1, r2;
  Array<dcomplex> rl, ru;
  Matrix<dcomplex> minvb;

  /// Flag to state whether this is the first time the solver is called
  /// on the point (jy,kz).
  Array<bool> first_call;

  /// Save previous x in Fourier space
  Tensor<dcomplex> x0saved;

  /// Solver tolerances
  BoutReal rtol, atol;

  /// Maximum number of iterations
  int maxits;

  /// Maximum number of coarse grids
  int max_level;

  /// Maximum number of iterations per grid
  int max_cycle;

  /// Predict when convergence will be reached, and skip expensive convergence
  /// checks at earlier iterations.
  bool predict_exit;

  /// Mean number of iterations taken by the solver
  BoutReal ipt_mean_its;

  /// Counter for the number of times the solver has been called
  int ncalls;

  /// True when matrix to be inverted is constant, allowing results to be cached and work skipped
  bool store_coefficients;

  /// Number of unfiltered Fourier modes
  int nmode;

  /// Neighbouring processors in the in and out directions
  int proc_in, proc_out;

  /// This processor's unique ID
  int myproc;

  /// Shorthand for localmesh->NXPE
  int nproc;

  /// Array recording whether a kz mode is converged
  Array<bool> converged ;

  /// Error interpolated onto the grid one finer than current grid
  Matrix<dcomplex> fine_error;

  /// Number of local x, y, z points
  int ncx, ny;

  /// First and last interior points xstart, xend
  int xs, xe;

};

#endif // __IPT_H__
