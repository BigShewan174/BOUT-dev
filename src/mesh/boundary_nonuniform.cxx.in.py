#!/usr/bin/env python3

from jinja2 import Environment
import sys
import stencils_sympy as sten

from boundary_nonuniform_common import orders, boundaries, maybeopen

header = """\
#include <boundary_standard.hxx>
#include <bout/constants.hxx>
#include <boutexception.hxx>
#include <derivs.hxx>
#include <fft.hxx>
#include <globals.hxx>
#include <invert_laplace.hxx>
#include <msg_stack.hxx>
#include <output.hxx>
#include <utils.hxx>

#include "boundary_nonuniform.hxx"

static void update_stagger_offsets(int& x_boundary_offset, int& y_boundary_offset, int& stagger, CELL_LOC loc){
  // NB: bx is going outwards
  // NB: XLOW means shifted in -x direction
  // `stagger` stagger direction with respect to direction of boundary
  //   0 : no stagger or orthogonal to boundary direction
  //   1 : staggerd in direction of boundary
  //  -1 : staggerd in oposite direction of boundary
  // Also note that all offsets are basically half a cell
  if (loc == CELL_XLOW) {
    if (x_boundary_offset == 0) {
      x_boundary_offset = -1;
    } else if (x_boundary_offset < 0) {
      stagger = -1;
    } else {
      stagger = 1;
    }
  }
  if (loc == CELL_YLOW) {
    if (y_boundary_offset == 0) {
      y_boundary_offset = -1;
    } else if (y_boundary_offset < 0) {
      stagger = -1;
    } else {
      stagger = 1;
    }
  }
}
"""

env = Environment(trim_blocks=True)

apply_str = """
#if ! BOUT_USE_METRIC_3D

void Boundary{{type}}NonUniform_O{{order}}::apply(Field3D &f, MAYBE_UNUSED(BoutReal t)) {
  bndry->first();
  Mesh *mesh = f.getMesh();
  CELL_LOC loc = f.getLocation();

{% if with_fg %}
  // Decide which generator to use
  std::shared_ptr<FieldGenerator> fg = gen;
  if (!fg)
    fg = f.getBndryGenerator(bndry->location);

  std::vector<BoutReal> vals;
  vals.reserve(mesh->LocalNz);
{% endif %}

  int x_boundary_offset = bndry->bx;
  int y_boundary_offset = bndry->by;
  int stagger = 0;
  update_stagger_offsets(x_boundary_offset, y_boundary_offset, stagger, loc);

  for (; !bndry->isDone(); bndry->next1d()) {
{% if with_fg %}
    if (fg) {
      // Calculate the X and Y normalised values half-way between the guard cell and
      // grid cell
      const BoutReal xnorm = 0.5 *
                     (mesh->GlobalX(bndry->x)          // In the guard cell
                      + mesh->GlobalX(bndry->x - x_boundary_offset)); // the grid cell
      const BoutReal ynorm = TWOPI * 0.5 *
                     (mesh->GlobalY(bndry->y)          // In the guard cell
                      + mesh->GlobalY(bndry->y - y_boundary_offset)); // the grid cell
      const BoutReal zfac =  TWOPI / mesh->LocalNz;
      for (int zk = 0; zk < mesh->LocalNz; zk++) {
        vals[zk] = fg->generate(bout::generator::Context().set("x", xnorm, "y", ynorm, "z", zfac * zk, "t" , t));
      }
    }
{% endif %}

    vec{{order}} spacing;

    const Coordinates::FieldMetric &coords_field =
        bndry->by != 0 ? mesh->getCoordinates()->dy : mesh->getCoordinates()->dx;
{% if type == "Dirichlet" %}
{% for i in range(1,order) %}
    Indices i{{i}}{bndry->x - {{i}} * bndry->bx, bndry->y - {{i}} * bndry->by, 0};
{% endfor %}
    if (stagger == 0) {
      BoutReal offset;
      spacing.f0 = 0;
      BoutReal total_offset = 0;
{% for i in range(1,order) %}
      offset = coords_field(i{{i}}.x, i{{i}}.y);
      spacing.f{{i}} = total_offset + offset / 2;
      total_offset += offset;
{% endfor %}
    } else {
      spacing.f0 = 0;
{% for i in range(1,order) %}
        spacing.f{{i}} = spacing.f{{i-1}} + coords_field(i{{i}}.x, i{{i}}.y);
{% endfor %}

    }
    if (stagger == -1) {
{% for i in range(1,order) %}
      i{{i}} = {bndry->x - {{i+1}} * bndry->bx, bndry->y - {{i+1}} * bndry->by, 0};
{% endfor %}
    }
    // with dirichlet, we specify the value on the boundary, even if
    // the value is part of the evolving system.
    for (int i = ((stagger == -1) ? -1 : 0); i < bndry->width; i++) {
      Indices ic{bndry->x + i * bndry->bx, bndry->y + i * bndry->by, 0};
      vec{{order}} facs;
      if (stagger == 0) {
        BoutReal to_add = coords_field(ic.x, ic.y) / 2;
        spacing += to_add;
        facs = calc_interp_to_stencil(spacing);
        spacing += to_add;
      } else {
        if (stagger == -1
              && i != -1) {
          spacing += coords_field(ic.x, ic.y);
        }
        facs = calc_interp_to_stencil(spacing);
        if (stagger == 1) {
          spacing += coords_field(ic.x, ic.y);
        }
      }
      for (int iz = 0; iz < mesh->LocalNz; iz++) {
        const BoutReal val = (fg) ? vals[iz] : 0.0;
        f(ic.x, ic.y, iz) = facs.f0 * val
{% for i in range(1,order) %}
           + facs.f{{i}} *f(i{{i}}.x, i{{i}}.y, iz)
{% endfor %}
        ;
{% elif type == "Neumann" %}
{% for i in range(1,order) %}
    Indices i{{i}}{bndry->x - {{i}} * bndry->bx, bndry->y - {{i}} * bndry->by, 0};
{% endfor %}
    if (stagger == 0) {
      BoutReal offset;
      spacing.f0 = 0;
      BoutReal total_offset=0;
{% for i in range(1,order) %}
      offset = coords_field(i{{i}}.x, i{{i}}.y);
      spacing.f{{i}} = total_offset + offset / 2;
      total_offset += offset;
{% endfor %}
    } else {
      spacing.f0 = 0;
      // Check if we are staggered and also boundary in low
      //  direction
      // In the case of Neumann we have in this case two values
      //  defined at the same point
      if (stagger == -1 &&
              (    (bndry->bx && x_boundary_offset == -1)
                || (bndry->by && y_boundary_offset == -1))){
        spacing.f1 = spacing.f0;
{% for i in range(2,order) %}
        spacing.f{{i}} = spacing.f{{i-1}} + coords_field(i{{i-1}}.x, i{{i-1}}.y);
{% endfor %}
      } else {
{% for i in range(1,order) %}
        spacing.f{{i}} = spacing.f{{i-1}} + coords_field(i{{i}}.x, i{{i}}.y);
{% endfor %}
      }
    }
    // With free and neumann the value is not set if the point is
    // evolved and it is on the boundary.
    for (int i = 0; i < bndry->width; i++) {
      Indices ic{bndry->x + i * bndry->bx, bndry->y + i * bndry->by, 0};
      vec{{order}} facs;
      if (stagger == 0) {
        BoutReal to_add = coords_field(ic.x, ic.y) / 2;
        spacing += to_add;
        facs = calc_interp_to_stencil(spacing);
        spacing += to_add;
      } else {
        if (stagger == -1) {
          spacing += coords_field(ic.x, ic.y);
        }
        facs = calc_interp_to_stencil(spacing);
        if (stagger == 1) {
          spacing += coords_field(ic.x, ic.y);
        }
      }
      for (int iz = 0; iz < mesh->LocalNz; iz++) {
        const BoutReal val = (fg) ? vals[iz] : 0.0;
        f(ic.x, ic.y, iz) = facs.f0 * val
{% for i in range(1,order) %}
           + facs.f{{i}} *f(i{{i}}.x, i{{i}}.y, iz)
{% endfor %}
        ;
{% elif type == "Free" %}
{% for i in range(order) %}
    const Indices i{{i}}{bndry->x - {{i+1}} * bndry->bx, bndry->y - {{i+1}} * bndry->by, 0};
{% endfor %}
    if (stagger == 0) {
      BoutReal total_offset = 0;
      BoutReal offset;
{% for i in range(order) %}
      offset = coords_field(i{{i}}.x, i{{i}}.y);
      spacing.f{{i}} = total_offset + offset / 2;
      total_offset += offset;
{% endfor %}
    } else {
      spacing.f0 = coords_field(i0.x, i0.y);
{% for i in range(1,order) %}
      spacing.f{{i}} = spacing.f{{i-1}} + coords_field(i{{i}}.x, i{{i}}.y);
{% endfor %}
    }

    // With free and neumann the value is not set if the point is
    // evolved and it is on the boundary.
    for (int i = 0; i < bndry->width; i++) {
      Indices ic{bndry->x + i * bndry->bx, bndry->y + i * bndry->by, 0};
      vec{{order}} facs;
      if (stagger == 0) {
        BoutReal to_add = coords_field(ic.x, ic.y) / 2;
        spacing += to_add;
        facs = calc_interp_to_stencil(spacing);
        spacing += to_add;
      } else {
        facs = calc_interp_to_stencil(spacing);
        spacing += coords_field(ic.x, ic.y);
      }
      for (int iz = 0; iz < mesh->LocalNz; iz++) {
        f(ic.x, ic.y, iz) = facs.f0 * f(i0.x, i0.y, iz)
{% for i in range(1,order) %}
           + facs.f{{i}} *f(i{{i}}.x, i{{i}}.y, iz)
{% endfor %}
        ;
{% endif %}
      }
    }
  }
}

#else // BOUT_USE_METRIC_3D

void Boundary{{type}}NonUniform_O{{order}}::apply(Field3D &f, MAYBE_UNUSED(BoutReal t)) {
  bndry->first();
  Mesh *mesh = f.getMesh();
  CELL_LOC loc = f.getLocation();

{% if with_fg %}
  // Decide which generator to use
  std::shared_ptr<FieldGenerator> fg = gen;
  if (!fg)
    fg = f.getBndryGenerator(bndry->location);

  std::vector<BoutReal> vals;
  vals.reserve(mesh->LocalNz);
{% endif %}

  int x_boundary_offset = bndry->bx;
  int y_boundary_offset = bndry->by;
  int stagger = 0;
  update_stagger_offsets(x_boundary_offset, y_boundary_offset, stagger, loc);

  for (; !bndry->isDone(); bndry->next1d()) {
{% if with_fg %}
    if (fg) {;
      const BoutReal zfac =  TWOPI / mesh->LocalNz;
      // Calculate the X and Y normalised values half-way between the guard cell and
      // grid cell
      const BoutReal xnorm = 0.5 *
                     (mesh->GlobalX(bndry->x)          // In the guard cell
                      + mesh->GlobalX(bndry->x - x_boundary_offset)); // the grid cell
      const BoutReal ynorm = TWOPI * 0.5 *
                     (mesh->GlobalY(bndry->y)          // In the guard cell
                      + mesh->GlobalY(bndry->y - y_boundary_offset)); // the grid cell
      for (int zk = 0; zk < mesh->LocalNz; zk++) {
        vals[zk] = fg->generate(bout::generator::Context().set("x", xnorm, "y", ynorm, "z", zfac * zk, "t" , t));
      }
    }
{% endif %}

    vec{{order}} spacing;

    const Coordinates::FieldMetric &coords_field =
        bndry->by != 0 ? mesh->getCoordinates()->dy : mesh->getCoordinates()->dx;
{% if type == "Dirichlet" %}{# DIRICHLET  DIRICHLET  DIRICHLET  DIRICHLET  DIRICHLET  DIRICHLET #}
    for (int iz = 0; iz < mesh->LocalNz; iz++) {
{% for i in range(1,order) %}
      Indices i{{i}}{bndry->x - {{i}} * bndry->bx, bndry->y - {{i}} * bndry->by, 0};
{% endfor %}
      if (stagger == 0) {
        BoutReal offset;
        spacing.f0 = 0;
        BoutReal total_offset = 0;
{% for i in range(1,order) %}
        offset = coords_field(i{{i}}.x, i{{i}}.y, iz);
        spacing.f{{i}} = total_offset + offset / 2;
        total_offset += offset;
{% endfor %}
      } else {
        spacing.f0 = 0;
{% for i in range(1,order) %}
          spacing.f{{i}} = spacing.f{{i-1}} + coords_field(i{{i}}.x, i{{i}}.y, iz);
{% endfor %}
      }
      if (stagger == -1) {
{% for i in range(1,order) %}
        i{{i}} = {bndry->x - {{i+1}} * bndry->bx, bndry->y - {{i+1}} * bndry->by, iz};
{% endfor %}
      }
      // with dirichlet, we specify the value on the boundary, even if
      // the value is part of the evolving system.
      for (int i = ((stagger == -1) ? -1 : 0); i < bndry->width; i++) {
        Indices ic{bndry->x + i * bndry->bx, bndry->y + i * bndry->by, iz};
        vec{{order}} facs;
        if (stagger == 0) {
          BoutReal to_add = coords_field(ic.x, ic.y, iz) / 2;
          spacing += to_add;
          facs = calc_interp_to_stencil(spacing);
          spacing += to_add;
        } else {
          if (stagger == -1
                && i != -1) {
            spacing += coords_field(ic.x, ic.y, iz);
          }
          facs = calc_interp_to_stencil(spacing);
          if (stagger == 1) {
            spacing += coords_field(ic.x, ic.y, iz);
          }
        }
        const BoutReal val = (fg) ? vals[iz] : 0.0;
        f(ic.x, ic.y, iz) = facs.f0 * val
{% for i in range(1,order) %}
           + facs.f{{i}} *f(i{{i}}.x, i{{i}}.y, iz)
{% endfor %}
        ;
{% elif type == "Neumann" %}{# NEUMANN  NEUMANN  NEUMANN  NEUMANN  NEUMANN  NEUMANN  NEUMANN #}
{% for i in range(1,order) %}
    Indices i{{i}}{bndry->x - {{i}} * bndry->bx, bndry->y - {{i}} * bndry->by, 0};
{% endfor %}
    for (int iz = 0; iz < mesh->LocalNz; iz++) {
      if (stagger == 0) {
        BoutReal offset;
        spacing.f0 = 0;
        BoutReal total_offset=0;
{% for i in range(1,order) %}
        offset = coords_field(i{{i}}.x, i{{i}}.y, iz);
        spacing.f{{i}} = total_offset + offset / 2;
        total_offset += offset;
{% endfor %}
      } else { // stagger != 0
        spacing.f0 = 0;
        // Check if we are staggered and also boundary in low
        //  direction
        // In the case of Neumann we have in this case two values
        //  defined at the same point
        if (stagger == -1 &&
                (    (bndry->bx && x_boundary_offset == -1)
                  || (bndry->by && y_boundary_offset == -1))){
          spacing.f1 = spacing.f0;
{% for i in range(2,order) %}
          spacing.f{{i}} = spacing.f{{i-1}} + coords_field(i{{i-1}}.x, i{{i-1}}.y, iz);
{% endfor %}
        } else {
{% for i in range(1,order) %}
          spacing.f{{i}} = spacing.f{{i-1}} + coords_field(i{{i}}.x, i{{i}}.y, iz);
{% endfor %}
        }
      } // stagger != 0
      // With neumann (and free) the value is not set if the point is
      // evolved and it is on the boundary.
      for (int i = 0; i < bndry->width; i++) {
        Indices ic{bndry->x + i * bndry->bx, bndry->y + i * bndry->by, 0};
        vec{{order}} facs;
        if (stagger == 0) {
          BoutReal to_add = coords_field(ic.x, ic.y, iz) / 2;
          spacing += to_add;
          facs = calc_interp_to_stencil(spacing);
          spacing += to_add;
        } else {
          if (stagger == -1) {
            spacing += coords_field(ic.x, ic.y, iz);
          }
          facs = calc_interp_to_stencil(spacing);
          if (stagger == 1) {
            spacing += coords_field(ic.x, ic.y, iz);
          }
        }
        const BoutReal val = (fg) ? vals[iz] : 0.0;
        f(ic.x, ic.y, iz) = facs.f0 * val
{% for i in range(1,order) %}
           + facs.f{{i}} *f(i{{i}}.x, i{{i}}.y, iz)
{% endfor %}
        ;
{% elif type == "Free" %}{# FREE  FREE  FREE  FREE  FREE  FREE  FREE  FREE  FREE  FREE  FREE  FREE #}
{% for i in range(order) %}
    const Indices i{{i}}{bndry->x - {{i+1}} * bndry->bx, bndry->y - {{i+1}} * bndry->by, 0};
{% endfor %}
    for (int iz = 0; iz < mesh->LocalNz; iz++) {
      if (stagger == 0) {
        BoutReal total_offset = 0;
        BoutReal offset;
{% for i in range(order) %}
        offset = coords_field(i{{i}}.x, i{{i}}.y, iz);
        spacing.f{{i}} = total_offset + offset / 2;
        total_offset += offset;
{% endfor %}
      } else {
        spacing.f0 = coords_field(i0.x, i0.y, iz);
{% for i in range(1,order) %}
        spacing.f{{i}} = spacing.f{{i-1}} + coords_field(i{{i}}.x, i{{i}}.y, iz);
{% endfor %}
      }

      // With free (and neumann) the value is not set if the point is
      // evolved and it is on the boundary.
      for (int i = 0; i < bndry->width; i++) {
        Indices ic{bndry->x + i * bndry->bx, bndry->y + i * bndry->by, 0};
        vec{{order}} facs;
        if (stagger == 0) {
          BoutReal to_add = coords_field(ic.x, ic.y, iz) / 2;
          spacing += to_add;
          facs = calc_interp_to_stencil(spacing);
          spacing += to_add;
        } else {
          facs = calc_interp_to_stencil(spacing);
          spacing += coords_field(ic.x, ic.y, iz);
        }
        f(ic.x, ic.y, iz) = facs.f0 * f(i0.x, i0.y, iz)
{% for i in range(1,order) %}
           + facs.f{{i}} *f(i{{i}}.x, i{{i}}.y, iz)
{% endfor %}
        ;
{% endif %}{# END  END  END  END  END  END  END  END  END  END  END  END #}
      }
    }
  }
}

#endif // BOUT_USE_METRIC_3D
"""
clone_str = """
BoundaryOp * {{class}}::clone(BoundaryRegion *region,
   const std::list<std::string> &args) {

  std::shared_ptr<FieldGenerator> newgen;
  if (!args.empty()) {
    // First argument should be an expression
    newgen = FieldFactory::get()->parse(args.front());
  }
  return new {{class}}(region, newgen);
}
"""
stencil_str = """
vec{{order}} {{class}}::calc_interp_to_stencil(const vec{{order}} & spacing) const {
vec{{order}} facs;
// Stencil Code
{{stencil_code}}
return facs;
}
"""


if __name__ == "__main__":
    with maybeopen(sys.argv) as print:
        print(header)

        for order in orders:
            for boundary in boundaries:
                if boundary == "Neumann":
                    mat = sten.neumann
                else:
                    mat = sten.dirichlet
                try:
                    code = sten.gen_code(order, mat)
                except:
                    import sys

                    print("Order:", order, "boundary:", boundary, file=sys.stderr)
                    raise
                args = {
                    "order": order,
                    "type": boundary,
                    "class": "Boundary%sNonUniform_O%d" % (boundary, order),
                    "stencil_code": code,
                    "with_fg": boundary != "Free",
                }

                print(env.from_string(apply_str).render(**args))
                print(env.from_string(clone_str).render(**args))
                print(env.from_string(stencil_str).render(**args))
