// Host-side intermediate representation of a Pulseq sequence.
//
// Produced by seq::LoadSequence from ExternalSequence, consumed by the channel
// table builder. Physical units, but all times are already integer RT clocks
// relative to sequence start (t = 0 at the start of block 0): the loader is the
// only place that deals with microseconds and rasters.
//
// Sampling conventions (see test/FINDINGS.md V3/V4):
//   - RF shape sample k is centred in its raster interval:
//       t_k = rf.start + (k + 0.5) * rf.rasterClk, where rf.start = block + delay
//   - Gradients are stored as values at raster EDGES:
//       t_k = grad.start + k * rasterClk, k = 0..N-1 (piecewise-linear source)
//   - ADC sample k is centred:  t_k = adc.start + (k + 0.5) * dwellClk
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pp::model {

using Clk = std::int64_t;  // RT clocks (10 ns); int64 so long runs don't wrap

struct RfEvent {
  int block = -1;          // 0-based block index
  Clk start = 0;           // block start + delay
  Clk rasterClk = 0;       // RF sample period
  double amplitudeHz = 0;  // Pulseq amplitude (Hz)
  std::vector<float> magnitude;  // normalised shape, one per raster interval
  std::vector<float> phaseRad;   // shape phase (rad), same length
  double freqOffsetHz = 0;       // incl. freqPPM * 1e-6 * f0
  double phaseOffsetRad = 0;     // incl. phasePPM * f0[MHz]
  double centerUs = -1;          // Pulseq 1.5 'center' (from shape start), -1 if absent
  char use = 'u';
  Clk Duration() const { return rasterClk * static_cast<Clk>(magnitude.size()); }
};

struct GradEvent {
  int block = -1;
  int axis = 0;            // 0 = x, 1 = y, 2 = z (Pulseq logical axes)
  Clk start = 0;           // time of samples[0] (block start + delay)
  Clk rasterClk = 0;       // gradient raster
  std::vector<double> samplesHzPerM;  // values at raster edges
  enum class Source { kTrapezoid, kExtendedTrapezoid, kArbitrary } source =
      Source::kTrapezoid;
  Clk End() const {
    return samplesHzPerM.empty()
               ? start
               : start + rasterClk * static_cast<Clk>(samplesHzPerM.size() - 1);
  }
};

struct AdcEvent {
  int block = -1;
  Clk start = 0;           // block start + delay (samples are centred after it)
  int numSamples = 0;
  Clk dwellClk = 0;
  double freqOffsetHz = 0;   // incl. ppm
  double phaseOffsetRad = 0; // incl. ppm
  Clk FirstSampleTime() const { return start + dwellClk / 2; }
  Clk End() const { return start + dwellClk * numSamples; }
};

struct Block {
  int index = 0;  // 0-based
  Clk start = 0;
  Clk duration = 0;
  int rf = -1;                       // index into SequenceModel::rf, -1 = none
  std::array<int, 3> grad{-1, -1, -1};
  int adc = -1;
};

struct Rasters {
  double blockUs = 10, gradUs = 10, rfUs = 1, adcUs = 0.1;
};

struct SequenceModel {
  int version = 0;   // major*1'000'000 + minor*1'000 + revision
  std::string name;
  Rasters rasters;
  Clk duration = 0;  // sum of block durations
  std::vector<Block> blocks;
  std::vector<RfEvent> rf;
  std::vector<GradEvent> grad;
  std::vector<AdcEvent> adc;
  // Informational output of the upstream reader (e.g. "signature block not
  // found"), one entry per message.
  std::vector<std::string> readerMessages;
};

}  // namespace pp::model
