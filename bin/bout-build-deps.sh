#!/usr/bin/env bash

test $PREFIX || PREFIX=$HOME/.local/bout-deps
test $BUILD || BUILD=$(pwd)/bout-deps
test $BOUT_TOP || BOUT_TOP=$(pwd)
test $MKFLAGS || MKFLAGS="-j 16"

HDF5VER=1.12.0
NCVER=4.7.4
NCCXXVER=4.3.1
FFTWVER=3.3.9
SUNVER=5.7.0
PETSCVER=3.15.0


setup() {
    mkdir -p $PREFIX
    mkdir -p $PREFIX/lib
	mkdir -p $PREFIX/bin
    if test -e $PREFIX/lib64 ; then
        if ! test -L $PREFIX/lib64 ; then
            echo "$PREFIX/lib64 exists and is not a symlink to lib - aborting"
            exit 1
        fi
    else
        ln -s lib $PREFIX/lib64
    fi
    mkdir -p $BUILD
}

hdf5() {
    cd $BUILD
    wget -c https://support.hdfgroup.org/ftp/HDF5/releases/hdf5-1.12/hdf5-${HDF5VER}/src/hdf5-${HDF5VER}.tar.bz2 || :
    tar -xvf hdf5-$HDF5VER.tar.bz2
    cd hdf5-${HDF5VER}
    ./configure --prefix $PREFIX --enable-build-mode=production
    make $MKFLAGS
    make install
}

netcdf() {
    cd $BUILD
    wget -c https://github.com/Unidata/netcdf-c/archive/v$NCVER/netcdf-$NCVER.tar.gz || :
    tar -xf netcdf-$NCVER.tar.gz
    cd netcdf-c-$NCVER
    CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib/" ./configure --prefix=$PREFIX
    make $MKFLAGS
    make install
}

nccxx() {
    cd $BUILD
    wget -c ftp://ftp.unidata.ucar.edu/pub/netcdf/netcdf-cxx4-$NCCXXVER.tar.gz || :
    tar -xf netcdf-cxx4-$NCCXXVER.tar.gz
    cd netcdf-cxx4-$NCCXXVER
    CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib/" ./configure --prefix=$PREFIX
    make $MKFLAGS
    make install
}

fftw() {
    cd $BUILD
    wget -c http://www.fftw.org/fftw-$FFTWVER.tar.gz || :
    tar -xf fftw-$FFTWVER.tar.gz
    cd fftw-$FFTWVER
    ./configure --prefix $PREFIX --enable-shared --enable-sse2 --enable-avx --enable-avx2 --enable-avx512 --enable-avx-128-fma
    make $MKFLAGS
    make install
}

sundials() {
    cd $BUILD
    wget -c https://github.com/LLNL/sundials/archive/v$SUNVER/sundials-$SUNVER.tar.gz || :
    tar -xvf sundials-$SUNVER.tar.gz
    cd sundials-$SUNVER
    mkdir -p build
    cd build
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX -DMPI_ENABLE=ON ..
    make $MKFLAGS
    make install
}

error () {
    echo "$@" >&2
    echo "$@"
    exit 1
}

petsc() {
    test $PETSC_DIR || error "\$PETSC_DIR is set - please unset"
    test $PETSC_ARCH || error "\$PETSC_ARCH is set - please unset"
    cd $BUILD
    wget -c https://ftp.mcs.anl.gov/pub/petsc/release-snapshots/petsc-$PETSCVER.tar.gz || :
    tar -xf petsc-$PETSCVER.tar.gz
    cd petsc-$PETSCVER
    unset PETSC_DIR
    ./configure COPTFLAGS="-O3" CXXOPTFLAGS="-O3" FOPTFLAGS="-O3" --with-batch --known-mpi-shared-libraries=1 --with-mpi-dir=$OPENMPI_HOME --download-fblaslapack \
        --known-64-bit-blas-indices=0 --download-hypre --with-debugging=0 --prefix=$PREFIX
    make $MKFLAGS
    make install
}

submod() {
    cd $BOUT_TOP
    git submodule update --init --recursive
}


info() {
set +x
    echo "Put this in a file in your module path"
    echo "#---------------------------"
    echo "#%Module 1.0
#
#  BOUT++ module for use with 'environment-modules' package


# Only allow one bout-dep module to be loaded at a time
conflict bout-dep
# Require all modules that where loaded at generation time
prereq $(echo $LOADEDMODULES | tr : \ )


setenv        BOUT_DEP         $PREFIX
prepend-path  PATH             $PREFIX/bin
prepend-path  LD_LIBRARY_PATH  $PREFIX/lib
"
    echo "#---------------------------"
    echo Run configure with:
    echo ./configure --with-netcdf=\$BOUT_DEP --with-sundials=\$BOUT_DEP --with-fftw=\$BOUT_DEP --with-petsc=\$BOUT_DEP
}

# Uncomment this if want to use it as a script to detect errors
set -ex

setup
hdf5
netcdf
nccxx
fftw
sundials
petsc
submod
info
