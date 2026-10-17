# Phase 0 probes: each is a tiny host program run against simulator.exe
# (see test/run_probe.sh). They answer timing/addressing questions the
# realtime block loop depends on.
foreach(p p01_hello p02_read_timing p03_stream_pointer p04_setamp_latch p05_addr_semantics p06_tx_block_loop p07_gradient p08_adc p09_origin)
  add_executable(${p} ${p}.cpp)
  target_link_libraries(${p} PRIVATE plato::corelib)
  plato_copy_runtime(${p})
endforeach()

# Probe 10 drives the real host core and realtime programs (pp_plato).
add_executable(p10_gap_stress p10_gap_stress.cpp)
target_link_libraries(p10_gap_stress PRIVATE pp_plato)
plato_copy_runtime(p10_gap_stress)
