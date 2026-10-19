// Probe 11: read the Uncommitted Input port (UIO) at a fixed rate and print
// every value on the host. First step towards respiratory gating: the
// SA Instruments 1030 gating module is cabled to the SYSTEM USER connector,
// which should show up on the MR4600 UIO input (vendor gradient_echo.cpp polls
// input(0xFFFFFFFF) on Uio(0)).
//
// input() works only on the UIO channel and meta() (data to the host) only on
// a receiver, so:
//   UIO  for i < N: resume(T0); v = input(mask);
//                   sendl three words 0x8000 | 14 bits (bits 0-13, 14-27,
//                   28-31: receivel only returns non-zero masked words)
//                   origin(period - latency): the next epoch starts exactly
//                   one period later, so resume(T0) samples at T0 + i * period
//                   and the timer never nears the signed 32-bit `at` limit
//        exit(0)
//   Rx   per sample: receive 3 words, rebuild v; meta(i), meta(v),
//        meta(now()), complete(): one acquisition per sample; exits after N.
//        Its timer is never reset, so now() wraps every 42.9 s; the host
//        takes the latency modulo 2^32.
//   host prints each acquisition to stderr as it arrives.
// No RF, no gradients. The UIO outputs (PTS bits) stay 0 during the run.
// No end-of-run message: one sent after the loop never reached Rx on the
// scanner (runs 1 and 2), so both channels stop on the sample count. Ctrl+C
// stops the sequence runner (the realtime loop would otherwise keep running).
//
// Usage: p11_uio_input [-m modules.txt] [-s seconds] [-period ms] [-mask m]
//   -m       modules file (name ip port per line, first = master);
//            default: simulator ports 21700-21702
//   -s       duration, default 20
//   -period  sample period in ms, default 10 (100 Hz), at least 2
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
constexpr std::int32_t kT0 = 100000;  // sample at 1 ms into each epoch
constexpr std::int32_t kData = 0x8000;
// origin(at) takes effect at at + 10 clocks (FINDINGS T10,
// ScanConfig::originLatencyClk).
constexpr std::int32_t kOriginLatency = 10;

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
      // Rx's timer runs free from the start and wraps at 2^32.
      const auto latency = static_cast<std::int32_t>(
          tRx - static_cast<std::uint32_t>(std::fmod(tSample, 4294967296.0)));
      std::fprintf(stderr,
                   "uio i=%u t=%.3f s value=0x%08X (dec %u) rx_latency=%.2f us\n",
                   i, tSample / kClockHz, v, v, latency / kClockHz * 1e6);
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
  const double samples = std::floor(seconds * kClockHz / periodClk);
  // The epoch must hold the sample at T0 plus the sends before origin().
  if (periodClk < 2 * kT0 || samples < 1 || samples > 2147483647.0) {
    std::fprintf(stderr, "need -period >= 2 ms and at least one sample, got "
                         "-s %g -period %g\n", seconds, periodMs);
    return 2;
  }
  const auto n = static_cast<std::int32_t>(samples);

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
              resume(kT0),
              v = input(static_cast<std::int32_t>(mask)),
              sendl(kData | (v & 0x3FFF)),
              sendl(kData | ((v >> 14) & 0x3FFF)),
              sendl(kData | ((v >> 28) & 0xF)),
              origin(periodClk - kOriginLatency)),
          ppl::exit(0)));

  // Rx stops after n samples. Scanner runs 1 and 2 ended on an end-marker
  // message (sendl(0x4000) after the loop, with or without a 1 ms delay
  // before exit) that never reached Rx, while all 6000 data words did.
  _variable j, w, x, t;
  auto rx = _component(
      "Rx",
      _function(
          _for(j = 0, j < n, ++j)(
              w = receivel(0xFFFF),
              x = w & 0x3FFF,
              w = receivel(0xFFFF),
              x = x | ((w & 0x3FFF) << 14),
              w = receivel(0xFFFF),
              x = x | ((w & 0xF) << 28),
              t = now(),
              meta(j), meta(x), meta(t),
              complete()),
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
  probe::InstallCtrlCStop();
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
