#include "check.hpp"
#include "units/units.hpp"

#include <numbers>

using namespace pp::units;

static void Run() {
  const Clocks c;

  // Times: exact multiples of 10 ns only.
  CHECK_EQ(MicrosecondsToClocks(1, c, "t"), 100);
  CHECK_EQ(MicrosecondsToClocks(10, c, "t"), 1000);
  CHECK_EQ(NanosecondsToClocks(100, c, "t"), 10);
  CHECK_EQ(SecondsToClocks(1.0, c, "t"), 100000000);
  CHECK_THROWS(NanosecondsToClocks(15, c, "t"), UnitError);

  // NCO words: 2^32 * f / 400 MHz, wrapped.
  CHECK_EQ(FrequencyWord(100e6, c), 0x40000000u);
  CHECK_EQ(FrequencyWord(0, c), 0u);
  CHECK_EQ(FrequencyWord(-100e6, c), 0xC0000000u);
  CHECK_EQ(FrequencyWord(27995000, c), 300594024u);  // round(2^32 * 0.0699875)
  CHECK_NEAR(ModulationFrequency(200e6, c), 1.0, 1e-12);
  CHECK_NEAR(ModulationFrequency(1000, c), 5e-6, 1e-15);
  CHECK_THROWS(ModulationFrequency(250e6, c), UnitError);

  // Phases.
  const double pi = std::numbers::pi;
  CHECK_EQ(PhaseWord16(pi / 2), 16384u);
  CHECK_EQ(PhaseWord16(-pi / 2), 49152u);
  CHECK_EQ(PhaseWord16(2 * pi), 0u);
  CHECK_EQ(PhaseWord16(0.25), 2608u);
  CHECK_EQ(PhaseWord32(pi / 2), 0x40000000u);
  CHECK_NEAR(WrapPi(3 * pi), -pi, 1e-12);
  CHECK_NEAR(ModulationPhase(pi / 2), 0.5, 1e-12);
  CHECK_NEAR(ModulationPhase(-pi / 4), -0.25, 1e-12);

  // PTS settings (the scanner's C:\smis\smis.ini [PplVars]).
  CHECK_NEAR(*PtsBaseFrequencyHz(77), 270e6, 0);
  CHECK_EQ(LookupPts(77)->sign, 1);
  CHECK_NEAR(LookupPts(190)->baseHz, 104e6, 0);  // 13C: 104 - 29 = 75 MHz
  CHECK_EQ(LookupPts(190)->sign, -1);
  CHECK_NEAR(LookupPts(109)->baseHz, 250e6, 0);
  CHECK_EQ(LookupPts(47)->sign, -1);
  CHECK_NEAR(*PtsBaseFrequencyHz(0), 0, 0);
  CHECK(!PtsBaseFrequencyHz(78).has_value());
  CHECK(!LookupPts(15).has_value());   // not on this scanner
  CHECK(!LookupPts(110).has_value());  // DO NOT USE in smis.ini

  // Full-scale fractions.
  CHECK_NEAR(Fraction(50, 100, "a"), 0.5, 1e-15);
  CHECK_NEAR(Fraction(-100, 100, "a"), -1.0, 1e-15);
  CHECK_THROWS(Fraction(101, 100, "a"), UnitError);
  CHECK_THROWS(Fraction(1, 0, "a"), UnitError);
}

TEST_MAIN(Run)
