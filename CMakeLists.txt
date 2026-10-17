cmake_minimum_required(VERSION 3.20)
project(pulseq_plato CXX C)

# PLATO headers use concepts, <span> and operator<=>.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/Plato.cmake)

# --- Official Pulseq reader (MIT), vendored unmodified ----------------------
add_library(pulseq_reader STATIC
  pulseq/ExternalSequence.cpp
  pulseq/md5.cpp)
target_include_directories(pulseq_reader PUBLIC pulseq)
target_compile_definitions(pulseq_reader PUBLIC BUILD_PLATFORM_LINUX)  # '/' path separator
target_compile_options(pulseq_reader PRIVATE -w)                       # third-party: silence warnings

# pulseq/parsemr.cpp (upstream block-loop demo) is kept for
# reference only: at the pinned commit it does not compile against the same
# commit's ExternalSequence (computeLocalPhaseAndFrequencyOffsets signature).

# --- Phase 1: host core (plain C++, no PLATO headers) --------------------------
# seq/ loads .seq -> model/, units/ converts, shapes/ + tables/ build the
# per-channel waveform libraries and block-row tables the realtime loop reads.
add_library(pp_host STATIC
  src/units/units.cpp
  src/seq/seq_loader.cpp
  src/shapes/waveform_lib.cpp
  src/tables/channel_table.cpp
  src/tables/expect.cpp
  src/tables/export.cpp
  src/config/yaml_lite.cpp
  src/config/scan_settings.cpp)
target_include_directories(pp_host PUBLIC src)
target_link_libraries(pp_host PUBLIC pulseq_reader)
target_compile_options(pp_host PRIVATE -Wall -Wextra)

add_executable(pulseq_plato_dump src/tools/dump_main.cpp)
target_link_libraries(pulseq_plato_dump PRIVATE pp_host)
target_compile_options(pulseq_plato_dump PRIVATE -Wall -Wextra)
copy_toolchain_runtime(pulseq_plato_dump)

# --- Phase 2: realtime programs + PLATO host side ------------------------------
# realtime/ is the only code that builds PLATO syntax trees; plato/ talks to
# the modules (runner, acquisition pump, .cfl output).
add_library(pp_plato STATIC
  src/realtime/program.cpp
  src/plato/session.cpp)
target_include_directories(pp_plato PUBLIC src)
target_link_libraries(pp_plato PUBLIC pp_host plato::corelib)
target_compile_options(pp_plato PRIVATE -Wall -Wextra)

add_executable(pulseq_plato src/main.cpp)
target_link_libraries(pulseq_plato PRIVATE pp_plato)
target_compile_options(pulseq_plato PRIVATE -Wall -Wextra)
plato_copy_runtime(pulseq_plato)

enable_testing()
add_subdirectory(unit_tests)

# --- Phase 0: simulator probes ------------------------------------------------
add_subdirectory(experiments)
