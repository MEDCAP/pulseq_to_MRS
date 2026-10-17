#include "tables/export.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace pp::tables {

namespace {
constexpr double kPi = 3.14159265358979323846;
}  // namespace

bool WriteWaveforms(const std::string& path, const ChannelTables& t,
                    const ScanConfig& cfg) {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return false;
  const double clk = cfg.clocks.rtClockHz;
  // Absolute clock -> seconds from sequence t=0.
  auto sec = [&](long long c) {
    return static_cast<double>(c - cfg.startOffset) / clk;
  };
  std::fprintf(f, "channel,t_s,value,extra\n");

  // RF: amplitude fraction * rfmax. Phase = shape phase + row setph word +
  // the offset phase earlier pulses made the NCO integrate (which the row word
  // subtracts, channel_table.cpp BuildTx) + this pulse's own offset ramp, so
  // it is referenced to the continuous carrier at -f, as Pulseq means it.
  const auto txBase = EpochBases(t.tx, cfg);
  const auto& pts = t.rfLib.points();
  double accPhase = 0;
  for (std::size_t i = 0; i < t.tx.size(); ++i) {
    const auto& r = t.tx[i];
    if (r.kind() != RowKind::kRf) continue;
    const long long b = txBase[i];
    const double rowPhase = 2 * kPi * r.w[7] / 65536.0 + accPhase;
    const std::size_t p0 = r.w[2] / shapes::RfLibrary::kBytesPerPoint;
    const double offHz = pts[p0].frequency * cfg.clocks.rfClockHz / 2;
    std::fprintf(f, "gate,%.9g,1,\n", sec(b + r.w[5]));
    for (std::uint32_t j = 0; j <= r.w[3]; ++j) {
      const auto& q = pts[p0 + j];
      const double dt = static_cast<double>(j) * r.w[4] / clk;
      // at is the first sample centre (point j holds for one raster around it).
      const long long centre = b + r.at() + static_cast<long long>(j) * r.w[4];
      std::fprintf(f, "rf,%.9g,%.9g,%.9g\n", sec(centre),
                   q.amplitude * cfg.rfMaxHz,
                   q.phase * kPi + rowPhase + 2 * kPi * offHz * dt);
    }
    std::fprintf(f, "gate,%.9g,0,\n", sec(b + r.w[6]));
    accPhase += 2 * kPi * offHz * static_cast<double>(r.w[3]) * r.w[4] / clk;
  }

  for (int axis = 0; axis < 3; ++axis) {
    const auto& rows = t.grad[axis];
    const auto gb = EpochBases(rows, cfg);
    const char* name = axis == 0 ? "gx" : axis == 1 ? "gy" : "gz";
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& r = rows[i];
      if (r.kind() != RowKind::kGrad) continue;
      const auto* v =
          &t.gradLib.values()[r.w[2] / shapes::GradLibrary::kBytesPerPoint];
      for (std::uint32_t j = 0; j <= r.w[3]; ++j)
        std::fprintf(f, "%s,%.9g,%.9g,\n", name,
                     sec(gb[i] + r.at() + static_cast<long long>(j) * r.w[4]),
                     v[j] * cfg.gradMaxHzPerM[axis]);
    }
  }

  // ADC: latches fall at step ends (at + (k+1)·dwell); the sample stands for
  // the centre of its dwell step (README, ADC convention).
  const auto rxBase = EpochBases(t.rx, cfg);
  for (std::size_t i = 0; i < t.rx.size(); ++i) {
    const auto& r = t.rx[i];
    if (r.kind() != RowKind::kAdc) continue;
    const double offHz =
        r.w[6] ? static_cast<std::int32_t>(r.w[4] - t.baseFreqWord) *
                     cfg.clocks.rfClockHz / 4294967296.0
               : 0.0;
    const double ph = 2 * kPi * r.w[5] / 65536.0;
    for (std::uint32_t k = 0; k < r.w[2]; ++k)
      std::fprintf(f, "adc,%.9g,%.9g,%.9g\n",
                   sec(rxBase[i] + r.at()) + (k + 0.5) * r.w[3] / clk, offHz,
                   ph);
  }
  std::fclose(f);
  return true;
}

}  // namespace pp::tables
