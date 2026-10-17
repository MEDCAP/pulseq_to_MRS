# Imports the PLATO SDK (C:\mrs\plato) by hand: the SDK ships no CMake config.
#
#   plato::corelib  - libcorelib.dll (import lib lib/libcorelib.dll.a) + ws2_32
#   plato::seqlib   - static lib/libseqlib.a (gradient_utils, sequence_utils, ...)
#
# plato_copy_runtime(<target>) copies libcorelib.dll plus the toolchain's C++
# runtime (libstdc++, libgcc_s, winpthread from SMIS_RUNTIME_DIR, see
# cmake/toolchain-smis-ucrt64.cmake) next to the executable, so it runs with no
# PATH setup and can never pick up the GCC 16 runtime from C:\msys64.
#
# copy_toolchain_runtime(<target>) does only the C++ runtime part (for
# executables that do not use PLATO, e.g. unit tests).

set(plato_dir "C:/mrs/plato" CACHE PATH "PLATO SDK root (contains include/, lib/, bin/)")

if(NOT EXISTS "${plato_dir}/include/spectrometer.hpp")
  message(FATAL_ERROR "plato_dir='${plato_dir}' does not look like a PLATO SDK")
endif()

add_library(plato::corelib SHARED IMPORTED)
set_target_properties(plato::corelib PROPERTIES
  IMPORTED_IMPLIB "${plato_dir}/lib/libcorelib.dll.a"
  IMPORTED_LOCATION "${plato_dir}/bin/libcorelib.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${plato_dir}/include"
  INTERFACE_LINK_LIBRARIES "ws2_32")

add_library(plato::seqlib STATIC IMPORTED)
set_target_properties(plato::seqlib PROPERTIES
  IMPORTED_LOCATION "${plato_dir}/lib/libseqlib.a"
  INTERFACE_LINK_LIBRARIES "plato::corelib")

if(NOT SMIS_RUNTIME_DIR)
  message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-smis-ucrt64.cmake")
endif()
if(NOT CMAKE_CXX_COMPILER_VERSION MATCHES "^13\\." OR
   NOT CMAKE_CXX_COMPILER MATCHES "^C:/smis/")
  message(FATAL_ERROR "Expected the C:/smis g++ 13.x, got ${CMAKE_CXX_COMPILER} "
                      "${CMAKE_CXX_COMPILER_VERSION}. Delete build/ and configure "
                      "with the smis toolchain file.")
endif()

function(copy_toolchain_runtime target)
  foreach(dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${SMIS_RUNTIME_DIR}/${dll}" "$<TARGET_FILE_DIR:${target}>")
  endforeach()
endfunction()

set(PLATO_SIMULATOR "${plato_dir}/bin/simulator.exe")
set(PLATO_BIN_DIR "${plato_dir}/bin")

function(plato_copy_runtime target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${plato_dir}/bin/libcorelib.dll" "$<TARGET_FILE_DIR:${target}>")
  copy_toolchain_runtime(${target})
endfunction()
