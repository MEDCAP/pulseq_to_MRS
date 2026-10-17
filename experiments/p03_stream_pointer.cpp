// Probe 3: stream addressing, and whether rf(address, ...) moves the stream
// pointer used by read().
//
// lib = one fmt::Modulation stream holding two 4-point segments:
//         seg A amplitude 0.25 (~8192), seg B amplitude 0.75 (~24575)
// table = {111, 222, 333}
//
// Tx:  seek(table); a = read();                 setamp(a, 1000)
//      rf(GetStreamAddress(lib) + OFFSET, 4, 100, 2000)
//      b = read();                               setamp(b, 5000)
//      seek(table, SEEKOFF); c = read();         setamp(c, 6000)
//
// Read the log:
//  - amplitude played from 2000: 8192 -> OFFSET points at seg A, 24575 -> seg B.
//    Seg B starts 4 points = 8 words = 32 bytes after seg A, so try OFFSET
//    0 / 4 / 8 / 32 to learn the address unit.
//  - OSC at 5000: 222 -> rf() did not disturb the read pointer; anything else
//    -> it did, and the block loop must re-seek after every waveform.
//  - OSC at 6000: which element seek(table, SEEKOFF) lands on (offset unit).
// Usage: p03_stream_pointer OFFSET SEEKOFF
#include "probe_common.hpp"

#include <cstdlib>

int main(int argc, char** argv) {
  using namespace mrs::plato;
  using namespace mrs::plato::ppl;
  const int offset = argc > 1 ? std::atoi(argv[1]) : 0;
  const int seekOff = argc > 2 ? std::atoi(argv[2]) : 2;

  _stream<fmt::Modulation> lib;
  lib.AppendSegment(4, [](std::size_t) {
    return fmt::Modulation::ValueType{0.25, 0, 0};
  });
  lib.AppendSegment(4, [](std::size_t) {
    return fmt::Modulation::ValueType{0.75, 0, 0};
  });
  _stream<fmt::Raw> table({111u, 222u, 333u});
  _variable a, b, c;

  // clang-format off
  _sequence seq(
    _component("Tx", _function(
      setscale(32767),
      seek(table),
      a = read(),
      setamp(a, 1000),
      rf(GetStreamAddress(lib.get()) + offset, 4, 100, 2000),
      b = read(),
      setamp(b, 5000),
      seek(table, seekOff),
      c = read(),
      setamp(c, 6000),
      setamp(0, 7000)
    ))
  );
  // clang-format on
  std::printf("OFFSET=%d SEEKOFF=%d\n", offset, seekOff);
  probe::Session s;
  return s.Run(seq, {probe::Tx()});
}
