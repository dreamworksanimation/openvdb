![OpenVDB](doc/img/banner.png)

| OpenVDB |   AX   |  Nano  | Houdini | License | CII |
| :----:  | :----: | :----: |  :----: | :-----: | :-: |
| [![core](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/build.yml/badge.svg)](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/build.yml) | [![ax](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/ax.yml/badge.svg)](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/ax.yml) | [![nano](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/nanovdb.yml/badge.svg)](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/nanovdb.yml) | [![hou](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/houdini.yml/badge.svg)](https://github.com/AcademySoftwareFoundation/openvdb/actions/workflows/houdini.yml) | [![License](https://img.shields.io/github/license/AcademySoftwareFoundation/openvdb)](LICENSE) | [![CII Best Practices](https://bestpractices.coreinfrastructure.org/projects/2774/badge)](https://bestpractices.coreinfrastructure.org/projects/2774) |

-------------------------------------------------------------------------------

[Website](https://www.openvdb.org) |
[Discussion Forum](https://github.com/AcademySoftwareFoundation/openvdb/discussions) |
[Documentation](https://www.openvdb.org/documentation/doxygen) |
[Releases](https://github.com/AcademySoftwareFoundation/openvdb/releases) |
[License](https://www.mozilla.org/MPL/2.0) |
[Slack](https://slack.aswf.io/)

OpenVDB is an open source C++ library comprising a novel hierarchical data
structure and a large suite of tools for the efficient storage and manipulation
of sparse volumetric data discretized on three-dimensional grids. It was
developed by DreamWorks Animation for use in volumetric applications typically
encountered in feature film production.

### Development Repository

This GitHub repository hosts the trunk of the OpenVDB development. This implies
that it is the newest public version with the latest features and bug fixes.
However, it also means that it has not undergone a lot of testing and is
generally less stable than the [production releases](https://github.com/AcademySoftwareFoundation/openvdb/releases).

### License

OpenVDB is released under the [Mozilla Public License Version 2.0](https://www.mozilla.org/MPL/2.0/),
which is a free, open source software license developed and maintained by the
Mozilla Foundation.

The trademarks of any contributor to this project may not be used in
association with the project without the contributor's express permission.

NOTE: OpenVDB is in the process of changing its license from [Mozilla Public License Version 2.0](https://www.mozilla.org/MPL/2.0/) to [Apache License, Version 2.0](https://www.apache.org/licenses/LICENSE-2.0)! Please see the file RE-LICENSE_NOTE.txt for more details.

### Contributing

OpenVDB welcomes contributions to the OpenVDB project. Please refer to the
[contribution guidelines](CONTRIBUTING.md) for details on how to make a
contribution.

-------------------------------------------------------------------------------

### Developer Quick Start

The following provides basic installation examples for the core OpenVDB library.
Other components, such as the python module, OpenVDB AX, NanoVDB and various
executables, may require additional dependencies. See the
[build documentation](https://www.openvdb.org/documentation/doxygen/build.html)
for help with installations.

##### Linux/MacOS

```bash
# Linux
# @note If your distribution does not have required versions, consider using
#   apt pinning. See the dependency documentation for more details.
apt-get install -y libboost-iostreams-dev
apt-get install -y libtbb-dev
apt-get install -y libblosc-dev

# MacOS
# @note We are using homebrew in this example to install requried dependencies
#  https://brew.sh/
brew install boost
brew install tbb
brew install c-blosc
```

```bash
git clone git@github.com:AcademySoftwareFoundation/openvdb.git
cd openvdb
mkdir build
cd build
cmake ..
make -j4 && make install
```

##### Windows

Note that the following commands have only been tested for 64bit systems/libraries.
It is recommended to set the `VCPKG_DEFAULT_TRIPLET` environment variable to
`x64-windows` to use 64-bit libraries by default. You will also require
[Visual Studio](https://visualstudio.microsoft.com/downloads/) (for the MSVC C++
runtime and compiler toolchains), [CMake](https://cmake.org/download/) and optionally
[vcpkg](https://github.com/microsoft/vcpkg) for the installation of OpenVDB's
dependencies.

```bash
vcpkg install zlib:x64-windows
vcpkg install blosc:x64-windows
vcpkg install tbb:x64-windows
vcpkg install boost-iostreams:x64-windows
vcpkg install boost-any:x64-windows
vcpkg install boost-algorithm:x64-windows
vcpkg install boost-interprocess:x64-windows
```

```bash
git clone git@github.com:AcademySoftwareFoundation/openvdb.git
cd openvdb
mkdir build
cd build
cmake -DCMAKE_TOOLCHAIN_FILE=<PATH_TO_VCPKG>\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows -A x64 ..
cmake --build . --parallel 4 --config Release --target install
```

#### Building OpenVDB AX and NanoVDB

OpenVDB AX depends on the core OpenVDB library. NanoVDB can be built with and
without OpenVDB support. Note that NanoVDB has its own build instructions, see
the [NanoVDB build documentation](https://www.openvdb.org/documentation/doxygen/NanoVDB_HowToBuild.html)
for details.

The following variables can be passed to the `cmake` configure command. There
are more optional VDB components, see the [build documentation](https://www.openvdb.org/documentation/doxygen/build.html)
for a complete list.

| Option | Details |
| :----  | :-----  |
| `-D OPENVDB_BUILD_AX=ON`      | to enable OpenVDB AX |
| `-D OPENVDB_BUILD_NANOVDB=ON` | to enable NanoVDB |
| `-D NANOVDB_USE_OPENVDB=ON`   | to use OpenVDB in NanoVDB |
