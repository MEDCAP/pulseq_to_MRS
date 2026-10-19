// Shared harness for the Phase 0 simulator probes.
//
// Connects to the three simulated modules of plato/bin/sim_modules.txt
// (MR4600 @21700, MR4610 @21701, MR4620 @21702) or to a given module list
// (scanner probes, e.g. p11), compiles a sequence, runs it
// until every component has exited, and prints per-component code/data size,
// exit status and the engine status flags that matter for timing.
#pragma once

#include <ppl_language.hpp>
#include <ppl_library.hpp>
#include <report.hpp>
#include <sequence_component_mapping.hpp>
#include <spectrometer.hpp>

#include <chrono>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <WinSock2.h>
#endif

namespace probe {

using namespace mrs::plato;

struct Component {
  std::string name;
  std::string module;
  ModuleChannel channel;
};

// Default component set used by every probe. Only list what a probe uses:
// every component in the mapping must also exist in the sequence.
inline Component Uio() { return {"UIO", "MR4600", ModuleChannel::Uio(0)}; }
inline Component Tx() { return {"Tx", "MR4610", ModuleChannel::Transmitter(0)}; }
inline Component Rx() { return {"Rx", "MR4620", ModuleChannel::Receiver(0)}; }
inline Component GA() { return {"GA", "MR4600", ModuleChannel::GradientA(0)}; }
inline Component GB() { return {"GB", "MR4600", ModuleChannel::GradientB(0)}; }
inline Component GC() { return {"GC", "MR4600", ModuleChannel::GradientC(0)}; }
inline Component GT() { return {"GT", "MR4600", ModuleChannel::GradientT(0)}; }

// A module to connect to (e.g. a line of a -m modules file).
struct Endpoint {
  std::string name;
  std::uint8_t ip[4] = {127, 0, 0, 1};
  std::uint16_t port = 0;
};

class Session {
public:
  // The simulator modules.
  Session() : Session({{"MR4600", {127, 0, 0, 1}, 21700},
                       {"MR4610", {127, 0, 0, 1}, 21701},
                       {"MR4620", {127, 0, 0, 1}, 21702}}) {}

  // Any module list; the first one is the master.
  explicit Session(const std::vector<Endpoint>& modules) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    spec_ = CreateSpectrometer();
    for (const auto& m : modules) Connect(m);
    spec_->SetMasterModuleName(modules.front().name);
  }

  Spectrometer& spectrometer() { return *spec_; }

  // Compiles and runs `seq`, waits for all components to exit (or timeout),
  // prints diagnostics. Returns EXIT_SUCCESS if everything ran cleanly.
  // Hooks: beforeRun runs after compilation and before Run() (e.g. to connect
  // an AcquisitionDataPump); afterRun runs once all components have exited.
  using Hook = std::function<void(SequenceRunner&)>;

  template <typename SequenceT>
  int Run(const SequenceT& seq, const std::vector<Component>& components,
          double timeoutSeconds = 5.0) {
    return Run(seq, components, Hook{}, Hook{}, timeoutSeconds);
  }

  template <typename SequenceT>
  int Run(const SequenceT& seq, const std::vector<Component>& components,
          const Hook& beforeRun, const Hook& afterRun,
          double timeoutSeconds = 5.0) {
    SequenceComponentMapping mapping;
    for (const auto& c : components)
      mapping.Insert(c.name, {c.module, c.channel});

    Report report;
    auto [res, runner] =
        spec_->CreateSequenceRunner("probe", seq, mapping, &report);
    for (const auto& r : report.records())
      std::fprintf(stderr, "report: %s\n", r.description.c_str());
    if (res != SequenceRuntimeResult::kSuccess) {
      std::fprintf(stderr, "CreateSequenceRunner failed: %d\n",
                   static_cast<int>(res));
      return EXIT_FAILURE;
    }

    for (const auto& c : components) {
      std::uint32_t code = 0, data = 0;
      (void)runner->GetCodeSize(c.name, code);
      (void)runner->GetDataSize(c.name, data);
      ModuleChannelInformation info{};
      (void)spec_->GetModule(c.module)->GetChannelInformation(c.channel, info);
      std::printf("[%s] code=%u/%u data=%u/%u\n", c.name.c_str(), code,
                  info.engineInstructionMemorySize, data,
                  info.engineDataMemorySize);
    }

    if (beforeRun) beforeRun(*runner);
    if (runner->Run() != SequenceRuntimeResult::kSuccess) {
      std::fprintf(stderr, "Run failed\n");
      return EXIT_FAILURE;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration<double>(timeoutSeconds);
    bool allDone = false;
    bool simulatorGone = false;
    while (!allDone && std::chrono::steady_clock::now() < deadline) {
      allDone = true;
      for (const auto& c : components) {
        bool running = true;
        std::uint16_t status = 0;
        const auto q = runner->GetExitStatus(c.name, running, status);
        // kCommunicationFailure: the simulator reached --until and closed its
        // sockets, so nothing more will run. Treat as finished.
        if (q == SequenceRuntimeResult::kCommunicationFailure) {
          simulatorGone = true;
          running = false;
        } else if (q != SequenceRuntimeResult::kSuccess) {
          running = true;
        }
        allDone = allDone && !running;
      }
      if (!allDone) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    int rc = allDone ? EXIT_SUCCESS : EXIT_FAILURE;
    for (const auto& c : components) {
      bool running = true;
      std::uint16_t status = 0;
      const auto er = runner->GetExitStatus(c.name, running, status);
      ModuleChannelStatus st{};
      (void)spec_->GetModule(c.module)->GetChannelStatus(c.channel, st);
      // Note: the simulator does not model engineTimingViolation - late
      // events must be detected by comparing the log against requested times.
      std::printf("[%s] exitQuery=%d running=%d exit=%u timingViolation=%d "
                  "div0=%d gradOverrun=%d\n",
                  c.name.c_str(), static_cast<int>(er), running, status,
                  st.engineTimingViolation, st.engineDivisionByZero,
                  st.gradientOverrun);
      if (st.engineTimingViolation) rc = EXIT_FAILURE;
    }
    if (afterRun) afterRun(*runner);
    if (!allDone) std::fprintf(stderr, "timeout waiting for exit\n");
    if (simulatorGone)
      std::fprintf(stderr, "simulator closed the connection (--until reached "
                           "before exit was observed)\n");
    spec_->DeleteSequenceRunner("probe");
    return rc;
  }

private:
  void Connect(const Endpoint& e) {
    auto [r, m] = spec_->CreateModule(
        e.name, IPv4Endpoint(IPv4Address(e.ip[0], e.ip[1], e.ip[2], e.ip[3]),
                             e.port));
    if (r != Module::Result::kSuccess) {
      std::fprintf(stderr, "Failed to connect %s at %u.%u.%u.%u:%u (is "
                           "simulator.exe running?)\n", e.name.c_str(), e.ip[0],
                   e.ip[1], e.ip[2], e.ip[3], e.port);
      std::exit(EXIT_FAILURE);
    }
  }

  std::unique_ptr<Spectrometer> spec_;
};

}  // namespace probe
