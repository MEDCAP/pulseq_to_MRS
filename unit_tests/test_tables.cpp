#include "check.hpp"
#include "seq/seq_loader.hpp"
#include "tables/channel_table.hpp"
#include "tables/export.hpp"

#include <algorithm>
#include <cstdio>
#include <numbers>
#include <string>

using namespace pp;
using tables::RowKind;

static const std::string kDir = PP_TEST_SEQ_DIR;

static tables::ScanConfig ReferenceConfig() {
  // Same parameters as the vendor reference run (FINDINGS.md V*).
  tables::ScanConfig c;
  c.larmorHz = 27995000;
  c.rfMaxHz = 100;
  c.gradMaxHzPerM = {4.2e6, 4.2e6, 4.2e6};
  return c;
}

static model::SequenceModel Load(const char* file) {
  seq::LoadOptions opt;
  opt.larmorHz = 27995000;
  return seq::LoadSequence(kDir + "/" + file, opt);
}

static void Fid() {
  const auto cfg = ReferenceConfig();
  const auto t = tables::BuildTables(Load("fid_v150.seq"), cfg);

  // Tx: one RF row + END. RF samples are centred: at = 10000 + 0 + 50.
  CHECK_EQ(t.tx.size(), 2u);
  CHECK(t.tx[0].kind() == RowKind::kRf);
  CHECK_EQ(t.tx[0].at(), 10050u);
  CHECK_EQ(t.tx[0].w[2], 0u);        // first waveform in the library
  CHECK_EQ(t.tx[0].w[3], 100u);      // 100 samples + trailing zero, minus 1
  CHECK_EQ(t.tx[0].w[4], 100u);      // 1 us raster
  CHECK_EQ(t.tx[0].w[5], 10000u);    // gate on at the RF event start
  CHECK_EQ(t.tx[0].w[6], 10050u + 100u * 100u + 50u);  // ... off after the zero
  CHECK_EQ(t.rfLib.points().size(), 101u);
  CHECK_NEAR(t.rfLib.points()[0].amplitude, 0.5, 1e-9);
  CHECK_NEAR(t.rfLib.points()[99].amplitude, 0.5, 1e-9);
  CHECK_NEAR(t.rfLib.points()[100].amplitude, 0.0, 0);
  CHECK(t.tx[1].kind() == RowKind::kEnd);
  CHECK_EQ(t.tx[1].at(), 10000u + 301000u);

  // GC: the trapezoid, 121 edge points starting at the block start.
  CHECK_EQ(t.grad[2].size(), 2u);
  CHECK_EQ(t.grad[2][0].at(), 31000u);
  CHECK_EQ(t.grad[2][0].w[3], 120u);
  CHECK_EQ(t.grad[2][0].w[4], 1000u);
  CHECK_NEAR(t.gradLib.values()[10], 100000 / 4.2e6, 1e-12);
  CHECK_NEAR(t.gradLib.values().back(), 0, 0);
  CHECK_EQ(t.grad[0].size(), 1u);  // only END
  CHECK_EQ(t.grad[1].size(), 1u);

  // Rx: the first sample integrates the dwell step [33000, 34000] and is
  // latched at its end, at + dwell = 34000. (The vendor latches at the step
  // centre instead: 33559 - 49 - 10 = 33500.)
  CHECK_EQ(t.rx.size(), 2u);
  const auto& adc = t.rx[0];
  CHECK(adc.kind() == RowKind::kAdc);
  CHECK_EQ(adc.at(), 33000u);
  CHECK_EQ(adc.at() + adc.w[3], 34000u);
  CHECK_EQ(adc.w[2], 256u);
  CHECK_EQ(adc.w[3], 1000u);
  CHECK_EQ(adc.w[4], t.baseFreqWord);  // no offset
  CHECK_EQ(adc.w[6], 0u);              // nothing to restore
  CHECK_EQ(adc.w[7], 1000u);           // grid = dwell with CIC comp off

  CHECK(t.timingIssues.empty());
  CHECK(t.warnings.empty());

  auto cic = cfg;
  cic.cicCompensation = true;
  const auto tc = tables::BuildTables(Load("fid_v150.seq"), cic);
  CHECK_EQ(tc.rx[0].w[7], 250u);

  const auto words = tables::Flatten(t.tx);
  CHECK_EQ(words.size(), 16u);
  CHECK_EQ(words[1], 10050u);
}

static void Features() {
  const auto cfg = ReferenceConfig();
  const auto t = tables::BuildTables(Load("features_v150.seq"), cfg);

  // RF with offsets: phase 0.5 rad in the row's setph word (the waveform keeps
  // the shape phase, 0), freq field = 1000 / 200 MHz.
  CHECK_EQ(t.tx[0].at(), 10000u + 1000u + 50u);
  CHECK_EQ(t.tx[0].w[3], 4u);
  CHECK_NEAR(t.rfLib.points()[0].phase, 0, 0);
  CHECK_EQ(t.tx[0].w[7], units::PhaseWord16(0.5));
  CHECK_NEAR(t.rfLib.points()[0].frequency, 1000 / 200e6, 1e-15);
  CHECK_NEAR(t.rfLib.points()[4].frequency, 0, 0);  // trailing zero resets it

  // x arbitrary gradient: 5 edge points, ends at 0 -> no extra point.
  CHECK_EQ(t.grad[0].size(), 2u);
  CHECK_EQ(t.grad[0][0].w[3], 4u);

  // y: the two extended trapezoids meet at 32000 -> first loses its last point.
  CHECK_EQ(t.grad[1].size(), 3u);
  CHECK_EQ(t.grad[1][0].at(), 10000u + 30000u);
  CHECK_EQ(t.grad[1][0].w[3], 1u);  // 2 points: 0, 30k
  CHECK_EQ(t.grad[1][1].at(), 10000u + 32000u);
  CHECK_EQ(t.grad[1][1].w[3], 2u);  // 3 points: 30k, 30k, 0
  const std::uint32_t second = t.grad[1][1].w[2] / 4;
  CHECK_NEAR(t.gradLib.values()[second], 30000 / 4.2e6, 1e-12);

  // ADC with a 500 Hz offset: absolute NCO word, phase word for 0.25 rad
  // (no earlier ADC, so no accumulated offset phase), restore at the end.
  const auto& adc = t.rx[0];
  CHECK_EQ(adc.at(), 10000u + 35000u);
  CHECK_EQ(adc.w[4], units::FrequencyWord(27995500, cfg.clocks));
  CHECK_EQ(adc.w[5], 2608u);
  CHECK_EQ(adc.w[6], 10000u + 35000u + 8u * 200u + 50u);
  CHECK(t.timingIssues.empty());
}

static model::RfEvent Pulse(int block, model::Clk start, double foff) {
  model::RfEvent rf;
  rf.block = block;
  rf.start = start;
  rf.rasterClk = 100;
  rf.amplitudeHz = 25;
  rf.magnitude.assign(10, 1.0f);
  rf.phaseRad.assign(10, 0.0f);
  rf.freqOffsetHz = foff;
  return rf;
}

static void Synthetic() {
  const auto cfg = ReferenceConfig();
  const double pi = std::numbers::pi;

  // Identical pulses share one library entry.
  model::SequenceModel m;
  m.duration = 100000;
  m.rf = {Pulse(0, 0, 0), Pulse(1, 50000, 0)};
  auto t = tables::BuildTables(m, cfg);
  CHECK_EQ(t.rfLib.unique(), 1u);
  CHECK_EQ(t.tx[0].w[2], t.tx[1].w[2]);

  // A frequency offset makes the NCO integrate extra phase (10 points +
  // trailing zero = 10 intervals of 1 us): the next pulse subtracts it.
  m.rf = {Pulse(0, 0, 10000), Pulse(1, 50000, 0)};
  t = tables::BuildTables(m, cfg);
  CHECK_EQ(t.rfLib.unique(), 2u);
  const double acc = 2 * pi * 10000 * 10e-6;  // 0.2 pi
  const std::uint32_t p2 = t.tx[1].w[2] / 8;
  CHECK_NEAR(t.rfLib.points()[p2].phase, 0, 0);
  CHECK_EQ(t.tx[1].w[7], units::PhaseWord16(-acc));

  // Pulses that differ only in phase (RF spoiling) share one waveform.
  m.rf = {Pulse(0, 0, 0), Pulse(1, 50000, 0)};
  m.rf[1].phaseOffsetRad = 2.04204;
  t = tables::BuildTables(m, cfg);
  CHECK_EQ(t.rfLib.unique(), 1u);
  CHECK_EQ(t.tx[1].w[7], units::PhaseWord16(2.04204));

  // ADC frequency offsets: each ADC's phase word only compensates what
  // *earlier* ADCs accumulated (regression: the first ADC used to include its
  // own share). ADC 1: 1 kHz offset over [at - lead, last latch + hold] =
  // 51 + 10 samples * 1000 clk + 50 = 10101 clk -> 2 pi * 1e3 * 101.01e-6 rad.
  {
    model::SequenceModel ms;
    ms.duration = 200000;
    model::AdcEvent a1;
    a1.block = 0;
    a1.start = 1000;
    a1.numSamples = 10;
    a1.dwellClk = 1000;
    a1.freqOffsetHz = 1000;
    a1.phaseOffsetRad = 0.5;
    model::AdcEvent a2 = a1;
    a2.block = 1;
    a2.start = 100000;
    a2.freqOffsetHz = 0;
    a2.phaseOffsetRad = 0;
    ms.adc = {a1, a2};
    const auto ta = tables::BuildTables(ms, cfg);
    CHECK_EQ(ta.rx[0].w[5], units::PhaseWord16(0.5));
    CHECK_EQ(ta.rx[1].w[5], units::PhaseWord16(-2 * pi * 1e3 * 101.01e-6));
    CHECK_EQ(ta.rx[1].w[4], ta.baseFreqWord);
    CHECK_EQ(ta.rx[1].w[6], 0u);
  }

  // Two pulses too close together for the loop: reported, not fatal.
  m.rf = {Pulse(0, 0, 0), Pulse(1, 1000 + 20, 0)};
  t = tables::BuildTables(m, cfg);
  CHECK_EQ(t.timingIssues.size(), 1u);

  // Over full scale: clipped with a warning up to 1 %, refused beyond.
  m.rf = {Pulse(0, 0, 0)};
  m.rf[0].amplitudeHz = 150;
  CHECK_THROWS(tables::BuildTables(m, cfg), tables::TableError);
  m.rf[0].amplitudeHz = 100.342;  // gre_base.seq
  t = tables::BuildTables(m, cfg);
  CHECK_EQ(t.warnings.size(), 1u);
  CHECK_NEAR(t.rfLib.points()[0].amplitude, 1.0, 0);

}

// Absolute clock of word k of every row (base + value; ORIGIN moves the base).
static std::vector<long long> Absolute(const std::vector<tables::Row>& rows,
                                       int k, const tables::ScanConfig& cfg) {
  std::vector<long long> out;
  long long base = 0;
  for (const auto& r : rows) {
    out.push_back(base + r.w[k]);
    if (r.kind() == RowKind::kOrigin) base += r.at() + cfg.originLatencyClk;
  }
  return out;
}

static void Rebasing() {
  auto cfg = ReferenceConfig();

  // 30 s with nothing but the END row: origin() rows chain across the idle
  // time (2^30-clock epochs), every word stays below 2^31.
  model::SequenceModel idle;
  idle.duration = 3'000'000'000LL;
  auto t = tables::BuildTables(idle, cfg);
  CHECK_EQ(t.tx.size(), 3u);  // ORIGIN, ORIGIN, END
  CHECK(t.tx[0].kind() == RowKind::kOrigin);
  CHECK(t.tx.back().kind() == RowKind::kEnd);
  CHECK_EQ(Absolute(t.tx, 1, cfg).back(), 10000LL + 3'000'000'000LL);
  CHECK_EQ(t.origins, 5 * 2);  // Tx, Rx, GA, GB, GC
  for (const auto& r : t.tx) CHECK(r.at() <= 0x7FFFFFFFu);

  // Pulses every 1e6 clocks with a forced 3e6-clock epoch: each row still
  // lands on its absolute time, and each ORIGIN keeps the loop slack to the
  // rows around it.
  cfg.epochLimitClk = 3'000'000;
  model::SequenceModel m;
  m.duration = 20'000'000;
  for (int k = 0; k < 18; ++k) m.rf.push_back(Pulse(k, k * 1'000'000LL, 0));
  t = tables::BuildTables(m, cfg);
  CHECK(t.origins > 0);
  const auto at = Absolute(t.tx, 1, cfg);
  const auto gateOn = Absolute(t.tx, 5, cfg);
  const auto gateOff = Absolute(t.tx, 6, cfg);
  int pulse = 0;
  long long prevOff = -1, lastOrigin = -1;
  for (std::size_t i = 0; i < t.tx.size(); ++i) {
    const auto& r = t.tx[i];
    CHECK(r.at() <= 3'000'000u + 10'000u);
    if (r.kind() == RowKind::kOrigin) {
      lastOrigin = at[i];
      CHECK(prevOff < 0 || lastOrigin - prevOff >= cfg.txSlackClk);
    } else if (r.kind() == RowKind::kRf) {
      CHECK_EQ(at[i], 10000LL + pulse * 1'000'000LL + 50);
      if (lastOrigin >= 0)
        CHECK(gateOn[i] - cfg.txPhaseLeadClk -
                  (lastOrigin + cfg.originLatencyClk) >= cfg.txSlackClk);
      prevOff = gateOff[i];
      ++pulse;
    }
  }
  CHECK_EQ(pulse, 18);
  CHECK(t.timingIssues.empty());

  // A single waveform longer than an epoch leaves no room for origin().
  model::SequenceModel g;
  g.duration = 10'000'000;
  model::GradEvent ge;
  ge.block = 0;
  ge.axis = 0;
  ge.start = 0;
  ge.rasterClk = 1000;
  ge.samplesHzPerM.assign(5001, 0.0);  // 5e6 clocks
  g.grad.push_back(ge);
  CHECK_THROWS(tables::BuildTables(g, cfg), tables::TableError);
}

// -export (plot_waveforms.py): samples back in physical units on the sequence
// timeline.
static void Export() {
  const auto cfg = ReferenceConfig();
  const auto t = tables::BuildTables(Load("fid_v150.seq"), cfg);
  const std::string path = std::string(PP_TEST_TMP_DIR) + "/fid_waves.csv";
  CHECK(tables::WriteWaveforms(path, t, cfg));
  std::FILE* f = std::fopen(path.c_str(), "r");
  CHECK(f != nullptr);
  char line[256];
  int rf = 0, gz = 0, adc = 0, gates = 0;
  double rf0t = -1, rf0v = -1, gzMax = 0, adc0 = -1;
  while (std::fgets(line, sizeof line, f)) {
    char ch[16];
    double ts = 0, v = 0;
    if (std::sscanf(line, "%15[^,],%lf,%lf", ch, &ts, &v) != 3) continue;
    const std::string c = ch;
    if (c == "rf" && rf++ == 0) rf0t = ts, rf0v = v;
    if (c == "gz") ++gz, gzMax = std::max(gzMax, v);
    if (c == "adc" && adc++ == 0) adc0 = ts;
    if (c == "gate") ++gates;
  }
  std::fclose(f);
  CHECK_EQ(rf, 101);                  // 100 samples + trailing zero
  CHECK_NEAR(rf0t, 0.5e-6, 1e-12);    // first sample centre
  CHECK_NEAR(rf0v, 50.0, 1e-9);       // 0.5 of -rfmax 100
  CHECK_EQ(gates, 2);
  CHECK_EQ(gz, 121);
  CHECK_NEAR(gzMax, 100000.0, 1e-6);  // trapezoid flat top
  CHECK_EQ(adc, 256);
  CHECK_NEAR(adc0, 235e-6, 1e-12);    // block start + delay + dwell/2
}

static void Run() {
  Fid();
  Export();
  Features();
  Synthetic();
  Rebasing();
}

TEST_MAIN(Run)
