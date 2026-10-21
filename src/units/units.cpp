#include "units/units.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>

namespace pp::units {

std::int64_t SecondsToClocks(double seconds, const Clocks& c,
                             const char* what) {
  const double clocks = seconds * c.rtClockHz;
  const double rounded = std::round(clocks);
  if (std::abs(clocks - rounded) > 1e-6 * std::max(1.0, std::abs(clocks))) {
    std::ostringstream os;
    os << what << ": " << seconds * 1e6 << " us is not a whole number of "
       << 1e9 / c.rtClockHz << " ns clocks";
    throw UnitError(os.str());
  }
  return static_cast<std::int64_t>(rounded);
}

std::uint32_t FrequencyWord(double hz, const Clocks& c) {
  // Reduce into [0, Frf) first so the product stays exact enough in double.
  double f = std::fmod(hz, c.rfClockHz);
  if (f < 0) f += c.rfClockHz;
  const double word = std::round(f / c.rfClockHz * 4294967296.0);
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(word) &
                                    0xFFFFFFFFull);
}

double ModulationFrequency(double hz, const Clocks& c) {
  const double v = hz / (c.rfClockHz / 2);
  if (std::abs(v) > 1.0)
    throw UnitError("frequency offset outside +-Frf/2");
  return v;
}

double WrapPi(double rad) {
  constexpr double kTwoPi = 2 * std::numbers::pi;
  double r = std::fmod(rad + std::numbers::pi, kTwoPi);
  if (r < 0) r += kTwoPi;
  return r - std::numbers::pi;
}

double ModulationPhase(double rad) { return WrapPi(rad) / std::numbers::pi; }

std::uint32_t PhaseWord16(double rad) {
  const double turns = WrapPi(rad) / (2 * std::numbers::pi);  // [-0.5, 0.5)
  const auto w = static_cast<std::int64_t>(std::llround(turns * 65536.0));
  return static_cast<std::uint32_t>(w & 0xFFFF);
}

std::uint32_t PhaseWord32(double rad) {
  const double turns = WrapPi(rad) / (2 * std::numbers::pi);
  const auto w = static_cast<std::int64_t>(std::llround(turns * 4294967296.0));
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(w) &
                                    0xFFFFFFFFull);
}

std::optional<PtsSetting> LookupPts(unsigned long ptsMask) {
  switch (ptsMask) {
    case 0: return PtsSetting{0.0, +1};
    case 77: return PtsSetting{270e6, +1};
    case 109: return PtsSetting{250e6, +1};
    case 47: return PtsSetting{150e6, -1};
    case 190: return PtsSetting{104e6, -1};
    case 63: return PtsSetting{114e6, -1};
    case 107: return PtsSetting{58e6, -1};
    default: return std::nullopt;
  }
}

double Fraction(double value, double fullScale, const char* what) {
  if (!(fullScale > 0)) throw UnitError(std::string(what) + ": full scale <= 0");
  const double f = value / fullScale;
  if (std::abs(f) > 1.0 + 1e-6) {
    std::ostringstream os;
    os << what << ": " << value << " exceeds full scale " << fullScale;
    throw UnitError(os.str());
  }
  return std::clamp(f, -1.0, 1.0);
}

}  // namespace pp::units
