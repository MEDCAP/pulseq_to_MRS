// Probe 1: smallest possible run. UIO bit 0 high at clock 100, low at 200.
// Expect in the simulator log: UOUT at 100+latency and 200+latency.
#include "probe_common.hpp"

int main() {
  using namespace mrs::plato;
  using namespace mrs::plato::ppl;
  // clang-format off
  _sequence seq(
    _component("UIO", _function(
      setout(1, 100),
      setout(0, 200),
      ppl::exit(0)
    ))
  );
  // clang-format on
  probe::Session s;
  return s.Run(seq, {probe::Uio()});
}
