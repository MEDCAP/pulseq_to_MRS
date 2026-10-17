// Probe 6: prototype of the Tx block loop, measuring its minimum event gap.
//
// Facts from probes 3/5 baked in:
//   - stream addresses and seek offsets are in BYTES
//   - rf(address, N, ...) plays N+1 points  -> rows store points-1
//   - rf(address, ...) moves the channel's single stream pointer
//     -> the loop re-seeks the table (tableAddr + pos) at the top of each row
//
// Row = {at, libByteOffset, pointsMinus1}. Each row plays a 2-point pulse
// (duration 200 clocks at period 100) followed by a gap that shrinks.
// A row is on time if its first OSC line is at `at + 10`.
// The host prints the expected times; compare with the log.
// Usage: p06_tx_block_loop [dwellBetweenReads]  (unused, reserved)
#include "probe_common.hpp"

int main() {
  using namespace mrs::plato;
  using namespace mrs::plato::ppl;

  // lib: 16 distinct 2-point pulses so every row is identifiable in the log.
  // (AppendSegment generators must be default-constructible, i.e. capture-less
  //  lambdas, so build the samples on the host and use the iterator form.)
  std::vector<fmt::Modulation::ValueType> pts;
  for (int k = 0; k < 16; ++k)
    for (int i = 0; i < 2; ++i)
      pts.push_back({(k + 1) / 20.0 + i * 0.01, 0, 0});
  _stream<fmt::Modulation> lib(pts.begin(), pts.end());

  const int gaps[] = {2000, 1000, 500, 300, 200, 150, 100, 80,
                      60,   50,   40,  30,  20,  10,  5,   0};
  std::vector<std::uint32_t> table;
  std::uint32_t at = 10000;
  for (int k = 0; k < 16; ++k) {
    table.push_back(at);
    table.push_back(k * 2 * 8);  // 2 points * 8 bytes per point
    table.push_back(2 - 1);
    std::printf("row %2d at %6u (expect OSC at %u) gapBefore %d\n", k, at,
                at + 10, k ? gaps[k - 1] : -1);
    at += 200 + gaps[k];
  }
  const int rows = 16;
  constexpr int kRowBytes = 3 * 4;

  _stream<fmt::Raw> tableStream(table.begin(), table.end());
  _variable i, pos, t, off, n;

  // clang-format off
  _sequence seq(
    _component("Tx", _function(
      setscale(32767),
      seek(lib),                 // make sure lib is loaded
      pos = 0,
      _for (i = 0, i < rows, ++i) (
        seek(GetStreamAddress(tableStream.get()) + pos),
        t = read(),
        off = read(),
        n = read(),
        rf(GetStreamAddress(lib.get()) + off, n, 100, t),
        pos += kRowBytes
      ),
      setamp(0, at + 1000),
      ppl::exit(0)
    ))
  );
  // clang-format on
  probe::Session s;
  return s.Run(seq, {probe::Tx()});
}
