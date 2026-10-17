// Probe 4: control for probe 2's off-by-one. No streams, literal times only.
//
// Tx: setamp(1, 11000); setamp(2, 12000); setamp(3, 13000); then
//     either nothing (mode 0), setamp(0, 14000) (mode 1),
//     or rf() of a 2-point stream at 14000 (mode 2).
// If the log shows amplitude 1 at 12010, the OSC log line (or the hardware)
// applies setamp one command late - not a stream/read artefact.
// Also compares setamp vs set_amp_freq_phase (mode 3).
#include "probe_common.hpp"

#include <cstdlib>

int main(int argc, char** argv) {
  using namespace mrs::plato;
  using namespace mrs::plato::ppl;
  const int mode = argc > 1 ? std::atoi(argv[1]) : 0;

  _stream<fmt::Modulation> two({{0.5, 0, 0}, {0.25, 0, 0}});
  std::vector<StatementBuilder> body;
  body.push_back(setscale(32767));
  if (mode == 3) {
    body.push_back(set_amp_freq_phase(1, 0, 0, 11000));
    body.push_back(set_amp_freq_phase(2, 0, 0, 12000));
    body.push_back(set_amp_freq_phase(3, 0, 0, 13000));
  } else {
    body.push_back(setamp(1, 11000));
    body.push_back(setamp(2, 12000));
    body.push_back(setamp(3, 13000));
  }
  if (mode == 1) body.push_back(setamp(0, 14000));
  if (mode == 2) body.push_back(rf(two, 100, 14000));
  body.push_back(ppl::exit(0));

  _sequence seq(_component("Tx", _function(_compound(body))));
  std::printf("mode=%d\n", mode);
  probe::Session s;
  return s.Run(seq, {probe::Tx()});
}
