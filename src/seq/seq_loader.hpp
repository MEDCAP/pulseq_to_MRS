// Loads a Pulseq file with the upstream ExternalSequence reader and converts it
// into model::SequenceModel. This is the host-side block loop of parsemr.cpp:
//   for (iB...) { GetBlock(iB); decodeBlock(); ...extract events... }
#pragma once

#include "model/sequence_model.hpp"
#include "units/units.hpp"

#include <stdexcept>
#include <string>

namespace pp::seq {

struct LoadOptions {
  // Physical RF frequency f0 = PTS base (pts_mask) + NCO frequency (-f); the
  // ppm columns of Pulseq 1.5 scale with it (freqPPM * 1e-6 * f0).
  double larmorHz = 0;
  bool allowPre15 = false;   // v1 targets Pulseq 1.5.x; older files are refused
  units::Clocks clocks;
};

class LoadError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Throws LoadError (parse / unsupported feature) or units::UnitError (a time
// that is not a whole number of RT clocks).
model::SequenceModel LoadSequence(const std::string& path,
                                  const LoadOptions& options);

}  // namespace pp::seq
