# Helper functions for building BOUT++ models

# Copy FILENAME from source directory to build directory
macro(bout_copy_file FILENAME)
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/${FILENAME}
      ${CMAKE_CURRENT_BINARY_DIR}/${FILENAME}
      COPYONLY)
endmacro()


# Build a BOUT++ physics model
#
# This is basically just a simple wrapper around 'add_executable' and
# 'target_link_libraries'.
#
# Arguments:
# - MODEL: Name of the executable
# - SOURCES: List of source files to compile
function(bout_add_model MODEL)
  cmake_parse_arguments(BOUT_MODEL_OPTIONS "" "" "SOURCES" ${ARGN})

  if (NOT BOUT_MODEL_OPTIONS_SOURCES)
    message(FATAL_ERROR "Required argument SOURCES missing from 'bout_add_model'")
  endif()

  if ("SOURCES" IN_LIST BOUT_MODEL_OPTIONS_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR "SOURCES missing values from 'bout_add_model'")
  endif()

  add_executable(${MODEL} ${BOUT_MODEL_OPTIONS_SOURCES})
  target_link_libraries(${MODEL} bout++::bout++)
  target_include_directories(${MODEL} PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
endfunction()
  

# Build a BOUT++ example
#
# If called from a standalone project, just builds the example as a
# normal model. If called when building the BOUT++ library itself,
# also copy input files and optionally other files, like grid files,
# to the library build directory.
#
# Arguments:
# - EXAMPENAME: Name of the executable
# - SOURCES: List of source files to compile
# - DATA_DIRS: List of data directories to copy (default: 'data')
# - EXTRA_FILES: List of other files to copy
function(bout_add_example EXAMPLENAME)
  set(multiValueArgs SOURCES DATA_DIRS EXTRA_FILES)
  cmake_parse_arguments(BOUT_EXAMPLE_OPTIONS "" "" "${multiValueArgs}" ${ARGN})

  bout_add_model(${EXAMPLENAME} SOURCES ${BOUT_EXAMPLE_OPTIONS_SOURCES})

  # If this is a standalone project, we can stop here. Otherwise, we
  # need to copy the various input files to the build directory
  get_directory_property(HAS_PARENT PARENT_DIRECTORY)
  if (NOT HAS_PARENT)
    return()
  endif()

  # Copy the documentation if it exists
  if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/README.md)
    bout_copy_file(README.md)
  endif()

  # Copy the input file
  if (NOT BOUT_EXAMPLE_OPTIONS_DATA_DIRS)
    bout_copy_file(data/BOUT.inp)
  else()
    foreach (DATA_DIR IN LISTS BOUT_EXAMPLE_OPTIONS_DATA_DIRS)
      bout_copy_file(${DATA_DIR}/BOUT.inp)
    endforeach()
  endif()

  # Copy any other needed files
  if (BOUT_EXAMPLE_OPTIONS_EXTRA_FILES)
    foreach (FILE ${BOUT_EXAMPLE_OPTIONS_EXTRA_FILES})
      bout_copy_file("${FILE}")
    endforeach()
  endif()

  set_target_properties(${EXAMPLENAME} PROPERTIES FOLDER examples)
endfunction()


# Add a new integrated or MMS test. By default, the executable is
# named like the first source, stripped of its file extension.
#
# Required arguments:
#
# - TESTNAME: name of the test
#
# - SOURCES: list of source files
#
# Optional arguments:
#
# - USE_RUNTEST: if given, the test uses `./runtest` as the test
#   command, otherwise it uses the executable
#
# - USE_DATA_BOUT_INP: if given, copy `data/BOUT.inp`
#
# - EXTRA_FILES: any extra files that are required to run the test
#
# - REQUIRES: list of variables that must be truthy to enable test
#
# - EXECUTABLE_NAME: name of the executable, if different from the
#   first source name

function(bout_add_integrated_or_mms_test BUILD_CHECK_TARGET TESTNAME)
  set(options USE_RUNTEST USE_DATA_BOUT_INP)
  set(oneValueArgs EXECUTABLE_NAME)
  set(multiValueArgs SOURCES EXTRA_FILES REQUIRES TESTARGS)
  cmake_parse_arguments(BOUT_TEST_OPTIONS "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  foreach (REQUIREMENT IN LISTS BOUT_TEST_OPTIONS_REQUIRES)
    if (NOT ${REQUIREMENT})
      message(STATUS "Not building test ${TESTNAME}, requirement not met: ${REQUIREMENT}")
      return()
    endif()
  endforeach()

  add_executable(${TESTNAME} ${BOUT_TEST_OPTIONS_SOURCES})
  target_link_libraries(${TESTNAME} bout++)
  target_include_directories(${TESTNAME} PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

  # Set the name of the executable. We either take it as an option,
  # or use the first source file, stripping the file suffix
  if (BOUT_TEST_OPTIONS_EXECUTABLE_NAME)
    set_target_properties(${TESTNAME} PROPERTIES OUTPUT_NAME ${BOUT_TEST_OPTIONS_EXECUTABLE_NAME})
  else()
    # If more than one source file, just get the first one
    list(LENGTH ${BOUT_TEST_OPTIONS_SOURCES} BOUT_SOURCES_LENGTH)
    if (BOUT_SOURCES_LENGTH GREATER 0)
      list(GET ${BOUT_TEST_OPTIONS_SOURCES} 0 BOUT_TEST_FIRST_SOURCE)
    else()
      set(BOUT_TEST_FIRST_SOURCE ${BOUT_TEST_OPTIONS_SOURCES})
    endif()
    # Strip the directory and file extension from the source file
    get_filename_component(BOUT_TEST_EXECUTABLE_NAME ${BOUT_TEST_FIRST_SOURCE} NAME_WE)
    set_target_properties(${TESTNAME} PROPERTIES OUTPUT_NAME ${BOUT_TEST_EXECUTABLE_NAME})
  endif()

  # Set the actual test command
  if (BOUT_TEST_OPTIONS_USE_RUNTEST)
    add_test(NAME ${TESTNAME} COMMAND ./runtest ${BOUT_TEST_OPTIONS_TESTARGS})
    set_tests_properties(${TESTNAME} PROPERTIES
      ENVIRONMENT PYTHONPATH=${BOUT_PYTHONPATH}:$ENV{PYTHONPATH}
      )
    bout_copy_file(runtest)
  else()
    add_test(NAME ${TESTNAME} COMMAND ${TESTNAME} ${BOUT_TEST_OPTIONS_TESTARGS})
  endif()

  # Copy the input file if needed
  if (BOUT_TEST_OPTIONS_USE_DATA_BOUT_INP)
    bout_copy_file(data/BOUT.inp)
  endif()

  # Copy any other needed files
  if (BOUT_TEST_OPTIONS_EXTRA_FILES)
    foreach (FILE ${BOUT_TEST_OPTIONS_EXTRA_FILES})
      bout_copy_file("${FILE}")
    endforeach()
  endif()

  set_target_properties(${TESTNAME} PROPERTIES FOLDER tests/integrated)

  # Add the test to the build-check-integrated-tests target
  add_dependencies(${BUILD_CHECK_TARGET} ${TESTNAME})
endfunction()

# Add a new integrated test. See `bout_add_integrated_or_mms_test` for arguments
function(bout_add_integrated_test TESTNAME)
  bout_add_integrated_or_mms_test(build-check-integrated-tests ${TESTNAME} ${ARGV})
endfunction()

# Add a new MMS test. See `bout_add_integrated_or_mms_test` for arguments
function(bout_add_mms_test TESTNAME)
  bout_add_integrated_or_mms_test(build-check-mms-tests ${TESTNAME} ${ARGV})
endfunction()

# Add an alias for an imported target
# Workaround for CMAke < 3.18
# Taken from https://github.com/conan-io/conan/issues/2125#issuecomment-351176653
function(bout_add_library_alias dst src)
  add_library(${dst} INTERFACE IMPORTED)
  foreach(name INTERFACE_LINK_LIBRARIES INTERFACE_INCLUDE_DIRECTORIES INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS)
    get_property(value TARGET ${src} PROPERTY ${name} )
    set_property(TARGET ${dst} PROPERTY ${name} ${value})
  endforeach()
endfunction()


# Call nx-config with an argument, and append the resulting path to a list
# Taken from https://github.com/LiamBindle/geos-chem/blob/feature/CMake/CMakeScripts/FindNetCDF.cmake
function(bout_inspect_netcdf_config VAR NX_CONFIG ARG)
  execute_process(
    COMMAND ${NX_CONFIG} ${ARG}
    OUTPUT_VARIABLE NX_CONFIG_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if (NX_CONFIG_OUTPUT)
    set(${VAR} ${NX_CONFIG_OUTPUT} PARENT_SCOPE)
  endif()
endfunction()
