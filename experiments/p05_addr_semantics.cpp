// Probe 5: exact semantics of the address-based stream API.
//
// lib = fmt::Modulation ramp of 8 points, amplitude (i+1)/8 -> each point has a
//       distinct OSC value (~4095 * (i+1)), then 8 points of amplitude 0.
// table = fmt::Raw {1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000}
//
// mode "n":    rf(lib + 0, N, 100, 10000) for N from argv  -> how many points play
// mode "seek": seek(GetStreamAddress(table) + OFF) ; v = read(); setamp(v, 5000)
//              and seek(table, OFF) ; w = read(); setamp(w, 6000)
//              -> which element each lands on (logged amp = v*32767>>15 = v-1)
// Usage: p05_addr_semantics n <N> | seek <OFF>
#include "probe_common.hpp"

#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
  using namespace mrs::plato;
  using namespace mrs::plato::ppl;
  const bool seekMode = argc > 1 && std::strcmp(argv[1], "seek") == 0;
  const int arg = argc > 2 ? std::atoi(argv[2]) : 4;

  _stream<fmt::Modulation> lib;
  lib.AppendSegment(8, [](std::size_t i) {
    return fmt::Modulation::ValueType{(i + 1) / 8.0, 0, 0};
  });
  lib.AppendSegment(8, [](std::size_t) {
    return fmt::Modulation::ValueType{0, 0, 0};
  });
  _stream<fmt::Raw> table(
      {1000u, 2000u, 3000u, 4000u, 5000u, 6000u, 7000u, 8000u});
  _variable v, w;

  std::vector<StatementBuilder> body;
  body.push_back(setscale(32767));
  if (!seekMode) {
    body.push_back(rf(GetStreamAddress(lib.get()), arg, 100, 10000));
  } else {
    // Touch lib so it is loaded too (streams never seek'd are not loaded).
    body.push_back(seek(lib));
    body.push_back(seek(GetStreamAddress(table.get()) + arg));
    body.push_back(v = read());
    body.push_back(setamp(v, 5000));
    body.push_back(seek(table, arg));
    body.push_back(w = read());
    body.push_back(setamp(w, 6000));
  }
  body.push_back(setamp(0, 20000));
  body.push_back(ppl::exit(0));

  _sequence seq(_component("Tx", _function(_compound(body))));
  std::printf("mode=%s arg=%d\n", seekMode ? "seek" : "n", arg);
  probe::Session s;
  return s.Run(seq, {probe::Tx()});
}
