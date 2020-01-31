/*!
 * \file field.hxx
 * \brief field base class definition for differencing methods
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
 */

class Field;

#ifndef __FIELD_H__
#define __FIELD_H__

#include <cstdio>
#include <memory>

#include "bout_types.hxx"
#include "boutexception.hxx"
#include <globals.hxx>
#include "msg_stack.hxx"
#include "stencils.hxx"
#include "boutexception.hxx"
#include "unused.hxx"

class Mesh;
class Coordinates;

#ifdef TRACK
#include <string>
#endif

namespace bout {
namespace utils {

namespace details {
/// Helper class for fold expressions pre-C++17
///
/// Taken from "C++ Templates: The Complete Guide, Second Edition"
///  Addison-Wesley, 2017
///  ISBN-13:  978-0-321-71412-1
///  ISBN-10:      0-321-71412-1
/// Copyright © 2017 by Addison-Wesley, David Vandevoorde, Nicolai
/// M. Josuttis, and Douglas Gregor.
constexpr bool and_all() { return true; }
template <class T>
constexpr bool and_all(T cond) {
  return cond;
}
template <class T, class... Ts>
constexpr bool and_all(T cond, Ts... conds) {
  return cond and and_all(conds...);
}
} // namespace details

/// Enable a function if all the Ts are subclasses of `Field`, and
/// returns the common type: i.e. `Field3D` if at least one argument
/// is `Field3D`, otherwise `Field2D` if they are all `Field2D`
///
/// Examples
/// --------
///
/// Consider the following template function:
///
///     template <class T, class U, class V,
///          class ResultType = typename bout::utils::EnableIfField<T, U, V>>
///     auto where(const T& test, const U& gt0, const V& le0) -> ResultType {
///       // function body
///     }
///
/// This function only "appears" if `T`, `U` and `V` are all
/// subclasses of `Field`. `ResultType` is the common type of `T`, `U`
/// and `V`. If `T` and `U` are both `Field2D`, `ResultType` is
/// `Field2D` if `V` is `Field2D`, and `Field3D` if `V` is `Field3D`.
template <class... Ts>
using EnableIfField =
    typename std::enable_if<details::and_all(std::is_base_of<Field, Ts>::value ...),
                            typename std::common_type<Ts...>::type>::type;
} // namespace utils
} // namespace bout

/*!
 * \brief Base class for fields
 *
 * Defines the virtual function SetStencil, used by differencing methods
 */
class Field {
 public:
  Field() = default;

  Field(Mesh* localmesh, CELL_LOC location_in, DirectionTypes directions_in);

  // Copy constructor
  Field(const Field& f)
    : name(f.name), fieldmesh(f.fieldmesh),
      fieldCoordinates(f.fieldCoordinates), location(f.location),
      directions(f.directions) {}

  virtual ~Field() { }

  /// Set variable location for staggered grids to @param new_location
  ///
  /// Throws BoutException if new_location is not `CELL_CENTRE` and
  /// staggered grids are turned off and checks are on. If checks are
  /// off, silently sets location to ``CELL_CENTRE`` instead.
  void setLocation(CELL_LOC new_location);
  /// Get variable location
  CELL_LOC getLocation() const;

  /// Getters for DIRECTION types
  DirectionTypes getDirections() const {
    return directions;
  }
  YDirectionType getDirectionY() const {
    return directions.y;
  }
  ZDirectionType getDirectionZ() const {
    return directions.z;
  }

  /// Setters for *DirectionType
  void setDirectionY(YDirectionType y_type) {
    directions.y = y_type;
  }
  void setDirectionZ(ZDirectionType z_type) {
    directions.z = z_type;
  }

  std::string name;

#if CHECK > 0
  // Routines to test guard/boundary cells set
  
  virtual bool bndryValid() {
    if(!bndry_xin)
      throw BoutException("Inner X guard cells not set\n");
    if(!bndry_xout)
      throw BoutException("Outer X guard cells not set\n");
    if(!bndry_yup)
      throw BoutException("Upper y guard cells not set\n");
    if(!bndry_ydown)
      throw BoutException("Lower y guard cells not set\n");
    return true;
  }
  
  /// Status of the 4 boundaries
  bool bndry_xin{true}, bndry_xout{true}, bndry_yup{true}, bndry_ydown{true};
#endif

  Mesh* getMesh() const {
    if (fieldmesh) {
      return fieldmesh;
    } else {
      // Don't set fieldmesh=mesh here, so that fieldmesh==nullptr until
      // allocate() is called in one of the derived classes. fieldmesh==nullptr
      // indicates that some initialization that would be done in the
      // constructor if fieldmesh was a valid Mesh object still needs to be
      // done.
      return bout::globals::mesh;
    }
  }

  /// Returns a pointer to the coordinates object at this field's
  /// location from the mesh this field is on.
  Coordinates* getCoordinates() const;

  /// Returns a pointer to the coordinates object at the requested
  /// location from the mesh this field is on. If location is CELL_DEFAULT
  /// then return coordinates at field location
  Coordinates* getCoordinates(CELL_LOC loc) const;

  /*!
   * Return the number of nx points
   */
  virtual int getNx() const;
  /*!
   * Return the number of ny points
   */
  virtual int getNy() const;
  /*!
   * Return the number of nz points
   */
  virtual int getNz() const;

  friend void swap(Field& first, Field& second) noexcept {
    using std::swap;
    swap(first.name, second.name);
    swap(first.fieldmesh, second.fieldmesh);
    swap(first.fieldCoordinates, second.fieldCoordinates);
    swap(first.location, second.location);
    swap(first.directions, second.directions);
  }
protected:
  Mesh* fieldmesh{nullptr};
  mutable std::shared_ptr<Coordinates> fieldCoordinates{nullptr};

  /// Location of the variable in the cell
  CELL_LOC location{CELL_CENTRE};

  /// Copy the members from another Field
  void copyFieldMembers(const Field& f) {
    name = f.name;
    fieldmesh = f.fieldmesh;
    fieldCoordinates = f.fieldCoordinates;
    location = f.location;
    directions = f.directions;
  }

private:
  DirectionTypes directions{YDirectionType::Standard, ZDirectionType::Standard};
};

/// Check if Fields have compatible meta-data
inline bool areFieldsCompatible(const Field& field1, const Field& field2) {
  return
      field1.getCoordinates() == field2.getCoordinates() &&
      field1.getMesh() == field2.getMesh() &&
      field1.getLocation() == field2.getLocation() &&
      areDirectionsCompatible(field1.getDirections(), field2.getDirections());
}

/// Return an empty shell field of some type derived from Field, with metadata
/// copied and a data array that is allocated but not initialised.
template<typename T>
inline T emptyFrom(const T& f) {
  static_assert(std::is_base_of<Field, T>::value, "emptyFrom only works on Fields");
  return T(f.getMesh(), f.getLocation(), {f.getDirectionY(), f.getDirectionZ()}).allocate();
}

/// Return a field of some type derived from Field, with metadata copied from
/// another field and a data array allocated and initialised to zero.
template<typename T>
inline T zeroFrom(const T& f) {
  static_assert(std::is_base_of<Field, T>::value, "emptyFrom only works on Fields");
  T result{emptyFrom(f)};
  result = 0.;
  return result;
}

/// Return a field of some type derived from Field, with metadata copied from
/// another field and a data array allocated and filled with the given value.
template<typename T>
inline T filledFrom(const T& f, BoutReal fill_value) {
  static_assert(std::is_base_of<Field, T>::value, "emptyFrom only works on Fields");
  T result{emptyFrom(f)};
  result = fill_value;
  return result;
}

/// Unary + operator. This doesn't do anything
template<typename T>
T operator+(const T& f) {return f;}

#endif /* __FIELD_H__ */
