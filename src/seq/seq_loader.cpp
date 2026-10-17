#include "seq/seq_loader.hpp"

#include "ExternalSequence.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>

namespace pp::seq {

namespace {

using model::Clk;

// ExternalSequence reports through a global C-style callback; collect its
// output so errors can be attached to the exception text.
std::vector<std::string>* gCapture = nullptr;
void CapturePrint(const std::string& s) {
  if (gCapture && !s.empty()) gCapture->push_back(s);
}

std::string Join(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& l : lines) out += l + (l.back() == '\n' ? "" : "\n");
  return out;
}

[[noreturn]] void Fail(int block, const std::string& what) {
  std::ostringstream os;
  if (block >= 0) os << "block " << block + 1 << ": ";
  os << what;
  throw LoadError(os.str());
}

// A piecewise-linear gradient given by (time us, value Hz/m) vertices, sampled
// at raster edges t = k * raster (k = 0 .. ceil(tEnd / raster)).
std::vector<double> SampleAtEdges(const std::vector<std::pair<double, double>>& v,
                                  double rasterUs) {
  std::vector<double> out;
  if (v.empty()) return out;
  const double tEnd = v.back().first;
  const int n = static_cast<int>(std::ceil(tEnd / rasterUs - 1e-9));
  out.reserve(n + 1);
  std::size_t seg = 0;
  for (int k = 0; k <= n; ++k) {
    const double t = k * rasterUs;
    // Advance to the segment [v[seg], v[seg+1]] that contains t.
    while (seg + 2 < v.size() && v[seg + 1].first <= t) ++seg;
    double value;
    if (t <= v.front().first) {
      value = v.front().second;
    } else if (t >= tEnd) {
      value = v.back().second;
    } else {
      const auto [t0, g0] = v[seg];
      const auto [t1, g1] = v[seg + 1];
      value = t1 > t0 ? g0 + (g1 - g0) * (t - t0) / (t1 - t0) : g1;
    }
    out.push_back(value);
  }
  return out;
}

double Defined(float v) { return v == FLOAT_UNDEFINED ? 0.0 : v; }

}  // namespace

model::SequenceModel LoadSequence(const std::string& path,
                                  const LoadOptions& opt) {
  std::vector<std::string> log;
  gCapture = &log;
  ExternalSequence::SetPrintFunction(&CapturePrint);
  struct Reset {
    ~Reset() { gCapture = nullptr; }
  } reset;

  ExternalSequence es;
  if (!es.load(path)) Fail(-1, "cannot load '" + path + "':\n" + Join(log));

  model::SequenceModel m;
  m.version = es.GetVersion();
  if (m.version < 1005000 && !opt.allowPre15) {
    std::ostringstream os;
    os << "Pulseq version " << m.version / 1000000 << '.'
       << m.version / 1000 % 1000 << '.' << m.version % 1000
       << " is not supported (v1 targets 1.5.x)";
    Fail(-1, os.str());
  }
  m.name = es.GetDefinitionStr("Name");

  auto def = [&](const char* key, double fallback) {
    const auto v = es.GetDefinition(key);
    return v.empty() ? fallback : v[0] * 1e6;  // seconds -> us
  };
  m.rasters.blockUs = def("BlockDurationRaster", 10);
  m.rasters.gradUs = def("GradientRasterTime", 10);
  m.rasters.rfUs = def("RadiofrequencyRasterTime", 1);
  m.rasters.adcUs = def("AdcRasterTime", 0.1);

  const auto& c = opt.clocks;
  const Clk gradRaster =
      units::MicrosecondsToClocks(m.rasters.gradUs, c, "GradientRasterTime");
  const double f0MHz = opt.larmorHz * 1e-6;

  Clk t = 0;
  for (int iB = 0; iB < es.GetNumberOfBlocks(); ++iB) {
    std::unique_ptr<SeqBlock> b(es.GetBlock(iB));
    if (!b || !es.decodeBlock(b.get())) Fail(iB, "decode failed:\n" + Join(log));

    if (b->isRotation()) Fail(iB, "rotation extension not supported yet");
    if (b->isTrigger()) Fail(iB, "trigger extension not supported yet");
    if (b->isSoftDelay()) Fail(iB, "soft delay extension not supported yet");

    model::Block blk;
    blk.index = iB;
    blk.start = t;
    blk.duration =
        units::MicrosecondsToClocks(b->GetDuration(), c, "block duration");

    if (b->isRF()) {
      const RFEvent& ev = b->GetRFEvent();
      model::RfEvent rf;
      rf.block = iB;
      rf.start = t + units::MicrosecondsToClocks(ev.delay, c, "RF delay");
      rf.rasterClk = units::MicrosecondsToClocks(b->GetRFDwellTime(), c,
                                                 "RF dwell time");
      rf.amplitudeHz = ev.amplitude;
      const int n = b->GetRFLength();
      rf.magnitude.assign(b->GetRFAmplitudePtr(), b->GetRFAmplitudePtr() + n);
      rf.phaseRad.assign(b->GetRFPhasePtr(), b->GetRFPhasePtr() + n);
      rf.freqOffsetHz = ev.freqOffset + ev.freqPPM * 1e-6 * opt.larmorHz;
      rf.phaseOffsetRad = ev.phaseOffset + ev.phasePPM * f0MHz;
      rf.centerUs = ev.center;
      rf.use = ev.use;
      if (rf.start + rf.Duration() > t + blk.duration)
        Fail(iB, "RF pulse extends past the end of the block");
      blk.rf = static_cast<int>(m.rf.size());
      m.rf.push_back(std::move(rf));
    }

    for (int axis = 0; axis < 3; ++axis) {
      if (!b->isTrapGradient(axis) && !b->isArbitraryGradient(axis) &&
          !b->isExtTrapGradient(axis))
        continue;
      const GradEvent& ev = b->GetGradEvent(axis);
      model::GradEvent g;
      g.block = iB;
      g.axis = axis;
      g.rasterClk = gradRaster;
      g.start = t + units::MicrosecondsToClocks(ev.delay, c, "gradient delay");

      std::vector<std::pair<double, double>> vertices;  // us from g.start
      if (b->isTrapGradient(axis)) {
        g.source = model::GradEvent::Source::kTrapezoid;
        const double r = ev.rampUpTime, f = ev.flatTime, d = ev.rampDownTime;
        vertices = {{0, 0}, {r, ev.amplitude}, {r + f, ev.amplitude},
                    {r + f + d, 0}};
      } else if (b->isExtTrapGradient(axis)) {
        g.source = model::GradEvent::Source::kExtendedTrapezoid;
        const auto& times = b->GetExtTrapGradTimes(axis);
        const auto& shape = b->GetExtTrapGradShape(axis);
        for (std::size_t i = 0; i < times.size(); ++i)
          vertices.emplace_back(static_cast<double>(times[i]),
                                shape[i] * ev.amplitude);
      } else {
        if (b->isArbGradWithOversampling(axis))
          Fail(iB, "oversampled arbitrary gradients are not supported yet");
        g.source = model::GradEvent::Source::kArbitrary;
        // Samples are defined at raster centres; first/last give the edges.
        const int n = b->GetArbGradNumSamples(axis);
        const float* s = b->GetArbGradShapePtr(axis);
        vertices.emplace_back(0.0, Defined(ev.first));
        for (int k = 0; k < n; ++k)
          vertices.emplace_back((k + 0.5) * m.rasters.gradUs,
                                s[k] * ev.amplitude);
        vertices.emplace_back(n * m.rasters.gradUs, Defined(ev.last));
      }
      g.samplesHzPerM = SampleAtEdges(vertices, m.rasters.gradUs);
      if (g.End() > t + blk.duration)
        Fail(iB, "gradient extends past the end of the block");
      blk.grad[axis] = static_cast<int>(m.grad.size());
      m.grad.push_back(std::move(g));
    }

    if (b->isADC()) {
      const ADCEvent& ev = b->GetADCEvent();
      if (ev.phaseModulationShapeID != 0)
        Fail(iB, "ADC phase modulation shapes are not supported yet");
      model::AdcEvent a;
      a.block = iB;
      a.start = t + units::MicrosecondsToClocks(ev.delay, c, "ADC delay");
      a.numSamples = ev.numSamples;
      a.dwellClk = units::NanosecondsToClocks(ev.dwellTime, c, "ADC dwell");
      if (a.dwellClk % 2 != 0)
        Fail(iB, "ADC dwell must be an even number of clocks (sample centres)");
      a.freqOffsetHz = ev.freqOffset + ev.freqPPM * 1e-6 * opt.larmorHz;
      a.phaseOffsetRad = ev.phaseOffset + ev.phasePPM * f0MHz;
      if (a.End() > t + blk.duration)
        Fail(iB, "ADC extends past the end of the block");
      blk.adc = static_cast<int>(m.adc.size());
      m.adc.push_back(a);
    }

    t += blk.duration;
    m.blocks.push_back(blk);
  }
  m.duration = t;
  m.readerMessages = std::move(log);
  return m;
}

}  // namespace pp::seq
