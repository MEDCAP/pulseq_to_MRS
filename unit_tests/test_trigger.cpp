// Respiratory triggering: tables, settings and realtime programs. No hardware
// or simulator. The test writes its own small .seq (the test/ data folder is
// not part of the repository): 3 repetitions of
//   block A  excitation RF (use 'e') + z trapezoid filling the block
//   block B  64-sample ADC + z trapezoid filling the block
// so the z gradient of B ends exactly where the next A starts: without a gate
// the two waveforms are joined, with a gate they must not be.
#include "check.hpp"
#include "config/scan_settings.hpp"
#include "realtime/program.hpp"
#include "seq/seq_loader.hpp"
#include "tables/channel_table.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace pp;
using tables::Row;
using tables::RowKind;

namespace {

const std::string kTmp = PP_TEST_TMP_DIR;

const char* kSeqText = R"(# Pulseq sequence file
# test_trigger: 3 repetitions

[VERSION]
major 1
minor 5
revision 0

[DEFINITIONS]
AdcRasterTime 1e-07
BlockDurationRaster 1e-05
GradientRasterTime 1e-05
Name trig
RadiofrequencyRasterTime 1e-06
TotalDuration 0.36

[BLOCKS]
1 2000 1 0 0 1 0 0
2 10000 0 0 0 2 1 0
3 2000 1 0 0 1 0 0
4 10000 0 0 0 2 1 0
5 2000 1 0 0 1 0 0
6 10000 0 0 0 2 1 0

[RF]
1 2500 1 2 3 50 0 0 0 0 0 e

[TRAP]
1 1000000 100 19800 100 0
2 1000000 100 99800 100 0

[ADC]
1 64 50000 20 0 0 0 0 0

[SHAPES]

shape_id 1
num_samples 2
1
1

shape_id 2
num_samples 2
0
0

shape_id 3
num_samples 2
0
100

)";

std::string Write(const std::string& name, std::string text) {
  const std::string path = kTmp + "/" + name;
  std::ofstream(path, std::ios::binary) << text;
  return path;
}

model::SequenceModel Load(const std::string& path) {
  seq::LoadOptions opt;
  opt.larmorHz = 27995000;
  return seq::LoadSequence(path, opt);
}

tables::ScanConfig Config() {
  tables::ScanConfig c;
  c.larmorHz = 27995000;
  c.rfMaxHz = 5000;
  c.gradMaxHzPerM = {1e7, 1e7, 1e7};
  return c;
}

std::vector<RowKind> Kinds(const std::vector<Row>& rows) {
  std::vector<RowKind> k;
  for (const auto& r : rows) k.push_back(r.kind());
  return k;
}

int Count(const std::vector<Row>& rows, RowKind k) {
  return static_cast<int>(std::count_if(
      rows.begin(), rows.end(), [&](const Row& r) { return r.kind() == k; }));
}

int TriggerRows(const tables::ChannelTables& t) {
  return Count(t.tx, RowKind::kTrigger) + Count(t.rx, RowKind::kTrigger) +
         Count(t.grad[0], RowKind::kTrigger) +
         Count(t.grad[1], RowKind::kTrigger) +
         Count(t.grad[2], RowKind::kTrigger) +
         Count(t.uio, RowKind::kTrigger);
}

void Tables() {
  const auto m = Load(Write("trigger_ok.seq", kSeqText));
  auto cfg = Config();

  // Off: nothing changes.
  const auto off = tables::BuildTables(m, cfg);
  CHECK_EQ(off.triggers, 0);
  CHECK_EQ(TriggerRows(off), 0);
  CHECK_EQ(off.uio.size(), 1u);

  // On: one gate per excitation block, a row on every channel incl. UIO.
  cfg.respiratoryTrigger = true;
  const auto on = tables::BuildTables(m, cfg);
  CHECK_EQ(on.triggers, 3);
  CHECK_EQ(TriggerRows(on), 6 * 3);
  const RowKind T = RowKind::kTrigger, R = RowKind::kRf, A = RowKind::kAdc,
                G = RowKind::kGrad, E = RowKind::kEnd;
  CHECK(Kinds(on.tx) == (std::vector<RowKind>{T, R, T, R, T, R, E}));
  CHECK(Kinds(on.rx) == (std::vector<RowKind>{T, A, T, A, T, A, E}));
  CHECK(Kinds(on.grad[0]) == (std::vector<RowKind>{T, T, T, E}));
  CHECK(Kinds(on.grad[2]) ==
        (std::vector<RowKind>{T, G, G, T, G, G, T, G, G, E}));
  CHECK(Kinds(on.uio) == (std::vector<RowKind>{T, T, T, E}));

  // Halt times: the start of each excitation block (120 ms apart); the epoch
  // restarts with the halt time at triggerLeadClk.
  const long long halt0 = cfg.startOffset, period = 12000000;
  CHECK_EQ(on.tx[0].at(), static_cast<std::uint32_t>(halt0));
  CHECK_EQ(on.tx[2].at(), static_cast<std::uint32_t>(period + cfg.triggerLeadClk));
  CHECK_EQ(on.tx[4].at(), static_cast<std::uint32_t>(period + cfg.triggerLeadClk));
  for (int i = 0; i < 3; ++i) {
    const long long halt = halt0 + i * period;
    // The first event of a repetition keeps its offset from the halt time.
    CHECK_EQ(on.tx[2 * i + 1].at(),
             static_cast<std::uint32_t>(off.tx[i].at() - halt + cfg.triggerLeadClk));
    // Rx: grid set up again 7 ticks before the ADC (relative to the new epoch).
    const auto& tr = on.rx[2 * i];
    CHECK_EQ(tr.w[3], on.rx[2 * i + 1].w[7]);
    CHECK_EQ(tr.w[4], static_cast<std::uint32_t>(
                          off.rx[i].at() - 7 * static_cast<long long>(tr.w[3]) -
                          (halt - cfg.triggerLeadClk)));
  }
  // Origin delays: settle everywhere, UIO also waits for the message to
  // arrive, and arms after the halt time.
  CHECK_EQ(on.tx[0].w[2], static_cast<std::uint32_t>(cfg.triggerSettleClk));
  CHECK_EQ(on.uio[0].w[2], static_cast<std::uint32_t>(cfg.triggerSettleClk +
                                                      cfg.triggerMsgLatencyClk));
  CHECK_EQ(on.uio[0].w[5], static_cast<std::uint32_t>(cfg.triggerArmClk));
  CHECK_EQ(on.tx[0].w[5], 0u);

  // The gradient of block B is not joined to the next repetition's across a
  // gate (else it would stop on its last non-zero sample while waiting).
  // Without gates the first two B waveforms lose their last point.
  CHECK_EQ(on.grad[2][2].w[3], off.grad[2][1].w[3] + 1);
  CHECK_EQ(on.grad[2][5].w[3], off.grad[2][3].w[3] + 1);
  CHECK_EQ(on.grad[2][8].w[3], off.grad[2][5].w[3]);  // last: never joined

  // No excitation pulse (RF use 'u'): a warning, nothing gated.
  std::string noExc = kSeqText;
  const auto pos = noExc.find("0 0 0 0 0 e");
  CHECK(pos != std::string::npos);
  noExc.replace(pos, 11, "0 0 0 0 0 u");
  const auto none = tables::BuildTables(Load(Write("trigger_none.seq", noExc)), cfg);
  CHECK_EQ(none.triggers, 0);
  CHECK_EQ(TriggerRows(none), 0);
  CHECK(std::any_of(none.warnings.begin(), none.warnings.end(),
                    [](const std::string& w) {
                      return w.find("no excitation") != std::string::npos;
                    }));
}

void Settings() {
  const auto ok = Write("trigger_on.yml",
                        "triggering:\n  respiratory: true\n"
                        "advanced:\n  message_latency_clocks: 1234\n");
  const auto s = config::LoadScanSettings(ok);
  CHECK(s.respiratory.has_value() && *s.respiratory);
  CHECK(s.messageLatencyClocks.has_value() && *s.messageLatencyClocks == 1234);
  CHECK(!config::LoadScanSettings(Write("trigger_unset.yml", "output: x\n"))
             .respiratory.has_value());

  // Typos must not pass silently in a scan configuration.
  bool threw = false;
  try {
    config::LoadScanSettings(
        Write("trigger_typo.yml", "triggering:\n  respiratry: true\n"));
  } catch (const config::SettingsError&) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    config::LoadScanSettings(
        Write("trigger_nobool.yml", "triggering:\n  respiratory: maybe\n"));
  } catch (const config::SettingsError&) {
    threw = true;
  }
  CHECK(threw);
}

bool HasComponent(const realtime::Program& p, const std::string& name) {
  return std::any_of(p.components.begin(), p.components.end(),
                     [&](const realtime::ComponentInfo& c) { return c.name == name; });
}

void Programs() {
  const auto m = Load(Write("trigger_prog.seq", kSeqText));
  auto cfg = Config();
  const auto off = tables::BuildTables(m, cfg);
  cfg.respiratoryTrigger = true;
  const auto on = tables::BuildTables(m, cfg);

  realtime::ProgramOptions po;
  // No PTS mask and no gates: no UIO component (as before).
  CHECK(!HasComponent(realtime::BuildProgram(off, po), "UIO"));
  // Gates need the UIO even without a PTS mask, in both emitters.
  for (auto emitter : {realtime::Emitter::kBlockLoop, realtime::Emitter::kUnrolled}) {
    po.emitter = emitter;
    const auto p = realtime::BuildProgram(on, po);
    CHECK(HasComponent(p, "UIO"));
    CHECK(HasComponent(p, "Tx") && HasComponent(p, "Rx") &&
          HasComponent(p, "GA") && HasComponent(p, "GB") && HasComponent(p, "GC"));
    for (const auto& c : p.components)
      if (c.name == "UIO") CHECK_EQ(c.rows, 4u);  // 3 gates + END
  }
  // PTS mask without gates: UIO as before.
  po.emitter = realtime::Emitter::kBlockLoop;
  po.uioOutWord = 77u << 8;
  CHECK(HasComponent(realtime::BuildProgram(off, po), "UIO"));
}

}  // namespace

int main() {
  try {
    Tables();
    Settings();
    Programs();
  } catch (const std::exception& e) {
    std::printf("uncaught exception: %s\n", e.what());
    return 1;
  }
  if (pp::test::Failures()) {
    std::printf("%d check(s) failed\n", pp::test::Failures());
    return 1;
  }
  std::printf("test_trigger: all checks passed\n");
  return 0;
}
