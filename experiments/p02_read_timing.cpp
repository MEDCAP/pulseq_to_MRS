// Probe 2: cost of a table-driven realtime loop.
//
// Tx runs:  seek(table); for (i < n) { at = read(); amp = read(); setamp(amp, at); }
// Rows are spaced by shrinking gaps. While the loop keeps up, each OSC line in
// the simulator log lands at requested at + fixed latency. Once the per-row
// overhead (2 reads + loop + branch) exceeds the gap, events start landing
// late (and engineTimingViolation may be set).
//
// Row amplitude = row index + 1, so each log line identifies its row.
// Usage: p02_read_timing [wordsPerRow]   (extra dummy words per row, default 0,
//        emulates a wider block-row format)
#include "probe_common.hpp"

#include <cstdlib>

int main(int argc, char** argv) {
  using namespace mrs::plato;
  using namespace mrs::plato::ppl;
  const int extraWords = argc > 1 ? std::atoi(argv[1]) : 0;

  // (gap in clocks, rows at that gap)
  const std::vector<std::pair<int, int>> schedule = {
      {1000, 4}, {500, 4}, {200, 4}, {100, 4}, {50, 4}, {30, 4}, {20, 4},
      {10, 4},   {5, 4}};

  std::vector<std::uint32_t> table;
  std::uint32_t at = 10000, row = 0;
  for (auto [gap, count] : schedule) {
    for (int k = 0; k < count; ++k) {
      at += gap;
      table.push_back(at);
      table.push_back(++row);
      for (int e = 0; e < extraWords; ++e) table.push_back(0xDEAD);
      std::printf("row %u gap %d at %u\n", row, gap, at);
    }
  }
  const int rows = static_cast<int>(row);

  _stream<fmt::Raw> tableStream(table.begin(), table.end());
  _variable i, t, amp, dummy;

  std::vector<StatementBuilder> body;
  body.push_back(t = read());
  body.push_back(amp = read());
  for (int e = 0; e < extraWords; ++e) body.push_back(dummy = read());
  body.push_back(setamp(amp, t));

  // clang-format off
  _sequence seq(
    _component("Tx", _function(
      seek(tableStream),
      _for (i = 0, i < rows, ++i) (
        _compound(body)
      )
    ))
  );
  // clang-format on
  probe::Session s;
  return s.Run(seq, {probe::Tx()});
}
