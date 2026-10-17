// Probe 7: gradient(address, N, period, at) semantics on a logical channel.
//
// lib = fmt::Gradient ramp of 8 points, value (i+1)/8, then 4 zeros.
// GA plays gradient(lib + OFF_BYTES, N, 1000, 20000).
// Questions:
//   - points played for N (rf() plays N+1)?
//   - byte size of one fmt::Gradient point (try OFF_BYTES 0 / 2 / 4)?
//   - which physical port shows GA with and without a GT component
//     (mode "gt" adds GT with setaangle/setbangle/setcangle = 0 and ...)?
//   - does the value hold after the last point?
// Usage: p07_gradient <N> <OFF_BYTES> [gt]
#include "probe_common.hpp"

#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
  using namespace mrs::plato;
  using namespace mrs::plato::ppl;
  const int n = argc > 1 ? std::atoi(argv[1]) : 4;
  const int off = argc > 2 ? std::atoi(argv[2]) : 0;
  const bool withGt = argc > 3 && std::strcmp(argv[3], "gt") == 0;

  std::vector<double> pts;
  for (int i = 0; i < 8; ++i) pts.push_back((i + 1) / 8.0);
  for (int i = 0; i < 4; ++i) pts.push_back(0.0);
  _stream<fmt::Gradient> lib(pts.begin(), pts.end());

  // clang-format off
  auto ga = _component("GA", _function(
    setscale(32767),
    seek(lib),
    gradient(GetStreamAddress(lib.get()) + off, n, 1000, 20000),
    setval(0, 50000),
    ppl::exit(0)
  ));
  // clang-format on
  std::vector<probe::Component> comps = {probe::GA()};
  std::printf("N=%d OFF=%d gt=%d\n", n, off, withGt);
  probe::Session s;
  if (!withGt) return s.Run(_sequence(std::move(ga)), comps);

  comps.push_back(probe::GT());
  // clang-format off
  auto gt = _component("GT", _function(
    setscale(32767),
    ppl::exit(0)
  ));
  // clang-format on
  return s.Run(_sequence(std::move(ga), std::move(gt)), comps);
}
