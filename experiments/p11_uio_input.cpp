// Probe 11: read the Uncommitted Input port (UIO) at a fixed rate and print
// every value on the host. First step towards respiratory gating: the
// SA Instruments 1030 gating module is cabled to the SYSTEM USER connector,
// which should show up on the MR4600 UIO input (vendor gradient_echo.cpp polls
// input(0xFFFFFFFF) on Uio(0)).
//
// input() works only on the UIO channel and meta() (data to the host) only on
// a receiver, so:
//   UIO  for i < N: resume(T0 + i * period); v = input(mask);
//                   sendl three words 0x8000 | 14 bits (bits 0-13, 14-27,
//                   28-31: receivel only returns non-zero masked words)
//        sendl(0x4000) (end); delay 1 ms; exit(0)
//   Rx   per sample: receive 3 words, rebuild v; meta(i), meta(v),
//        meta(now()), complete(): one acquisition per sample
//   host prints each acquisition to stderr as it arrives.
// No RF, no gradients. The UIO outputs (PTS bits) stay 0 during the run.
//
// `at` arguments are signed 32-bit, so all sample times must stay below 2^31
// clocks (21.47 s): -s is capped at 21.
//
// Usage: p11_uio_input [-m modules.txt] [-s seconds] [-period ms] [-mask m]
//   -m       modules file (name ip port per line, first = master);
//            default: simulator ports 21700-21702
//   -s       duration, default 20, max 21
//   -period  sample period in ms, default 10 (100 Hz)
//   -mask    input mask, default 0xFFFFFFFF
#include "probe_common.hpp"

#include "plato/session.hpp"

#include <acquisition_data_pump.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace {

constexpr double kClockHz = 100e6;
constexpr std::int32_t kT0 = 100000;  // first sample at 1 ms
constexpr std::int32_t kEnd = 0x4000;
constexpr std::int32_t kData = 0x8000;

class Printer : public mrs::plato::AcquisitionDataPump::Delegate {
public:
  Printer(std::int32_t periodClk) : periodClk_(periodClk) {}

  void AcquisitionDataPump_Submit(mrs::plato::AcquisitionDataPump*, unsigned,
                                  std::uint32_t acqId,
                                  std::uint32_t fragmentOffset, bool last,
                                  const void* payload,
                                  std::size_t size) override {
    std::lock_guard<std::mutex> lock(m_);
    auto& b = pending_[acqId];
    if (b.size() < fragmentOffset + size) b.resize(fragmentOffset + size);
    std::memcpy(b.data() + fragmentOffset, payload, size);
    if (!last) return;
    if (b.size() != 12) {
      std::fprintf(stderr, "uio acq=%u: unexpected %zu bytes\n", acqId,
                   b.size());
    } else {
      auto be = [&](int k) {
        const auto* p = b.data() + 4 * k;
        return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
               (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
      };
      const std::uint32_t i = be(0), v = be(1), tRx = be(2);
      const double tSample = kT0 + static_cast<double>(i) * periodClk_;
      std::fprintf(stderr,
                   "uio i=%u t=%.3f s value=0x%08X (dec %u) rx_latency=%.2f us\n",
                   i, tSample / kClockHz, v, v, (tRx - tSample) / kClockHz * 1e6);
      if (count_ == 0) first_ = v;
      changed_ |= v ^ first_;
      ++count_;
    }
    pending_.erase(acqId);
  }

  void Summary(std::uint32_t expected) {
    std::lock_guard<std::mutex> lock(m_);
    std::fprintf(stderr,
                 "uio summary: %u of %u samples, first value 0x%08X, bits that "
                 "changed 0x%08X\n",
                 count_, expected, first_, changed_);
  }
  std::uint32_t count() {
    std::lock_guard<std::mutex> lock(m_);
    return count_;
  }

private:
  std::mutex m_;
  std::int32_t periodClk_;
  std::map<std::uint32_t, std::vector<std::uint8_t>> pending_;
  std::uint32_t count_ = 0, first_ = 0, changed_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  using namespace mrs::plato;
  using namespace mrs::plato::ppl;

  std::string modulesFile;
  double seconds = 20, periodMs = 10;
  std::uint32_t mask = 0xFFFFFFFF;
  for (int k = 1; k < argc; ++k) {
    const char* a = argv[k];
    if (k + 1 >= argc) {
      std::fprintf(stderr, "%s needs a value\n", a);
      return 2;
    }
    const char* v = argv[++k];
    if (!std::strcmp(a, "-m")) modulesFile = v;
    else if (!std::strcmp(a, "-s")) seconds = std::atof(v);
    else if (!std::strcmp(a, "-period")) periodMs = std::atof(v);
    else if (!std::strcmp(a, "-mask")) mask = std::strtoul(v, nullptr, 0);
    else {
      std::fprintf(stderr, "unknown option %s\n", a);
      return 2;
    }
  }
  const auto periodClk =
      static_cast<std::int32_t>(std::lround(periodMs * 1e-3 * kClockHz));
  const auto n = static_cast<std::int32_t>(seconds * kClockHz / periodClk);
  if (periodClk < 1000 || n < 1 ||
      kT0 + static_cast<double>(n) * periodClk >= 2147483647.0) {
    std::fprintf(stderr, "need -period >= 0.01 ms and -s <= 21 (signed 32-bit "
                         "timeline), got -s %g -period %g\n", seconds, periodMs);
    return 2;
  }

  std::vector<probe::Endpoint> modules;
  if (!modulesFile.empty()) {
    try {
      for (const auto& m : pp::plato::ReadModulesFile(modulesFile))
        modules.push_back(
            {m.name, {m.ip[0], m.ip[1], m.ip[2], m.ip[3]}, m.port});
    } catch (const std::exception& e) {
      std::fprintf(stderr, "error: %s\n", e.what());
      return 2;
    }
  }

  _variable i, v;
  auto uio = _component(
      "UIO",
      _function(
          _for(i = 0, i < n, ++i)(
              resume(kT0 + i * periodClk),
              v = input(static_cast<std::int32_t>(mask)),
              sendl(kData | (v & 0x3FFF)),
              sendl(kData | ((v >> 14) & 0x3FFF)),
              sendl(kData | ((v >> 28) & 0xF))),
          sendl(kEnd),
          // Scanner run 1: exit right after sendl lost the end marker (Rx
          // never exited). Give the message time to leave before exiting.
          delay(100000),
          ppl::exit(0)));

  _variable j, w, x, t;
  auto rx = _component(
      "Rx",
      _function(
          j = 0,
          w = receivel(0xFFFF),
          _while(w != kEnd)(
              x = w & 0x3FFF,
              w = receivel(0xFFFF),
              x = x | ((w & 0x3FFF) << 14),
              w = receivel(0xFFFF),
              x = x | ((w & 0xF) << 28),
              t = now(),
              meta(j), meta(x), meta(t),
              complete(),
              ++j,
              w = receivel(0xFFFF)),
          ppl::exit(0)),
      _filter::Transparent());
  _sequence seq(std::move(uio), std::move(rx));

  std::printf("uio probe: %d samples every %.3f ms (%.3f s), mask 0x%08X, "
              "modules %s\n",
              n, periodClk / kClockHz * 1e3, n * (periodClk / kClockHz), mask,
              modulesFile.empty() ? "simulator" : modulesFile.c_str());
  Printer printer(periodClk);
  AcquisitionDataPump pump(&printer);
  std::thread worker;
  auto s = modules.empty() ? probe::Session() : probe::Session(modules);
  int rc = s.Run(
      seq, {probe::Uio(), probe::Rx()},
      [&](SequenceRunner& r) {
        auto src = r.GetAcquisitionDataSource("Rx");
        if (!src || pump.Connect(*src, 0) !=
                        AcquisitionDataPump::ConnectResult::kSuccess) {
          std::fprintf(stderr, "pump connect failed\n");
          return;
        }
        worker = std::thread([&] { pump.Run(); });
      },
      [&](SequenceRunner&) {
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::seconds(2);
        while (printer.count() < static_cast<std::uint32_t>(n) &&
               std::chrono::steady_clock::now() < until)
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        pump.Stop();
        if (worker.joinable()) worker.join();
      },
      seconds + 10);
  printer.Summary(static_cast<std::uint32_t>(n));
  if (printer.count() != static_cast<std::uint32_t>(n)) rc = EXIT_FAILURE;
  return rc;
}
