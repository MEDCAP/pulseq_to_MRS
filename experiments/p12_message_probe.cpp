// Probe 12: inter-channel message behaviour that respiratory triggering relies
// on (UIO detects the gate and broadcasts a message; Tx, Rx and the gradient
// channels wait for it with receivel and then restart their timelines).
//
// Questions, answered by the printed rounds and summary:
//   (a) buffering: is a message that activates while the receiver is NOT
//       blocked in receivel delivered later, or dropped? (-late: Tx starts
//       listening that long after the first gates were sent. Every report
//       carries its round number, so a dropped gate shows as a jump in the
//       Tx round tags.)
//   (b) simultaneity: do Tx (MR4610), Rx (MR4620) and GA (MR4600) wake at the
//       same timer value after a gate message? (printed per round, relative
//       to the UIO's own send time, modulo 2^16 clocks = 655 us)
//   (c) loss: does every message arrive over many rounds? (-rounds; 4
//       messages per round; a lost message hangs the round -> timeout and
//       "rounds received" below the expected count)
//
// Per round k (period -period ms, UIO timeline resume(T0 + k * period)):
//   UIO  t = now(); sendl(GATE); delay 20 us; sendl(ID_UIO | k | t)
//   Tx   receivel(GATE); t = now(); delay 40 us; sendl(ID_TX | k | t)
//   GA   receivel(GATE); t = now(); delay 60 us; sendl(ID_GA | k | t)
//   Rx   receivel(GATE); t = now(); meta(k), meta(t); three times
//        x = receivel(REPORT mask); meta(x); complete()
// Report word: id << 24 | (round & 0xFF) << 16 | (now() & 0xFFFF). The reports
// are staggered by >= 20 us so that each one activates while Rx is blocked in
// its next receivel (a first version sent them back to back and Rx only saw
// those that arrived while it was waiting). GATE is bit 30, reports use bits
// 0-25, so a receivel with one mask never matches the other kind. Timers are
// never reset (21 s limit).
//
// Usage: p12_message_probe [-m modules.txt] [-rounds N] [-period ms] [-late ms] [-raw]
//   -m       modules file (default: simulator ports 21700-21702)
//   -rounds  default 100 (the soak: -rounds 2500 -period 5 = 10 000 messages)
//   -period  default 10 ms, at least 2
//   -late    one-off delay (ms) before the first receivel of Tx, default 0
//   -raw     also print the five metadata words of every round
#include "probe_common.hpp"

#include "plato/session.hpp"

#include <acquisition_data_pump.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace {

constexpr double kClockHz = 100e6;
constexpr std::int32_t kT0 = 100000;  // first round at 1 ms
constexpr std::int32_t kGate = 0x40000000;
constexpr std::int32_t kReportMask = 0x03FFFFFF;
constexpr std::int32_t kIdTx = 1 << 24, kIdGa = 2 << 24, kIdUio = 3 << 24;
constexpr std::int32_t kUioDelay = 2000, kTxDelay = 4000, kGaDelay = 6000;

class Printer : public mrs::plato::AcquisitionDataPump::Delegate {
public:
  explicit Printer(bool raw) : raw_(raw) {}

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
    if (b.size() != 20) {
      std::fprintf(stderr, "msg acq=%u: unexpected %zu bytes\n", acqId,
                   b.size());
    } else {
      auto be = [&](int k) {
        const auto* p = b.data() + 4 * k;
        return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
               (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
      };
      if (raw_)
        std::fprintf(stderr, "raw acq=%u: %08X %08X %08X %08X %08X\n", acqId,
                     be(0), be(1), be(2), be(3), be(4));
      const std::uint32_t round = be(0), tRx = be(1) & 0xFFFF;
      std::uint32_t t[4] = {}, tag[4] = {};  // by id: 1 Tx, 2 GA, 3 UIO
      bool ok = true;
      for (int k = 2; k < 5; ++k) {
        const std::uint32_t id = (be(k) >> 24) & 3;
        if (id < 1 || t[id] || tag[id]) ok = false;
        else {
          tag[id] = ((be(k) >> 16) & 0xFF) + 1;  // +1: 0 = absent
          t[id] = be(k) & 0xFFFF;
        }
      }
      if (!ok) {
        std::fprintf(stderr, "msg round=%u: bad reports 0x%08X 0x%08X 0x%08X\n",
                     round, be(2), be(3), be(4));
      } else {
        const double us = 1e6 / kClockHz;
        const std::uint32_t dTx = (t[1] - t[3]) & 0xFFFF,
                            dGa = (t[2] - t[3]) & 0xFFFF,
                            dRx = (tRx - t[3]) & 0xFFFF;
        const std::uint32_t want = (round & 0xFF) + 1;
        const bool tagsOk = tag[1] == want && tag[2] == want && tag[3] == want;
        std::fprintf(stderr,
                     "msg round=%u tags UIO %d Tx %d GA %d%s  gate->wake: Tx "
                     "%.2f us, GA %.2f us, Rx %.2f us\n",
                     round, int(tag[3]) - 1, int(tag[1]) - 1, int(tag[2]) - 1,
                     tagsOk ? "" : " (TAG MISMATCH)", dTx * us, dGa * us,
                     dRx * us);
        if (tagsOk) {
          Add(0, dTx);
          Add(1, dGa);
          Add(2, dRx);
        } else {
          ++mismatched_;
        }
        ++rounds_;
      }
    }
    pending_.erase(acqId);
  }

  void Summary(int expected) {
    std::lock_guard<std::mutex> lock(m_);
    static const char* kNames[3] = {"Tx", "GA", "Rx"};
    std::fprintf(stderr,
                 "msg summary: %d of %d rounds received, %d with a round-tag "
                 "mismatch\n",
                 rounds_, expected, mismatched_);
    for (int i = 0; i < 3; ++i)
      if (n_[i])
        std::fprintf(stderr,
                     "msg %s wake after the gate send: min %.2f, mean %.2f, max "
                     "%.2f us (%d rounds)\n",
                     kNames[i], min_[i] * 1e6 / kClockHz,
                     sum_[i] / n_[i] * 1e6 / kClockHz, max_[i] * 1e6 / kClockHz,
                     n_[i]);
  }
  int rounds() {
    std::lock_guard<std::mutex> lock(m_);
    return rounds_;
  }

private:
  void Add(int i, std::uint32_t d) {
    if (!n_[i]) min_[i] = max_[i] = d;
    min_[i] = std::min(min_[i], d);
    max_[i] = std::max(max_[i], d);
    sum_[i] += d;
    ++n_[i];
  }
  bool raw_;
  std::mutex m_;
  std::map<std::uint32_t, std::vector<std::uint8_t>> pending_;
  int rounds_ = 0, mismatched_ = 0;
  std::uint32_t min_[3] = {}, max_[3] = {};
  double sum_[3] = {};
  int n_[3] = {};
};

}  // namespace

int main(int argc, char** argv) {
  using namespace mrs::plato;
  using namespace mrs::plato::ppl;

  std::string modulesFile;
  int rounds = 100;
  double periodMs = 10, lateMs = 0;
  bool raw = false;
  for (int k = 1; k < argc; ++k) {
    const char* a = argv[k];
    if (!std::strcmp(a, "-raw")) { raw = true; continue; }
    if (k + 1 >= argc) {
      std::fprintf(stderr, "%s needs a value\n", a);
      return 2;
    }
    const char* v = argv[++k];
    if (!std::strcmp(a, "-m")) modulesFile = v;
    else if (!std::strcmp(a, "-rounds")) rounds = std::atoi(v);
    else if (!std::strcmp(a, "-period")) periodMs = std::atof(v);
    else if (!std::strcmp(a, "-late")) lateMs = std::atof(v);
    else {
      std::fprintf(stderr, "unknown option %s\n", a);
      return 2;
    }
  }
  const auto periodClk =
      static_cast<std::int32_t>(std::lround(periodMs * 1e-3 * kClockHz));
  const auto lateClk =
      static_cast<std::int32_t>(std::lround(lateMs * 1e-3 * kClockHz));
  if (rounds < 1 || periodClk < 200000 ||
      kT0 + static_cast<double>(rounds) * periodClk >= 2147483647.0 ||
      lateMs < 0) {
    std::fprintf(stderr, "need -rounds >= 1, -period >= 2 ms, rounds * period "
                         "< 21 s, -late >= 0\n");
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

  _variable ku, tu;
  auto uio = _component(
      "UIO",
      _function(_for(ku = 0, ku < rounds, ++ku)(
                    resume(kT0 + ku * periodClk), tu = now(), sendl(kGate),
                    delay(kUioDelay),
                    sendl(kIdUio | ((ku & 0xFF) << 16) | (tu & 0xFFFF))),
                ppl::exit(0)));

  _variable kt, tt;
  auto tx = _component(
      "Tx", _function(delay(lateClk),
                      _for(kt = 0, kt < rounds, ++kt)(
                          receivel(kGate), tt = now(), delay(kTxDelay),
                          sendl(kIdTx | ((kt & 0xFF) << 16) | (tt & 0xFFFF))),
                      ppl::exit(0)));

  _variable kg, tg;
  auto ga = _component(
      "GA", _function(_for(kg = 0, kg < rounds, ++kg)(
                          receivel(kGate), tg = now(), delay(kGaDelay),
                          sendl(kIdGa | ((kg & 0xFF) << 16) | (tg & 0xFFFF))),
                      ppl::exit(0)));

  _variable kr, tr, x;
  auto rx = _component(
      "Rx",
      _function(_for(kr = 0, kr < rounds, ++kr)(
                    receivel(kGate), tr = now(), meta(kr), meta(tr),
                    x = receivel(kReportMask), meta(x),
                    x = receivel(kReportMask), meta(x),
                    x = receivel(kReportMask), meta(x), complete()),
                ppl::exit(0)),
      _filter::Transparent());
  _sequence seq(std::move(uio), std::move(tx), std::move(rx), std::move(ga));

  std::printf("message probe: %d rounds every %.3f ms (%.3f s), Tx late %.3f "
              "ms, modules %s\n",
              rounds, periodClk / kClockHz * 1e3,
              rounds * (periodClk / kClockHz), lateMs,
              modulesFile.empty() ? "simulator" : modulesFile.c_str());
  Printer printer(raw);
  AcquisitionDataPump pump(&printer);
  std::thread worker;
  auto s = modules.empty() ? probe::Session() : probe::Session(modules);
  probe::InstallCtrlCStop();
  int rc = s.Run(
      seq, {probe::Uio(), probe::Tx(), probe::Rx(), probe::GA()},
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
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (printer.rounds() < rounds &&
               std::chrono::steady_clock::now() < until)
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        pump.Stop();
        if (worker.joinable()) worker.join();
      },
      rounds * (periodClk / kClockHz) + lateMs * 1e-3 + 10);
  printer.Summary(rounds);
  if (printer.rounds() != rounds) rc = EXIT_FAILURE;
  return rc;
}
