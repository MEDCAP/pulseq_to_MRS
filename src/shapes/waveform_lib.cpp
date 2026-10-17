#include "shapes/waveform_lib.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace pp::shapes {

std::int32_t QuantizeInt16(double v) {
  return static_cast<std::int32_t>(std::round(std::clamp(v, -1.0, 1.0) * 32767.0));
}

std::int32_t QuantizeInt32(double v) {
  return static_cast<std::int32_t>(
      std::llround(std::clamp(v, -1.0, 1.0) * 2147483647.0));
}

namespace {
void CheckSize(std::size_t current, std::size_t add, std::uint32_t bytesPerPoint) {
  if ((current + add) * bytesPerPoint > std::numeric_limits<std::int32_t>::max())
    throw std::length_error("waveform library exceeds 2 GiB");
}
}  // namespace

WaveformRef RfLibrary::Add(const std::vector<ModPoint>& pts) {
  std::vector<std::int32_t> key;
  key.reserve(pts.size() * 3);
  for (const auto& p : pts) {
    key.push_back(QuantizeInt16(p.amplitude));
    key.push_back(QuantizeInt16(p.phase));
    key.push_back(QuantizeInt32(p.frequency));
  }
  if (auto it = index_.find(key); it != index_.end()) return it->second;
  CheckSize(points_.size(), pts.size(), kBytesPerPoint);
  WaveformRef ref{static_cast<std::uint32_t>(points_.size() * kBytesPerPoint),
                  static_cast<std::uint32_t>(pts.size())};
  points_.insert(points_.end(), pts.begin(), pts.end());
  index_.emplace(std::move(key), ref);
  return ref;
}

WaveformRef GradLibrary::Add(const std::vector<double>& vals) {
  std::vector<std::int32_t> key;
  key.reserve(vals.size());
  for (double v : vals) key.push_back(QuantizeInt16(v));
  if (auto it = index_.find(key); it != index_.end()) return it->second;
  CheckSize(values_.size(), vals.size(), kBytesPerPoint);
  WaveformRef ref{static_cast<std::uint32_t>(values_.size() * kBytesPerPoint),
                  static_cast<std::uint32_t>(vals.size())};
  values_.insert(values_.end(), vals.begin(), vals.end());
  index_.emplace(std::move(key), ref);
  return ref;
}

}  // namespace pp::shapes
