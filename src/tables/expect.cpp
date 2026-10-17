#include "tables/expect.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace pp::tables {

// Expected first log line of every row, for test/compare.py expect:
//   <log file index> <command> <logged time> <allowed lateness>
// Logged time = absolute requested time + 10 (engine latency, FINDINGS T1);
// row times count from the channel's last origin(), so each channel's epoch
// base is tracked (origin(at) moves it to base + at + originLatencyClk, T10).
// Log file index = channel index of the simulator's --multiple-files split
// (V5): Tx, Rx and GA are 0, GB 1, GC 2.
bool WriteExpect(const std::string& path, const ChannelTables& t,
                 const ScanConfig& cfg) {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return false;
  using shapes::QuantizeInt16;
  using shapes::QuantizeInt32;
  constexpr long long kLatency = 10;
  std::fprintf(f, "# file command time lateness row\n");
  auto line = [&](int file, const char* cmd, long long t, const char* ch,
                  std::size_t i) {
    std::fprintf(f, "%d %s %lld 0 %s%zu\n", file, cmd, t + kLatency, ch, i);
  };
  // Epoch base before row i, per channel.
  auto bases = [&](const std::vector<Row>& rows) {
    return EpochBases(rows, cfg);
  };

  // The simulator logs outputs only when they change, so a waveform row is
  // checked at its first point that differs from the value held before it.
  const auto txBase = bases(t.tx);
  for (std::size_t i = 0; i < t.tx.size(); ++i) {
    const auto& r = t.tx[i];
    if (r.kind() != RowKind::kRf) continue;
    const long long b = txBase[i];
    line(0, "GATE1", b + r.w[5], "tx", i);
    // Every RF waveform ends with the (0, 0, 0) point, so that is held.
    const auto& pts = t.rfLib.points();
    for (std::uint32_t j = 0; j <= r.w[3]; ++j) {
      const auto& q = pts[r.w[2] / shapes::RfLibrary::kBytesPerPoint + j];
      if (QuantizeInt16(q.amplitude) || QuantizeInt16(q.phase) ||
          QuantizeInt32(q.frequency)) {
        line(0, "OSC", b + r.at() + static_cast<long long>(j) * r.w[4], "tx", i);
        break;
      }
    }
    line(0, "GATE0", b + r.w[6], "tx", i);
  }
  for (int axis = 0; axis < 3; ++axis) {
    const auto& rows = t.grad[axis];
    const auto gb = bases(rows);
    const char ch[3] = {'g', "xyz"[axis], 0};
    std::int32_t held = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& r = rows[i];
      if (r.kind() != RowKind::kGrad) continue;
      // The simulator logs gradient values through its own (non-linear)
      // mapping (FINDINGS G3), so which point first shows as a change can't
      // be predicted exactly. Instead the row's first logged line must lie on
      // its raster grid at + j * period: a late start moves every point off
      // the grid. Rows that never change the output are skipped.
      const auto* v =
          &t.gradLib.values()[r.w[2] / shapes::GradLibrary::kBytesPerPoint];
      // The peak (DAC counts) lets the checker tell a row too small to show
      // in the simulator log (G3: ~4 counts vanish) from a missing one.
      bool changes = false;
      std::int32_t peak = 0;
      for (std::uint32_t j = 0; j <= r.w[3]; ++j) {
        changes |= QuantizeInt16(v[j]) != held;
        held = QuantizeInt16(v[j]);
        peak = std::max(peak, held < 0 ? -held : held);
      }
      if (changes)
        std::fprintf(f, "%d GRAD %lld 0 %s%zu grid %u %u %d\n", axis,
                     gb[i] + r.at() + kLatency, ch, i, r.w[4], r.w[3], peak);
    }
  }
  const auto rxBase = bases(t.rx);
  for (std::size_t i = 0; i < t.rx.size(); ++i) {
    const auto& r = t.rx[i];
    if (r.kind() != RowKind::kAdc) continue;
    const long long b = rxBase[i];
    // acquire2 resets the CIC grid when it executes, which should be `at`
    // (logged as GRID grid-1, FINDINGS R2/R5), and takes its first sample at
    // `at + dwell` (R1).
    const std::string grid = "GRID" + std::to_string(r.w[7] - 1);
    line(0, grid.c_str(), b + r.at(), "rx", i);
    line(0, "ACQ", b + r.at() + r.w[3], "rx", i);
    if (r.w[6] != 0)  // frequency restore, also logged as a GRID line (R6)
      line(0, grid.c_str(), b + r.w[6], "rx", i);
  }
  std::fclose(f);
  return true;
}

}  // namespace pp::tables
