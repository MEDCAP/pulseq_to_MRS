# Pins the build to the vendor's MSYS2 UCRT64 toolchain in C:\smis\msys64\ucrt64
# (g++ 13.1, ninja 1.11). That is the runtime family libcorelib.dll and the
# vendor pulseq.exe were built against; the local C:\msys64 GCC 16.1 libstdc++
# no longer exports symbols those binaries import (see test/FINDINGS.md).
#
# Configure (Git Bash):
#   export PATH=/c/smis/msys64/ucrt64/bin:$PATH
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-smis-ucrt64.cmake
#
# C:\smis is the vendor installation: this project only reads from it.

set(SMIS_UCRT64 "C:/smis/msys64/ucrt64" CACHE PATH "Vendor MSYS2 UCRT64 prefix")

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER "${SMIS_UCRT64}/bin/gcc.exe")
set(CMAKE_CXX_COMPILER "${SMIS_UCRT64}/bin/g++.exe")
set(CMAKE_RC_COMPILER "${SMIS_UCRT64}/bin/windres.exe")
set(CMAKE_MAKE_PROGRAM "${SMIS_UCRT64}/bin/ninja.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_FIND_ROOT_PATH "${SMIS_UCRT64}")

# Directory holding the runtime DLLs copied next to every executable.
set(SMIS_RUNTIME_DIR "${SMIS_UCRT64}/bin" CACHE PATH "" FORCE)
