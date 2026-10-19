# Phase 1 unit tests (ctest). No hardware or simulator needed.
foreach(t test_units test_loader test_tables test_config)
  add_executable(${t} ${t}.cpp)
  target_link_libraries(${t} PRIVATE pp_host)
  target_compile_options(${t} PRIVATE -Wall -Wextra)
  target_compile_definitions(${t} PRIVATE
    PP_TEST_SEQ_DIR="${CMAKE_SOURCE_DIR}/test/seqs"
    PP_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config"
    PP_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")
  copy_toolchain_runtime(${t})
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# Phase 2: realtime program construction (PLATO headers, no spectrometer).
add_executable(test_program test_program.cpp)
target_link_libraries(test_program PRIVATE pp_plato)
target_compile_options(test_program PRIVATE -Wall -Wextra)
target_compile_definitions(test_program PRIVATE
  PP_TEST_SEQ_DIR="${CMAKE_SOURCE_DIR}/test/seqs")
plato_copy_runtime(test_program)
add_test(NAME test_program COMMAND test_program)

# Respiratory triggering: tables, settings and realtime programs. Writes its own
# small .seq, so it does not need the (untracked) test/ data folder.
add_executable(test_trigger test_trigger.cpp)
target_link_libraries(test_trigger PRIVATE pp_plato)
target_compile_options(test_trigger PRIVATE -Wall -Wextra)
target_compile_definitions(test_trigger PRIVATE
  PP_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")
plato_copy_runtime(test_trigger)
add_test(NAME test_trigger COMMAND test_trigger)
