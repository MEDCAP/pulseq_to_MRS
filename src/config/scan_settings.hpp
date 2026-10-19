// Scan settings file (YAML): one file that carries everything the command line
// of pulseq_plato can set. Given as `-f settings.yml` (a -f value ending in
// .yml/.yaml is a settings file; a number is the frequency, as in the vendor
// pulseq.exe). Options on the command line override the file. Example (see
// config/*.yml):
//
//   sequence: ../test/seqs/fid_v150.seq   # relative to this file
//   output: fid                            # -o (relative to the working dir)
//   rf:
//     pts_mask: 77                         # -ptsmask  PTS base: 77 = 270 MHz
//     frequency_hz: 27998450               # -f   NCO frequency = offset from
//                                          #      the PTS base (Tx and Rx)
//     max_amplitude_hz: 100                # -rfmax
//   gradients:
//     max_hz_per_m: {x: 57290000, y: 61090000, z: 60712000}   # -gx -gy -gz
//   channels:
//     tx_mask: 1                           # bit i = transmitter i
//     rx_mask: 1                           # bit i = receiver i
//   receiver:
//     cic: off                             # -cic on|off
//   triggering:
//     respiratory: false                   # -trigger on|off: before every
//                                          #   excitation pulse (RF use 'e')
//                                          #   wait for UIO input bit 0 to rise
//                                          #   (respiratory monitor)
//   modules:                               # or  modules_file: sim_modules.txt
//     - {name: MR4600, ip: 127.0.0.1, port: 21700}   # first = master
//     - {name: MR4610, ip: 127.0.0.1, port: 21701}
//     - {name: MR4620, ip: 127.0.0.1, port: 21702}
//   advanced:                              # testing options, all optional
//     emit: loop                           # loop | unrolled
//     t0_clocks: 10000
//     epoch_clocks: 1073741824
//     timeout_s: 30
//     allow_late: false
//     message_latency_clocks: 5300         # respiratory triggering: PLATO
//                                          #   message delay (scanner 5300)
//     stdout: false
//
// Every key is optional; unknown keys are errors (typos must not pass
// silently in a scan configuration).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace pp::config {

class SettingsError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct ModuleEntry {
  std::string name;
  std::array<std::uint8_t, 4> ip{127, 0, 0, 1};
  std::uint16_t port = 0;
};

// Unset fields mean "not given": the command line or the built-in default
// decides.
struct ScanSettings {
  std::optional<std::string> sequence;
  std::optional<std::string> output;
  std::optional<unsigned long> ptsMask;
  std::optional<double> frequencyHz;  // NCO frequency, offset from the PTS base
  std::optional<double> rfMaxHz;
  std::array<std::optional<double>, 3> gradMaxHzPerM;
  std::optional<unsigned long> txMask, rxMask;
  std::optional<bool> cic;
  std::optional<bool> respiratory;  // triggering.respiratory
  std::optional<std::string> modulesFile;
  std::vector<ModuleEntry> modules;  // empty = not given
  // advanced
  std::optional<std::string> emit;
  std::optional<long long> t0Clocks, epochClocks, messageLatencyClocks;
  std::optional<double> timeoutSeconds;
  std::optional<bool> allowLate;
  std::optional<bool> toStdout;
};

// True if a -f value names a settings file (.yml / .yaml, any case).
bool IsSettingsFileName(const std::string& value);

// Reads and validates a settings file. Relative `sequence` and `modules_file`
// paths are made relative to the directory of `path`. Throws SettingsError
// (including YAML syntax errors) with "<file>:<line>:" in the message.
ScanSettings LoadScanSettings(const std::string& path);

// Parses "a.b.c.d"; throws SettingsError naming `what`.
std::array<std::uint8_t, 4> ParseIPv4(const std::string& text,
                                      const std::string& what);

}  // namespace pp::config
