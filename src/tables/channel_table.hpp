// Per-channel block-row tables: the data the realtime block loop reads.
//
// One table per PLATO component (Tx, Rx, GA, GB, GC), rows in time order. The
// realtime loop on each channel does, per row (Phase 2):
//     seek(tableAddr + pos); kind = read(); at = read(); ... ; play; pos += 32
// A row is 8 x 32-bit words = 32 bytes = one 256-bit stream prefetch block
// (plato docs 4.6), so each row read costs at most one cache fill.
//
// Row layouts (word index):
//   all      0 kind          1 at (absolute RT clock, incl. start offset)
//   kRf      2 libByteOffset 3 pointsMinus1  4 periodClk
//            5 gateOnAt (unblank)  6 gateOffAt (blank, rfGateHoldClk after
//            the trailing zero point)  7 phaseWord16 (setph at gateOn -
//            txPhaseLeadClk: phase offset minus offset phase of earlier pulses)
//   kGrad    2 libByteOffset 3 pointsMinus1  4 periodClk
//   kAdc     2 numSamples    3 dwellClk      4 freqWord (absolute NCO word)
//            5 phaseWord16   6 freqRestoreAt (0 = no offset, keep base freq)
//            7 gridClk
//            setfreq runs at `at - rxFreqLeadClk`; setph and acquire2 follow
//            back to back, and the lead is chosen so that acquire2 executes
//            (and resets the CIC grid, FINDINGS R2) exactly at `at`.
//   kEnd     (at = end of sequence)
//   kOrigin  origin(at): the channel timer restarts at at + originLatencyClk,
//            and later rows count from there
//   kTrigger respiratory gate (ScanConfig::respiratoryTrigger), one per
//            excitation block on EVERY channel incl. UIO, same order.
//            w1 nominal halt time (start of the excitation block), w2 delay
//            from the release to the new epoch (origin(now() + w2); UIO adds
//            the message latency so its epoch starts with the others'),
//            w3/w4 Rx CIC grid and setgrid time if the next row is an ADC
//            (as in kOrigin, 0 = none), w5 UIO arming delay after w1.
//            UIO: waits for UIO input bit 0 to rise, then sendl(1); every
//            other channel: receivel, then origin. The new epoch starts at
//            the release, so later rows count from there: the halt time
//            maps to triggerLeadClk into the new epoch.
// Time words are clocks since the channel's last origin() (or its start); the
// host keeps them below 2^31 (signed realtime literals and variables).
// pointsMinus1 because rf()/gradient() with an address play N + 1 points
// (FINDINGS S4).
#pragma once

#include "model/sequence_model.hpp"
#include "shapes/waveform_lib.hpp"
#include "units/units.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pp::tables {

enum class RowKind : std::uint32_t {
  kEnd = 0, kRf = 1, kGrad = 2, kAdc = 3, kOrigin = 4, kTrigger = 5
};
// Absolute clock of a time word: base + value, where base starts at 0 and
// every kOrigin row adds its at + ScanConfig::originLatencyClk.

struct Row {
  static constexpr int kWords = 8;
  static constexpr std::uint32_t kBytes = kWords * 4;
  std::array<std::uint32_t, kWords> w{};

  RowKind kind() const { return static_cast<RowKind>(w[0]); }
  std::uint32_t at() const { return w[1]; }
};

struct ScanConfig {
  double larmorHz = 0;                  // -f: NCO frequency (Tx and Rx), the
                                        // offset from the PTS base frequency
  // PTS sideband (units::PtsSetting::sign): +1 physical = base + NCO, -1
  // physical = base - NCO (13C etc.). With -1 every frequency offset, phase
  // offset and RF shape phase is negated, so the physical result is as written
  // in the .seq; the received data are conjugated (RunOptions).
  int sideband = +1;
  double rfMaxHz = 0;                   // -rfmax: B1 (Hz) at full-scale RF
  std::array<double, 3> gradMaxHzPerM{0, 0, 0};  // -gx/-gy/-gz at full scale
  bool cicCompensation = false;         // -cic on: FIR decimates by 4
  model::Clk startOffset = 10000;       // sequence t=0 on the RT clock (100 us
                                        // lead-in for NCO reset etc.)
  // Minimum time from a row's last timed action to the next row's first on
  // the same channel: the block loop must return from the previous call,
  // seek, read and dispatch in between. Measured with p10_gap_stress
  // (FINDINGS T11): Tx on time down to 90, gradients 50, Rx 150 (for the CIC
  // grid reset on `at`; sample times hold down to 50); +10 margin. Closer rows
  // are reported in `timingIssues`.
  model::Clk txSlackClk = 100;
  model::Clk rxSlackClk = 160;
  model::Clk gradSlackClk = 60;
  // Rx rows set the NCO frequency this long before `at`: setfreq -> setph ->
  // acquire2 takes 51 clocks on the simulator (FINDINGS R5), so acquire2 then
  // resets the CIC grid on `at` and grid boundaries fall on sample times.
  model::Clk rxFreqLeadClk = 51;
  // An ADC frequency offset is undone this long after the last sample latch.
  model::Clk rxRestoreHoldClk = 50;
  // The RF gate opens at the RF block start, but at least rfGateLeadClk before
  // the first sample, and closes rfGateHoldClk after the trailing zero point:
  // blank() cannot share a clock with the end of rf(), which returns ~28
  // clocks after its last point (FINDINGS X2).
  model::Clk rfGateLeadClk = 40;
  model::Clk rfGateHoldClk = 50;
  // Tx rows set the pulse phase (setph) this long before gateOn.
  model::Clk txPhaseLeadClk = 50;
  // RF up to this fraction above -rfmax is clipped at full scale (warning),
  // like the vendor; more is refused.
  double rfClipTolerance = 0.01;
  // Timeline rebasing: a channel inserts origin() once a row would reach more
  // than epochLimitClk past the current origin (2^30 clocks = 10.7 s, half
  // the signed 32-bit range). origin(at) takes effect at at + latency (p09).
  model::Clk epochLimitClk = model::Clk{1} << 30;
  model::Clk originLatencyClk = 10;
  // Respiratory triggering: before every excitation block (RF use 'e') all
  // channels wait for UIO input bit 0 to rise (the monitor sets the delay and
  // the width itself). The Pulseq format is not involved.
  bool respiratoryTrigger = false;
  // The block start of the excitation block plays this long after the new
  // epoch starts, so the first event keeps every channel's row slack (the Tx
  // row starts up to 90 clocks before the block start).
  model::Clk triggerLeadClk = 1000;
  // origin(now() + settle) after the release, so it is still in the future.
  model::Clk triggerSettleClk = 200;
  // UIO starts looking for the gate this long after the halt time, so that
  // every channel is already blocked in receivel (an early message may be
  // dropped, p12) and a gate during the previous repetition is ignored.
  model::Clk triggerArmClk = 5000;
  // PLATO delivers a message to the other channels this long after it is sent
  // (p11: 52.41 us on the scanner, 0.11 us on the simulator). UIO delays its
  // origin by it so that its epoch starts with the others'.
  model::Clk triggerMsgLatencyClk = 5300;
  units::Clocks clocks;
};

struct ChannelTables {
  std::vector<Row> tx;
  std::vector<Row> rx;
  std::array<std::vector<Row>, 3> grad;  // logical x, y, z -> GA, GB, GC
  // Uncommitted I/O: END (and ORIGIN) rows, plus the kTrigger rows. It holds
  // the PTS output bits (set at init) until the end of the sequence; outputs
  // return to 0 when a channel exits.
  std::vector<Row> uio;
  shapes::RfLibrary rfLib;
  shapes::GradLibrary gradLib;
  std::uint32_t baseFreqWord = 0;   // setfreq(larmor) on Tx and Rx at start
  model::Clk end = 0;               // absolute clock of the last event
  int origins = 0;                  // ORIGIN rows over all channels
  int triggers = 0;                 // gates (kTrigger rows per channel)
  std::vector<std::string> warnings;
  std::vector<std::string> timingIssues;
};

class TableError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Throws TableError (e.g. amplitude above full scale, or no gap to place an
// origin() in).
ChannelTables BuildTables(const model::SequenceModel& m, const ScanConfig& cfg);

// The raw word stream for one table (what Phase 2 puts into fmt::Raw).
std::vector<std::uint32_t> Flatten(const std::vector<Row>& rows);

// Epoch base before each row: absolute clock of a time word = base + value.
std::vector<long long> EpochBases(const std::vector<Row>& rows,
                                  const ScanConfig& cfg);

}  // namespace pp::tables
