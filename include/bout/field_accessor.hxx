// GPU version Field-Accessor, updated by Dr. Yining Qin, Oct.27, 2020

#pragma once
#ifndef FIELD_ACCESSOR_H__
#define FIELD_ACCESSOR_H__

#include "../bout_types.hxx"
#include "../field.hxx"
#include "../field2d.hxx"
#include "../field3d.hxx"
#include "coordinate_field_accessor.hxx"
#include "coordinates.hxx"
#include "build_config.hxx"

template <CELL_LOC location = CELL_CENTRE, class FieldType = Field3D>
struct FieldAccessor {
  /// Remove default constructor
  FieldAccessor() = delete;

  /// Constructor from Field3D
  explicit FieldAccessor(FieldType& f)
      : coords(f.getCoordinates()), dx(coords->dx), dy(coords->dy), dz(coords->dz),
        J(coords->J), G1(coords->G1), G3(coords->G3), g11(coords->g11), g12(coords->g12),
        g13(coords->g13), g22(coords->g22), g23(coords->g23), g33(coords->g33),
        g_11(coords->g_11), g_12(coords->g_12), g_13(coords->g_13), g_22(coords->g_22),
        g_23(coords->g_23), g_33(coords->g_33) {
    ASSERT0(f.getLocation() == location);
    ASSERT0(f.isAllocated());

    data = &f(0, 0, 0);

    // Field size
    nx = f.getNx();
    ny = f.getNy();
    nz = f.getNz();

    // Mesh z size, for index conversion
    mesh_nz = f.getMesh()->LocalNz;

    if (f.hasParallelSlices()) {
      // Get arrays from yup and ydown fields
      yup = &(f.yup()(0, 0, 0));
      ydown = &(f.ydown()(0, 0, 0));
    }

    // ddt() array data
    ddt = &(f.timeDeriv()->operator()(0, 0, 0));
  }

  BOUT_HOST_DEVICE BoutReal operator[](int ind) const {
    return data[ind];
  }

  BoutReal* data{nullptr}; ///< Pointer to the Field data
  BoutReal* ddt{nullptr};  ///< Time-derivative data

  BoutReal* yup{nullptr};   ///< Pointer to the Field yup data
  BoutReal* ydown{nullptr}; ///< Pointer to the Field ydown data

  Coordinates* coords;

  // Metric tensor (Coordinates) data
  // Note: The data size depends on Coordinates::FieldMetric
  //       and could be Field2D or Field3D

  CoordinateFieldAccessor dx, dy, dz; /// Grid spacing
  CoordinateFieldAccessor J;          ///< Coordinate system Jacobian

  CoordinateFieldAccessor G1, G3;

  CoordinateFieldAccessor g11, g12, g13, g22, g23, g33;

  CoordinateFieldAccessor g_11, g_12, g_13, g_22, g_23, g_33;

  // Field size
  int nx = 0;
  int ny = 0;
  int nz = 0;

  // Mesh Z size. Used to convert 3D to 2D indices
  int mesh_nz;
};

/// Define a shorthand for 2D fields
template <CELL_LOC location = CELL_CENTRE>
using Field2DAccessor = FieldAccessor<location, Field2D>;

/// Syntactic sugar for time derivative of a field
template <CELL_LOC location, class FieldType>
BOUT_HOST_DEVICE inline BoutReal* ddt(FieldAccessor<location, FieldType> &fa) {
  return fa.ddt;
}

#endif
