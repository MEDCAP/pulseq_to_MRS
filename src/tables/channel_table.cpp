#include "tables/channel_table.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <sstream>

namespace pp::tables {

namespace {

using model::Clk;
constexpr double kTwoPi = 2 * std::numbers::pi;

// A row while it is built: time words hold absolute clocks (int64) until
// Rebase() turns them into clocks since the channel's current origin().
struct TimedRow {
  Row row;
  std::array<Clk, Row::kWords> abs{};  // absolute clock of each time word
  unsigned timeMask = 0;               // bit k: word k is a time
  Clk first = 0;  // earliest timed action of the row (incl. leads)
  Clk last = 0;   // latest timed action of the row
  Clk haltAt = 0; // kTrigger: absolute nominal halt time (excitation block start)
  int block = -1;

  void SetTime(int k, Clk t) {
    abs[k] = t;
    timeMask |= 1u << k;
  }
};

// Splits a channel's timeline into epochs that fit the signed 32-bit `at`
// arguments, inserting ORIGIN rows (origin(at) resets the channel timer).
// origin(T) takes effect at T + originLatencyClk (FINDINGS T10), so the next
// epoch's base is T + latency. An ORIGIN is placed as late as possible in the
// gap before the row that would overflow, keeping the channel's row slack on
// both sides; long idle stretches get several.
std::vector<Row> Rebase(const char* channel, std::vector<TimedRow>& rows,
                        Clk slack, const ScanConfig& cfg, int& origins) {
  constexpr Clk kMax = std::numeric_limits<std::int32_t>::max();
  std::vector<Row> out;
  out.reserve(rows.size());
  Clk base = 0;
  Clk prevLast = -1;
  for (std::size_t ri = 0; ri < rows.size(); ++ri) {
    auto& r = rows[ri];
    if (r.row.w[0] == static_cast<std::uint32_t>(RowKind::kTrigger)) {
      // A gate is an epoch boundary: the channel restarts its timer when it is
      // released, and the halt time maps to triggerLeadClk into the new epoch.
      const Clk rel = r.haltAt - base;
      if (rel < 0 || rel > kMax) {
        std::ostringstream os;
        os << channel << " block " << r.block + 1 << ": gate at clock "
           << r.haltAt << " is outside the current epoch (base " << base << ")";
        throw TableError(os.str());
      }
      r.row.w[1] = static_cast<std::uint32_t>(rel);
      const Clk newBase = r.haltAt - cfg.triggerLeadClk;
      // Rx: origin() stops the CIC grid, so the next ADC needs its grid set
      // up again (w3/w4, like the ORIGIN row), unless it is so far away that
      // an epoch-limit ORIGIN does it.
      if (ri + 1 < rows.size() &&
          rows[ri + 1].row.w[0] == static_cast<std::uint32_t>(RowKind::kAdc) &&
          rows[ri + 1].last - newBase <= cfg.epochLimitClk) {
        const auto& a = rows[ri + 1];
        const Clk grid = static_cast<Clk>(a.row.w[7]);
        const Clk tg = a.abs[1] - 7 * grid - newBase;
        if (tg < cfg.triggerSettleClk + 200) {
          std::ostringstream os;
          os << channel << " block " << a.block + 1
             << ": the first ADC after the gate is too close to it (no room "
                "to set up the CIC grid 7 ticks before it)";
          throw TableError(os.str());
        }
        r.row.w[3] = static_cast<std::uint32_t>(grid);
        r.row.w[4] = static_cast<std::uint32_t>(tg);
      }
      out.push_back(r.row);
      base = newBase;
      prevLast = newBase + cfg.triggerSettleClk;
      continue;
    }
    const bool adc = r.row.w[0] == static_cast<std::uint32_t>(RowKind::kAdc);
    // r.row.w[7] is its grid word
    const Clk grid = adc ? static_cast<Clk>(r.row.w[7]) : 0;
    // setgrid time 7 ticks before adc reset at r.abs[1]
    const Clk tg = adc ? r.abs[1] - 7 * grid : 0; 
    while (r.last - base > cfg.epochLimitClk) {
      const Clk lo = prevLast < 0 ? cfg.startOffset
                                  : prevLast + slack;
      Clk hi = r.first - cfg.originLatencyClk - slack;
      if (adc) hi = std::min(hi, tg - cfg.originLatencyClk - slack);  // origin before setgrid
      const Clk at = std::min(hi, base + cfg.epochLimitClk);
      if (at < lo || at < base) {
        std::ostringstream os;
        os << channel << " block " << r.block + 1
           << ": no room for origin() before the event at clock " << r.first
           << " (the channel is busy from " << lo << "; a row must fit in "
           << cfg.epochLimitClk << " clocks)";
        throw TableError(os.str());
      }
      Row o;
      o.w[0] = static_cast<std::uint32_t>(RowKind::kOrigin);
      o.w[1] = static_cast<std::uint32_t>(at - base);
      out.push_back(o);
      ++origins;
      base = at + cfg.originLatencyClk;
      prevLast = at;
      // last oroigin before this ADC
      if (adc && r.last - base <= cfg.epochLimitClk) {
        out.back().w[2] = static_cast<std::uint32_t>(grid);
        out.back().w[3] = static_cast<std::uint32_t>(tg - base);
      }
    }
    for (int k = 0; k < Row::kWords; ++k)
      if (r.timeMask & (1u << k)) {
        const Clk v = r.abs[k] - base;
        if (v < 0 || v > kMax) {
          std::ostringstream os;
          os << channel << " block " << r.block + 1 << ": time " << r.abs[k]
             << " is outside the current epoch (base " << base << ")";
          throw TableError(os.str());
        }
        r.row.w[k] = static_cast<std::uint32_t>(v);
      }
    out.push_back(r.row);
    prevLast = r.last;
  }
  return out;
}

// Records a timing issue when `at` (the row's first timed call) leaves the
// loop less than `slack` after the previous row's last timed action.
void CheckSlack(const char* channel, int block, Clk prevLast, Clk at,
                Clk slack, ChannelTables& t) {
  if (prevLast < 0) return;
  if (at - prevLast < slack) {
    std::ostringstream os;
    os << channel << " block " << block + 1 << ": event at clock " << at
       << " starts " << at - prevLast << " clocks after the previous event's "
       << "last action (the block loop needs >= " << slack << ")";
    t.timingIssues.push_back(os.str());
  }
}

void BuildTx(const model::SequenceModel& m, const ScanConfig& cfg,
             ChannelTables& t, std::vector<TimedRow>& out) {
  double accPhase = 0;  // extra NCO phase from frequency offsets so far (rad)
  int clipped = 0;
  double worstOver = 0;
  Clk prevLast = -1;
  for (const auto& rf : m.rf) {
    if (!(cfg.rfMaxHz > 0)) throw TableError("RF events present but rfmax <= 0");
    // The vendor clips RF above -rfmax at full scale without a message
    // (FINDINGS V11). We clip up to rfClipTolerance over full scale with a
    // warning, and refuse more, which would change the flip angle.
    double peak = 0;
    for (float mag : rf.magnitude)
      peak = std::max(peak, std::abs(rf.amplitudeHz * mag));
    double rfFull = cfg.rfMaxHz;
    if (peak > cfg.rfMaxHz * (1 + 1e-6)) {
      if (peak > cfg.rfMaxHz * (1 + cfg.rfClipTolerance)) {
        std::ostringstream os;
        os << "RF block " << rf.block + 1 << ": peak " << peak
           << " Hz exceeds -rfmax " << cfg.rfMaxHz << " Hz by "
           << 100 * (peak / cfg.rfMaxHz - 1) << " % (clipping is allowed up to "
           << 100 * cfg.rfClipTolerance << " %); use -rfmax >= " << peak;
        throw TableError(os.str());
      }
      ++clipped;
      worstOver = std::max(worstOver, peak / cfg.rfMaxHz - 1);
      rfFull = peak;  // Fraction() accepts it; the value is clamped below
    }
    const Clk r = rf.rasterClk;
    if (r % 2 != 0)
      throw TableError("RF raster must be an even number of clocks");
    const auto n = static_cast<Clk>(rf.magnitude.size());
    const Clk at = cfg.startOffset + rf.start + r / 2;  // sample centres
    const double freq = units::ModulationFrequency(rf.freqOffsetHz, cfg.clocks);

    // rf() applies freq as setfreqofs, so the NCO integrates the offset while
    // the pulse plays; subtracting the phase accumulated by earlier pulses
    // keeps every pulse referenced to the continuous carrier at the base
    // frequency, with the offset's phase ramp starting at the first sample.
    // That per-pulse phase goes into the row (setph, NCO absolute phase) and
    // the waveform keeps only the shape phase, so pulses that differ only in
    // phase (RF spoiling) share one library waveform.
    const std::uint32_t phaseWord =
        units::PhaseWord16(rf.phaseOffsetRad - accPhase);
    std::vector<shapes::ModPoint> pts;
    pts.reserve(rf.magnitude.size() + 1);
    for (std::size_t k = 0; k < rf.magnitude.size(); ++k) {
      pts.push_back(
          {std::clamp(units::Fraction(rf.amplitudeHz * rf.magnitude[k], rfFull,
                                      "RF amplitude") * rfFull / cfg.rfMaxHz,
                      -1.0, 1.0),
           units::ModulationPhase(rf.phaseRad[k]),
           freq});
    }
    pts.push_back({0, 0, 0});  // outputs hold their last value (FINDINGS S6)
    const auto ref = t.rfLib.Add(pts);

    // Gate: open at the start of the RF block, so the RF delay (Pulseq's
    // rfDeadTime) is the amplifier unblank time, as the vendor does and as
    // imaged on the scanner (FINDINGS V1, V8). Close it after the trailing zero.
    // rf() does not gate by itself (p06 logs no GATE 1).
    const Clk blockStart =
        rf.block >= 0 && rf.block < static_cast<int>(m.blocks.size())
            ? m.blocks[rf.block].start
            : rf.start;
    const Clk gateOn =
        std::min(cfg.startOffset + blockStart, at - cfg.rfGateLeadClk);
    const Clk gateOff = at + n * r + cfg.rfGateHoldClk;
    CheckSlack("Tx", rf.block, prevLast, gateOn - cfg.txPhaseLeadClk,
               cfg.txSlackClk, t);
    TimedRow row;
    row.row.w = {static_cast<std::uint32_t>(RowKind::kRf), 0, ref.byteOffset,
                 ref.points - 1, static_cast<std::uint32_t>(r), 0, 0,
                 phaseWord};
    row.SetTime(1, at);
    row.SetTime(5, gateOn);
    row.SetTime(6, gateOff);
    row.first = gateOn - cfg.txPhaseLeadClk;
    row.last = gateOff;
    row.block = rf.block;
    out.push_back(row);
    prevLast = gateOff;
    accPhase += kTwoPi * rf.freqOffsetHz * static_cast<double>(n * r) /
                cfg.clocks.rtClockHz;
  }
  if (clipped) {
    std::ostringstream os;
    os << clipped << " RF pulse(s) exceed -rfmax " << cfg.rfMaxHz
       << " Hz by up to " << 100 * worstOver
       << " % and are clipped at full scale (as the vendor pulseq.exe does)";
    t.warnings.push_back(os.str());
  }
}

void BuildGrad(const model::SequenceModel& m, const ScanConfig& cfg,
               ChannelTables& t, std::array<std::vector<TimedRow>, 3>& out,
               const std::vector<int>& haltBlocks) {
  static const char* kNames[3] = {"GA(x)", "GB(y)", "GC(z)"};
  for (int axis = 0; axis < 3; ++axis) {
    struct Pending {
      int block;
      Clk at;
      Clk raster;
      std::vector<double> v;  // fractions of full scale
    };
    std::vector<Pending> list;
    for (const auto& g : m.grad) {
      if (g.axis != axis) continue;
      if (!(cfg.gradMaxHzPerM[axis] > 0))
        throw TableError(std::string(kNames[axis]) +
                         ": gradient events present but gmax <= 0");
      Pending p{g.block, cfg.startOffset + g.start, g.rasterClk, {}};
      for (double hz : g.samplesHzPerM)
        p.v.push_back(
            units::Fraction(hz, cfg.gradMaxHzPerM[axis], "gradient amplitude"));
      list.push_back(std::move(p));
    }

    // Join waveforms that meet on the same raster edge: the joint value is
    // played once, by the later waveform (else two events share one clock).
    // A waveform that ends non-zero without a continuation gets a zero point.
    for (std::size_t i = 0; i < list.size(); ++i) {
      auto& p = list[i];
      const Clk last = p.at + p.raster * static_cast<Clk>(p.v.size() - 1);
      // Never join across a gate: the joint point is played by the later
      // waveform, so the earlier one would end on its last non-zero sample and
      // the gradient would hold it for as long as the channel waits.
      bool gated = false;
      if (i + 1 < list.size())
        for (int hb : haltBlocks)
          gated |= p.block < hb && hb <= list[i + 1].block;
      const bool joined = i + 1 < list.size() && list[i + 1].at == last && !gated;
      if (joined) {
        if (std::abs(p.v.back() - list[i + 1].v.front()) > 1e-6) {
          std::ostringstream os;
          os << kNames[axis] << " block " << p.block + 1
             << ": gradient jumps at block boundary (" << p.v.back() << " -> "
             << list[i + 1].v.front() << " of full scale)";
          t.warnings.push_back(os.str());
        }
        if (p.v.size() > 1) p.v.pop_back();
      } else if (std::abs(p.v.back()) > 1e-9) {
        std::ostringstream os;
        os << kNames[axis] << " block " << p.block + 1
           << ": gradient ends at " << p.v.back()
           << " of full scale with no continuation; ramping to 0 in one raster";
        t.warnings.push_back(os.str());
        p.v.push_back(0.0);
      }
    }

    Clk prevLast = -1;
    for (const auto& p : list) {
      const auto ref = t.gradLib.Add(p.v);
      CheckSlack(kNames[axis], p.block, prevLast, p.at, cfg.gradSlackClk, t);
      TimedRow row;
      row.row.w = {static_cast<std::uint32_t>(RowKind::kGrad), 0,
                   ref.byteOffset, ref.points - 1,
                   static_cast<std::uint32_t>(p.raster)};
      row.SetTime(1, p.at);
      row.first = p.at;
      row.last = p.at + p.raster * static_cast<Clk>(p.v.size() - 1);
      row.block = p.block;
      out[axis].push_back(row);
      prevLast = row.last;
    }
  }
}

void BuildRx(const model::SequenceModel& m, const ScanConfig& cfg,
             ChannelTables& t, std::vector<TimedRow>& out) {
  double accPhase = 0;
  Clk prevLast = -1;
  for (const auto& a : m.adc) {
    const Clk d = a.dwellClk;
    Clk grid = d;
    if (cfg.cicCompensation) {
      if (d % 4 != 0)
        throw TableError("ADC dwell must be a multiple of 4 clocks when the "
                         "CIC compensation FIR is on");
      grid = d / 4;
    }
    // A Pulseq ADC sample k stands for the centre of its dwell step
    // [start + k d, start + (k+1) d]. With grid = dwell the CIC integrates
    // exactly that step and the sample is latched at its end. acquire2 latches
    // its first sample at `at + dwell` (FINDINGS R1) and resets the grid on
    // `at` (R5), so at = start.
    const Clk at = cfg.startOffset + a.start;
    const Clk freqAt = at - cfg.rxFreqLeadClk;
    const Clk lastSample = at + d * a.numSamples;
    // setfreq(base + offset) at freqAt, back to base at restoreAt: the NCO
    // integrates the offset in between. This ADC's phase word references only
    // what earlier ADCs accumulated; its own share is added after the row.
    // The restore cannot share a clock with the last latch (acquire2 returns
    // after it), so it follows rxRestoreHoldClk later.
    const Clk restoreAt =
        a.freqOffsetHz != 0 ? lastSample + cfg.rxRestoreHoldClk : 0;
    const std::uint32_t phaseWord =
        units::PhaseWord16(a.phaseOffsetRad - accPhase);
    if (restoreAt != 0)
      accPhase += kTwoPi * a.freqOffsetHz *
                  static_cast<double>(restoreAt - freqAt) /
                  cfg.clocks.rtClockHz;
    CheckSlack("Rx", a.block, prevLast, freqAt, cfg.rxSlackClk, t);
    TimedRow row;
    row.row.w = {static_cast<std::uint32_t>(RowKind::kAdc), 0,
                 static_cast<std::uint32_t>(a.numSamples),
                 static_cast<std::uint32_t>(d),
                 units::FrequencyWord(cfg.larmorHz + a.freqOffsetHz, cfg.clocks),
                 phaseWord, 0, static_cast<std::uint32_t>(grid)};
    row.SetTime(1, at);
    if (restoreAt != 0) row.SetTime(6, restoreAt);  // 0 = no restore
    row.first = freqAt;
    row.last = std::max(lastSample, restoreAt);
    row.block = a.block;
    out.push_back(row);
    prevLast = row.last;
  }
}

// Puts one kTrigger row per halt block into a channel's rows, ahead of the
// first row of that block (rows are in block order; the END row belongs to the
// last block, so a gate in the last block still precedes it).
void InsertTriggers(std::vector<TimedRow>& rows, const model::SequenceModel& m,
                    const ScanConfig& cfg, const std::vector<int>& haltBlocks,
                    Clk originDelay, Clk arm) {
  std::size_t pos = 0;
  for (int b : haltBlocks) {
    while (pos < rows.size() && rows[pos].block < b) ++pos;
    const Clk at = cfg.startOffset + m.blocks[b].start;
    TimedRow tr;
    tr.row.w = {static_cast<std::uint32_t>(RowKind::kTrigger), 0,
                static_cast<std::uint32_t>(originDelay), 0, 0,
                static_cast<std::uint32_t>(arm), 0, 0};
    tr.SetTime(1, at);
    tr.haltAt = at;
    tr.first = tr.last = at;
    tr.block = b;
    rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(pos), tr);
    ++pos;
  }
}

}  // namespace

ChannelTables BuildTables(const model::SequenceModel& m, const ScanConfig& cfg) {
  ChannelTables t;
  t.baseFreqWord = units::FrequencyWord(cfg.larmorHz, cfg.clocks);
  // Gates: the start of every block with an excitation pulse (Pulseq RF use
  // 'e'); no change to the Pulseq format is needed.
  std::vector<int> haltBlocks;
  if (cfg.respiratoryTrigger) {
    for (const auto& b : m.blocks)
      if (b.rf >= 0 && m.rf[b.rf].use == 'e') haltBlocks.push_back(b.index);
    if (haltBlocks.empty())
      t.warnings.push_back(
          "respiratory triggering is on but the sequence has no excitation "
          "pulse (RF use 'e'): nothing is gated");
  }
  t.triggers = static_cast<int>(haltBlocks.size());
  std::vector<TimedRow> tx, rx;
  std::array<std::vector<TimedRow>, 3> grad;
  BuildTx(m, cfg, t, tx);
  BuildGrad(m, cfg, t, grad, haltBlocks);
  BuildRx(m, cfg, t, rx);

  t.end = cfg.startOffset + m.duration;
  TimedRow end;
  end.row.w[0] = static_cast<std::uint32_t>(RowKind::kEnd);
  end.SetTime(1, t.end);
  end.first = end.last = t.end;
  end.block = static_cast<int>(m.blocks.size()) - 1;
  tx.push_back(end);
  rx.push_back(end);
  for (auto& g : grad) g.push_back(end);
  std::vector<TimedRow> uio{end};

  // Every channel gets every gate, in the same order. UIO also waits for the
  // message to reach the others before it restarts its own timer.
  if (!haltBlocks.empty()) {
    for (auto* rows : {&tx, &rx, &grad[0], &grad[1], &grad[2]})
      InsertTriggers(*rows, m, cfg, haltBlocks, cfg.triggerSettleClk, 0);
    InsertTriggers(uio, m, cfg, haltBlocks,
                   cfg.triggerSettleClk + cfg.triggerMsgLatencyClk,
                   cfg.triggerArmClk);
  }

  t.tx = Rebase("Tx", tx, cfg.txSlackClk, cfg, t.origins);
  t.rx = Rebase("Rx", rx, cfg.rxSlackClk, cfg, t.origins);
  static const char* kNames[3] = {"GA(x)", "GB(y)", "GC(z)"};
  for (int a = 0; a < 3; ++a)
    t.grad[a] = Rebase(kNames[a], grad[a], cfg.gradSlackClk, cfg, t.origins);
  int uioOrigins = 0;  // UIO is optional: not counted in t.origins
  t.uio = Rebase("UIO", uio, cfg.gradSlackClk, cfg, uioOrigins);
  return t;
}

std::vector<std::uint32_t> Flatten(const std::vector<Row>& rows) {
  std::vector<std::uint32_t> out;
  out.reserve(rows.size() * Row::kWords);
  for (const auto& r : rows) out.insert(out.end(), r.w.begin(), r.w.end());
  return out;
}

std::vector<long long> EpochBases(const std::vector<Row>& rows,
                                  const ScanConfig& cfg) {
  std::vector<long long> b(rows.size());
  long long base = 0;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    b[i] = base;
    if (rows[i].kind() == RowKind::kOrigin)
      base += rows[i].at() + cfg.originLatencyClk;
    else if (rows[i].kind() == RowKind::kTrigger)
      base += rows[i].at() - cfg.triggerLeadClk;  // nominal; the real start
                                                  // depends on the gate
  }
  return b;
}

}  // namespace pp::tables
