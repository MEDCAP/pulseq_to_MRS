// Unit conversions between Pulseq physical units and PLATO hardware words.
//
// Plain C++ (no PLATO headers). Conventions measured / documented in
// test/FINDINGS.md and plato/docs (pulse_sequence_programming):
//   - RT clock 100 MHz: 1 clock = 10 ns
//   - NCO (RF) clock 400 MHz: frequency word = 2^32 * f / Frf
//   - setph phase word: 16 bit, 65536 = 360 deg
//   - fmt::Modulation fields in [-1, 1]: amplitude = fraction of full scale,
//     phase +-1 = +-180 deg, frequency +-1 = +-Frf/2
//   - fmt::Gradient value in [-1, 1] = fraction of full-scale DAC
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace pp::units {

struct Clocks {
  double rtClockHz = 100e6;  // realtime engine clock
  double rfClockHz = 400e6;  // NCO clock
};

class UnitError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Exact conversion of a time to RT clocks. Throws UnitError if `seconds` is not
// an integer number of clocks (within 1e-6 clock), naming `what` in the message.
std::int64_t SecondsToClocks(double seconds, const Clocks& c, const char* what);
inline std::int64_t MicrosecondsToClocks(double us, const Clocks& c,
                                         const char* what) {
  return SecondsToClocks(us * 1e-6, c, what);
}
inline std::int64_t NanosecondsToClocks(double ns, const Clocks& c,
                                        const char* what) {
  return SecondsToClocks(ns * 1e-9, c, what);
}

// Absolute NCO frequency word (setfreq), wrapped modulo 2^32. f may be negative.
std::uint32_t FrequencyWord(double hz, const Clocks& c);

// fmt::Modulation frequency field for an offset in Hz: f / (Frf / 2).
double ModulationFrequency(double hz, const Clocks& c);

// Wraps an angle to [-pi, pi).
double WrapPi(double rad);

// fmt::Modulation phase field: WrapPi(rad) / pi, in [-1, 1).
double ModulationPhase(double rad);

// 16-bit setph word, 65536 = 2 pi, wrapped.
std::uint32_t PhaseWord16(double rad);

// 32-bit resetosc phase, 2^32 = 2 pi, wrapped.
std::uint32_t PhaseWord32(double rad);

// PTS synthesizer setting selected by a pts_mask, per the table in the
// scanner's C:\smis\smis.ini [PplVars]: 77 = 270 MHz (1H, 270 + 30 = 300),
// 109 = 250 MHz (19F, 250 + 30), 47 = 150 MHz (31P, 150 - 29), 190 = 104 MHz
// (13C, 104 - 29 = 75), 63 = 114 MHz (129Xe, 114 - 32), 107 = 58 MHz (15N,
// 58 - 28). The physical RF frequency is base + sign * NCO (-f, positive):
// the high-side masks add the NCO, the low-side ones subtract it, so there a
// physical offset +d needs NCO - d and phases / spectra are mirrored.
// pts_mask 0 means no PTS (base 0, sign +1, e.g. the simulator). nullopt for a
// mask not in the table (110 = 54 MHz is marked DO NOT USE there).
struct PtsSetting {
  double baseHz = 0;
  int sign = +1;  // +1: base + NCO, -1: base - NCO
};
std::optional<PtsSetting> LookupPts(unsigned long ptsMask);
inline std::optional<double> PtsBaseFrequencyHz(unsigned long ptsMask) {
  const auto p = LookupPts(ptsMask);
  return p ? std::optional<double>(p->baseHz) : std::nullopt;
}
// The known masks, for error messages.
inline constexpr const char* kPtsMaskList =
    "0 = none, 77 = 270 MHz, 109 = 250 MHz, 47 = 150 MHz (-NCO), "
    "190 = 104 MHz (-NCO), 63 = 114 MHz (-NCO), 107 = 58 MHz (-NCO)";

// Physical amplitude as a fraction of full scale. Throws UnitError if
// |value| > fullScale * (1 + 1e-6) (tiny float round-off is clamped).
double Fraction(double value, double fullScale, const char* what);

}  // namespace pp::units
