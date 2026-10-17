// Waveform libraries: every distinct waveform stored once, concatenated into
// one array per kind that Phase 2 turns into a single PLATO stream per channel
// type. Table rows then refer to waveforms by BYTE offset into that stream
// (test/FINDINGS.md S1-S3: stream addresses are bytes; one
// fmt::Modulation point = 8 B, one fmt::Gradient point = 4 B).
//
// De-duplication compares the values after quantisation to what the hardware
// sees (int16 amplitude/phase, int32 frequency, int16 gradient), so two
// waveforms that differ only below the DAC resolution share storage.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace pp::shapes {

// One fmt::Modulation point, fields in [-1, 1].
struct ModPoint {
  double amplitude = 0;
  double phase = 0;      // +-1 = +-180 deg
  double frequency = 0;  // +-1 = +-Frf/2
};

struct WaveformRef {
  std::uint32_t byteOffset = 0;
  std::uint32_t points = 0;
};

class RfLibrary {
public:
  static constexpr std::uint32_t kBytesPerPoint = 8;
  WaveformRef Add(const std::vector<ModPoint>& points);
  const std::vector<ModPoint>& points() const { return points_; }
  std::size_t unique() const { return index_.size(); }

private:
  std::vector<ModPoint> points_;
  std::map<std::vector<std::int32_t>, WaveformRef> index_;
};

class GradLibrary {
public:
  static constexpr std::uint32_t kBytesPerPoint = 4;
  WaveformRef Add(const std::vector<double>& values);
  const std::vector<double>& values() const { return values_; }
  std::size_t unique() const { return index_.size(); }

private:
  std::vector<double> values_;
  std::map<std::vector<std::int32_t>, WaveformRef> index_;
};

// Hardware quantisation used for de-duplication (matches stream_definition.hpp:
// round(clamp(v * INTMAX))).
std::int32_t QuantizeInt16(double v);
std::int32_t QuantizeInt32(double v);

}  // namespace pp::shapes
