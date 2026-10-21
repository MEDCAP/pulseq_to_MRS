// Host side of a run: connects to the modules, maps the program's components
// onto module channels, runs the sequence, pumps the acquisition data into a
// .cfl file and reports per-component exit status.
//
// Modules file (-m), one module per line, as plato/bin/sim_modules.txt:
//     <name> <IPv4 address> <port>        ('#' starts a comment)
// The first module is the master. Channels are found by asking each module
// what it has: Tx on the first module with a transmitter, Rx on the first with
// a receiver, GA/GB/GC on the first with a gradient partial.
#pragma once

#include "realtime/program.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pp::plato {

struct ModuleAddress {
  std::string name;
  std::uint8_t ip[4] = {127, 0, 0, 1};
  std::uint16_t port = 0;
};

// Throws std::runtime_error on a malformed or empty file.
std::vector<ModuleAddress> ReadModulesFile(const std::string& path);

struct RunOptions {
  std::vector<ModuleAddress> modules;
  std::string outputPrefix;   // -o: writes <prefix>_0.cfl (receiver 0)
  bool dataToStdout = false;  // -stdout: complex64 samples to stdout instead
  std::uint64_t expectedSamples = 0;  // total ADC samples, to know when the
                                      // pump has everything
  double timeoutSeconds = 0;  // 0 = sequence duration + 10 s + 60 s per gate
  double sequenceSeconds = 0;
  int gates = 0;              // respiratory gates: the run waits for each
  std::FILE* log = stdout;    // diagnostics (stderr with -stdout)
  unsigned txChannel = 0;     // -txmask bit: transmitter channel on the Tx module
  unsigned rxChannel = 0;     // -rxmask bit: receiver channel on the Rx module
  bool conjugateData = false; // low-side PTS: write conj(samples), so +Hz in
                              // the data is a higher physical frequency
};

// Returns the process exit code: 0 on success.
int Run(const realtime::Program& program, const RunOptions& options);

}  // namespace pp::plato
