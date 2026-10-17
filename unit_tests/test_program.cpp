// Builds the realtime programs (no spectrometer needed): both emitters accept
// the test sequences, every channel gets a component, and the unrolled emitter
// refuses tables too large for it.
#include "check.hpp"
#include "realtime/program.hpp"
#include "seq/seq_loader.hpp"
#include "tables/channel_table.hpp"

#include <stdexcept>
#include <string>

using namespace pp;

static const std::string kDir = PP_TEST_SEQ_DIR;

static tables::ChannelTables Tables(const char* file) {
  seq::LoadOptions lo;
  lo.larmorHz = 27995000;
  tables::ScanConfig c;
  c.larmorHz = 27995000;
  c.rfMaxHz = 100;
  c.gradMaxHzPerM = {4.2e6, 4.2e6, 4.2e6};
  return tables::BuildTables(seq::LoadSequence(kDir + "/" + file, lo), c);
}

static void BothEmitters() {
  for (const char* file : {"fid_v150.seq", "features_v150.seq"}) {
    const auto t = Tables(file);
    for (auto e : {realtime::Emitter::kBlockLoop, realtime::Emitter::kUnrolled}) {
      realtime::ProgramOptions o;
      o.emitter = e;
      const auto p = realtime::BuildProgram(t, o);
      CHECK_EQ(p.components.size(), 5u);
      CHECK_EQ(p.sequence.get()->components().size(), 5u);
      CHECK(p.components[0].name == "Tx");
      CHECK(p.components[1].name == "Rx");
      CHECK_EQ(p.components[0].rows, t.tx.size());
      CHECK_EQ(p.components[4].rows, t.grad[2].size());
    }
  }
}

static void UnrolledLimit() {
  // 100 pulses: fine for the block loop, too many rows to unroll.
  model::SequenceModel m;
  for (int k = 0; k < 100; ++k) {
    model::RfEvent rf;
    rf.block = k;
    rf.start = k * 10000;
    rf.rasterClk = 100;
    rf.amplitudeHz = 10;
    rf.magnitude.assign(4, 1.0f);
    rf.phaseRad.assign(4, 0.0f);
    m.rf.push_back(rf);
  }
  m.duration = 1'000'000;
  tables::ScanConfig c;
  c.larmorHz = 27995000;
  c.rfMaxHz = 100;
  const auto t = tables::BuildTables(m, c);
  realtime::ProgramOptions o;
  (void)realtime::BuildProgram(t, o);
  o.emitter = realtime::Emitter::kUnrolled;
  CHECK_THROWS(realtime::BuildProgram(t, o), std::runtime_error);
}

static void Run() {
  BothEmitters();
  UnrolledLimit();
}

TEST_MAIN(Run)
