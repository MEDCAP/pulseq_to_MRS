// Turns the per-channel block-row tables into a PLATO realtime sequence.
//
// Components (one CPU each): Tx, Rx, GA, GB, GC, and UIO when a PTS mask is
// set.  Every component runs the same
// shape of program (plato docs 4.2, 4.6; test/FINDINGS.md):
//
//   init                      setscale / setfreq / setph / resetosc
//   for each row of its table seek(table + pos); kind = read(); at = read();
//                             _switch(kind) { play the row's event }
//   exit(0)
//
// Two emitters produce the same timed calls from the same rows:
//   kBlockLoop  the realtime loop above; code size does not depend on the
//               sequence length (the production path)
//   kUnrolled   one straight-line statement group per row with literal
//               arguments; debug only, for small sequences (FINDINGS T6)
// Phase 2 requires both to give identical simulator logs.
//
// Per-row calls (identical in both emitters):
//   kRf    setph(phase, gateOn - lead); unblank(gateOn);
//          rf(lib + off, pointsMinus1, period, at); blank(gateOff)
//   kGrad  gradient(lib + off, pointsMinus1, period, at)
//   kAdc   setfreq(freq, at - lead); setph(phase, at - lead);
//          acquire2(n, dwell, grid, at); if (restoreAt) setfreq(base, restoreAt)
//   kEnd   resume(at): every channel exits at the end of the sequence
#pragma once

#include "tables/channel_table.hpp"

#include <sequence_definition.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pp::realtime {

enum class Emitter { kBlockLoop, kUnrolled };

enum class Role { kTx, kRx, kGradA, kGradB, kGradC, kUio };

struct ComponentInfo {
  std::string name;  // component name in the sequence and the mapping
  Role role;
  std::size_t rows = 0;
};

struct ProgramOptions {
  Emitter emitter = Emitter::kBlockLoop;
  bool cicCompensation = false;  // Rx FIR: CicCompensation (decimates by 4)
                                 // instead of Transparent
  model::Clk rxFreqLeadClk = 51; // must match ScanConfig::rxFreqLeadClk
  model::Clk txPhaseLeadClk = 50; // must match ScanConfig::txPhaseLeadClk
  model::Clk ncoResetAt = 5000;  // resetosc(0) on Tx and Rx, before t = 0
  // PTS synthesizer selection: UIO output word (pts_mask << 8, bits 8-15, as
  // the vendor pulseq.exe) set at uioAt and held until the sequence ends.
  // 0 = no UIO component.
  std::uint32_t uioOutWord = 0;
  model::Clk uioAt = 100;
};

struct Program {
  mrs::plato::SequenceDefinitionBuilder sequence;
  std::vector<ComponentInfo> components;
};

// Throws std::runtime_error if the tables cannot be expressed (e.g. an
// unrolled program that would obviously not fit the instruction memory).
Program BuildProgram(const tables::ChannelTables& t, const ProgramOptions& o);

const char* RoleName(Role r);

}  // namespace pp::realtime
