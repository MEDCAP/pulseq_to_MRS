// Probe 9: origin(at) semantics, for timeline rebasing past ~21 s.
//
// GA: setval(1000, 10000); origin(20000); setval(2000, 10000);
//     origin(30000); setval(3000, 5000); setval(0, 6000)
// Questions:
//   - is the channel timer reset exactly at `at` (second setval logged at
//     20000 + 10000 + 10 in simulator time)?
//   - does the simulator log keep one global timeline across origin()?
//   - do other channels (Tx, not rebased) stay on the global timeline?
#include "probe_common.hpp"

int main() {
  using namespace mrs::plato;
  using namespace mrs::plato::ppl;
  // clang-format off
  _sequence seq(
    _component("GA", _function(
      setscale(32767),
      setval(1000, 10000),
      origin(20000),
      setval(2000, 10000),
      origin(30000),
      setval(3000, 5000),
      setval(0, 6000),
      ppl::exit(0)
    )),
    _component("Tx", _function(
      setscale(32767),
      setamp(1000, 10000),
      setamp(2000, 30000),
      setamp(0, 60000),
      ppl::exit(0)
    ))
  );
  // clang-format on
  std::printf("expect GA at 10010, 30010, 55010, 56010 if origin(at) resets "
              "exactly at at; Tx at 10010, 30010, 60010\n");
  probe::Session s;
  return s.Run(seq, {probe::GA(), probe::Tx()});
}
