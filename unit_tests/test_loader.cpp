#include "check.hpp"
#include "seq/seq_loader.hpp"

#include <string>

using namespace pp;

static const std::string kDir = PP_TEST_SEQ_DIR;

static void Fid() {
  seq::LoadOptions opt;
  opt.larmorHz = 27995000;
  const auto m = seq::LoadSequence(kDir + "/fid_v150.seq", opt);

  CHECK_EQ(m.version, 1005000);
  CHECK(m.name == "fid_v150");
  CHECK_EQ(m.blocks.size(), 3u);
  CHECK_EQ(m.blocks[0].duration, 11000);  // 110 us
  CHECK_EQ(m.blocks[1].duration, 10000);
  CHECK_EQ(m.blocks[2].duration, 280000);  // 280 x 10 us = 2.8 ms
  CHECK_EQ(m.blocks[2].start, 21000);
  CHECK_EQ(m.duration, 301000);

  CHECK_EQ(m.rf.size(), 1u);
  const auto& rf = m.rf[0];
  CHECK_EQ(rf.start, 0);
  CHECK_EQ(rf.rasterClk, 100);
  CHECK_EQ(rf.magnitude.size(), 100u);
  CHECK_NEAR(rf.amplitudeHz, 50, 1e-9);
  CHECK_NEAR(rf.magnitude[0], 1, 1e-6);
  CHECK_NEAR(rf.magnitude[99], 1, 1e-6);
  CHECK_NEAR(rf.phaseRad[50], 0, 1e-9);
  CHECK_NEAR(rf.centerUs, 50, 1e-6);
  CHECK(rf.use == 'e');

  // z trapezoid 100 kHz/m, 100/1000/100 us -> 121 edge samples on 10 us.
  CHECK_EQ(m.grad.size(), 1u);
  const auto& g = m.grad[0];
  CHECK_EQ(g.axis, 2);
  CHECK_EQ(g.block, 2);
  CHECK_EQ(g.start, 21000);
  CHECK_EQ(g.rasterClk, 1000);
  CHECK_EQ(g.samplesHzPerM.size(), 121u);
  CHECK_NEAR(g.samplesHzPerM[0], 0, 1e-6);
  CHECK_NEAR(g.samplesHzPerM[1], 10000, 1e-3);   // first edge on the ramp
  CHECK_NEAR(g.samplesHzPerM[10], 100000, 1e-3);
  CHECK_NEAR(g.samplesHzPerM[110], 100000, 1e-3);
  CHECK_NEAR(g.samplesHzPerM[115], 50000, 1e-3);
  CHECK_NEAR(g.samplesHzPerM[120], 0, 1e-6);
  CHECK_EQ(g.End(), 21000 + 120000);

  CHECK_EQ(m.adc.size(), 1u);
  const auto& a = m.adc[0];
  CHECK_EQ(a.numSamples, 256);
  CHECK_EQ(a.dwellClk, 1000);
  CHECK_EQ(a.start, 23000);            // block 3 + 20 us
  CHECK_EQ(a.FirstSampleTime(), 23500);
  CHECK_EQ(a.End(), 23000 + 256000);
}

static void Features() {
  seq::LoadOptions opt;
  opt.larmorHz = 27995000;
  const auto m = seq::LoadSequence(kDir + "/features_v150.seq", opt);
  CHECK_EQ(m.blocks.size(), 4u);
  CHECK_EQ(m.duration, 30000 + 2000 + 2000 + 3000);

  const auto& rf = m.rf.at(0);
  CHECK_EQ(rf.start, 1000);
  CHECK_EQ(rf.magnitude.size(), 4u);
  CHECK_NEAR(rf.freqOffsetHz, 1000, 1e-9);
  CHECK_NEAR(rf.phaseOffsetRad, 0.5, 1e-7);

  // Arbitrary x gradient: centre samples .25 .75 .75 .25 * 20 kHz/m with
  // first = last = 0 -> edges 0, 10k, 15k, 10k, 0.
  CHECK_EQ(m.grad.size(), 3u);
  const auto& gx = m.grad[0];
  CHECK_EQ(gx.axis, 0);
  CHECK(gx.source == model::GradEvent::Source::kArbitrary);
  CHECK_EQ(gx.samplesHzPerM.size(), 5u);
  CHECK_NEAR(gx.samplesHzPerM[1], 10000, 1e-3);
  CHECK_NEAR(gx.samplesHzPerM[2], 15000, 1e-3);
  CHECK_NEAR(gx.samplesHzPerM[4], 0, 1e-6);

  // Extended trapezoids on y: [0, 30k, 30k] then [30k, 30k, 0].
  const auto& gy1 = m.grad[1];
  const auto& gy2 = m.grad[2];
  CHECK(gy1.source == model::GradEvent::Source::kExtendedTrapezoid);
  CHECK_EQ(gy1.start, 30000);
  CHECK_EQ(gy1.samplesHzPerM.size(), 3u);
  CHECK_NEAR(gy1.samplesHzPerM[1], 30000, 1e-3);
  CHECK_NEAR(gy1.samplesHzPerM[2], 30000, 1e-3);
  CHECK_EQ(gy1.End(), gy2.start);  // meet on the block boundary
  CHECK_NEAR(gy2.samplesHzPerM[0], 30000, 1e-3);
  CHECK_NEAR(gy2.samplesHzPerM[2], 0, 1e-6);

  const auto& a = m.adc.at(0);
  CHECK_EQ(a.start, 34000 + 1000);
  CHECK_EQ(a.dwellClk, 200);
  CHECK_NEAR(a.freqOffsetHz, 500, 1e-9);
  CHECK_NEAR(a.phaseOffsetRad, 0.25, 1e-7);
}

// ppm offsets scale with the physical frequency: PTS base (270 MHz for
// pts_mask 77) + NCO frequency (-f), not with -f alone.
static void Ppm() {
  seq::LoadOptions opt;
  opt.larmorHz = 270e6 + 27998450;  // 297.99845 MHz
  const auto m = seq::LoadSequence(kDir + "/ppm_v150.seq", opt);
  CHECK_NEAR(m.rf.at(0).freqOffsetHz, 3.5e-6 * 297998450, 1e-6);   // ~1043 Hz
  CHECK_NEAR(m.adc.at(0).freqOffsetHz, -3.5e-6 * 297998450, 1e-6);
}

static void Errors() {
  seq::LoadOptions opt;
  CHECK_THROWS(seq::LoadSequence(kDir + "/does_not_exist.seq", opt),
               seq::LoadError);
}

static void Run() {
  Fid();
  Features();
  Ppm();
  Errors();
}

TEST_MAIN(Run)
