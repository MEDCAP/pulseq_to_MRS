#include "realtime/program.hpp"

#include <ppl_language.hpp>
#include <ppl_library.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>

namespace pp::realtime {

namespace {

using namespace mrs::plato;
using namespace mrs::plato::ppl;
using tables::Row;
using tables::RowKind;

// Realtime literals are signed 32-bit (plan: "cast frequency words").
std::int32_t I32(std::uint32_t v) { return static_cast<std::int32_t>(v); }

constexpr std::int32_t Kind(RowKind k) { return static_cast<std::int32_t>(k); }

// The respiratory monitor (SA Instruments 1030) sets UIO input bit 0 high.
constexpr std::int32_t kGateMask = 1;

// Rows of an unrolled program each cost tens of bytes of the 8 KiB instruction
// memory (FINDINGS T6); refuse early instead of failing in the compiler.
constexpr std::size_t kMaxUnrolledRows = 64;

// Words 2.. of a row that each kind uses (word 1, `at`, always).
int WordsUsed(RowKind k) {
  switch (k) {
    case RowKind::kRf: return 6;    // off, pointsMinus1, period, gateOn,
                                    // gateOff, phase
    case RowKind::kGrad: return 3;  // off, pointsMinus1, period
    case RowKind::kAdc: return 6;   // n, dwell, freq, phase, restoreAt, grid
    case RowKind::kEnd: return 0;
    case RowKind::kOrigin: return 2;// w2 grid (0=none), w3 setgrid time
    case RowKind::kTrigger: return 4;  // w2 origin delay, w3 grid (0=none),
                                       // w4 setgrid time, w5 UIO arming delay
  }
  return 0;
}

// The data one channel program plays from.
struct ChannelData {
  const std::vector<Row>* rows = nullptr;
  // Waveform library of this channel type; nullopt if the channel has no
  // waveform rows (an empty stream must not be referenced).
  std::optional<_stream<fmt::Modulation>> rfLib;
  std::optional<_stream<fmt::Gradient>> gradLib;
};

// Realtime variables holding the current row: at = word 1, w[k] = word k.
// Variables are per component: each channel program makes its own.
struct RowVars {
  _variable at;
  _variable w[8];  // w[2]..w[7] used
  _variable gate;  // UIO: the input value while waiting for a gate
};

// ---- per-row calls, shared by both emitters ----------------------------------
// Both emitters run exactly this code on the row variables and differ only in
// how the variables are filled (literals vs stream reads), which completes
// before the row's first timed call. Instruction timing after that point is
// therefore the same, and so are the simulator logs (Phase 2 requirement).

std::vector<StatementBuilder> RowCalls(RowKind kind, RowVars& v,
                                       const tables::ChannelTables& t,
                                       const ChannelData& d,
                                       const ProgramOptions& o) {
  std::vector<StatementBuilder> s;
  switch (kind) {
    case RowKind::kRf:
      // Each call exact: setph on its own clock before the gate (sharing
      // unblank's clock made rf() 26 clocks late, FINDINGS X3; an untimed
      // setph would land wherever the row's reads finished, which differs
      // between the emitters). Pabs only matters once rf() plays.
      s.push_back(setph(v.w[7], v.w[5] - static_cast<std::int32_t>(
                                            o.txPhaseLeadClk)));
      s.push_back(unblank(v.w[5]));
      s.push_back(rf(GetStreamAddress(d.rfLib->get()) + v.w[2], v.w[3], v.w[4],
                     v.at));
      s.push_back(blank(v.w[6]));
      break;
    case RowKind::kGrad:
      s.push_back(gradient(GetStreamAddress(d.gradLib->get()) + v.w[2], v.w[3],
                           v.w[4], v.at));
      break;
    case RowKind::kAdc: {
      // w2 n, w3 dwell, w4 freq word, w5 phase word, w6 restoreAt, w7 grid.
      // setfreq is exact; setph and acquire2 follow back to back, and the lead
      // makes acquire2 execute (reset the CIC grid) on `at` (FINDINGS R5).
      const auto lead = static_cast<std::int32_t>(o.rxFreqLeadClk);
      s.push_back(setfreq(v.w[4], v.at - lead));
      s.push_back(setph(v.w[5], v.at - lead));
      s.push_back(acquire2(v.w[2], v.w[3], v.w[7], v.at));
      s.push_back(_if(v.w[6] != 0)(setfreq(I32(t.baseFreqWord), v.w[6])));
      break;
    }
    case RowKind::kOrigin:
      // Restart the channel timer; later rows count from at + 10 (p09).
      s.push_back(origin(v.at));
      // Rx: origin() stops the CIC grid; restablish it lattice >=7 ticks before
      // the next ADC so that comb stages are flushed (GRE-18). w3 counts from the new epoch
      if (d.rows == &t.rx)  // if adc
        s.push_back(_if(v.w[2] != 0)(setgrid(v.w[2], v.w[3]))); // setgrid(period, at)
      break;
    case RowKind::kTrigger:
      // Respiratory gate. UIO arms a little after the halt time (every other
      // channel is blocked in receivel by then), waits for input bit 0 to fall
      // and then rise (a rising edge, so one monitor pulse releases only one
      // repetition) and broadcasts the release. The others wake on the message,
      // which PLATO activates at the same clock on every channel. Everyone then
      // restarts its timer a short delay later; later rows count from there.
      if (d.rows == &t.uio) {
        s.push_back(resume(v.at + v.w[5]));
        s.push_back(v.gate = input(kGateMask));
        s.push_back(_while(v.gate != 0)(v.gate = input(kGateMask)));
        s.push_back(_while(v.gate == 0)(v.gate = input(kGateMask)));
        s.push_back(sendl(1));
      } else {
        s.push_back(receivel(0xFFFFFFFF));
      }
      s.push_back(origin(now() + v.w[2]));
      // Rx: origin() stops the CIC grid; set it up again for the next ADC.
      if (d.rows == &t.rx)
        s.push_back(_if(v.w[3] != 0)(setgrid(v.w[3], v.w[4])));
      break;
    case RowKind::kEnd:
      // Every channel exits at the end of the sequence.
      s.push_back(resume(v.at));
      s.push_back(ppl::exit(0));
      break;
  }
  return s;
}

// ---- channel initialisation -----------------------------------------------

std::vector<StatementBuilder> Init(Role role, const tables::ChannelTables& t,
                                   const ChannelData& d,
                                   const ProgramOptions& o) {
  std::vector<StatementBuilder> s;
  switch (role) {
    case Role::kTx:
      s.push_back(setscale(32767));  // rf() amplitudes are relative to it (X1)
      [[fallthrough]];
    case Role::kRx:
      // Same frequency and a phase reset at the same clock on Tx and Rx, so
      // both NCOs run coherently from before the first event.
      s.push_back(setfreq(I32(t.baseFreqWord)));
      s.push_back(setph(0));
      s.push_back(resetosc(0, static_cast<std::int32_t>(o.ncoResetAt)));
      break;
    case Role::kUio:
      // PTS synthesizer output bits; the channel holds them until it exits.
      // (The UIO also exists for respiratory gates without a PTS mask.)
      if (o.uioOutWord != 0)
        s.push_back(setout(static_cast<std::int32_t>(o.uioOutWord),
                           static_cast<std::int32_t>(o.uioAt)));
      break;
    default:
      s.push_back(setscale(32767));
      break;
  }
  // Rx: no CIC grid is established before the first acquire2, so its samples
  // 1-6 would be stale CIC words (FINDINGS H7). Establish the grid on the
  // first ADC's lattice 7 ticks before it, as after origin() (GRE-18/19). If
  // the first row is an ORIGIN, that row's setgrid does it instead. Skipped
  // when the grid would start before resetosc or leave the loop less than the
  // Rx row slack (160 clocks, ScanConfig::rxSlackClk) before the ADC's setfreq.
  if (role == Role::kRx && !d.rows->empty() &&
      d.rows->front().kind() == RowKind::kAdc) {
        const Row& first = d.rows->front();
        const auto grid = static_cast<long long>(first.w[7]);
        const auto at = static_cast<long long>(first.at());
        const long long tg = at - 7 * grid;
        const long long freqAt = at - static_cast<long long>(o.rxFreqLeadClk);
        if (grid > 0 && tg > static_cast<long long>(o.ncoResetAt) + 100 &&
            freqAt - tg >= 160)
          s.push_back(setgrid(static_cast<std::int32_t>(grid),
                              static_cast<std::int32_t>(tg)));
      } 

  // Streams that are never seek'd are not loaded (plato docs 4.6), and
  // GetStreamAddress alone is not documented to count: touch the library.
  if (d.rfLib) s.push_back(seek(*d.rfLib));
  if (d.gradLib) s.push_back(seek(*d.gradLib));
  return s;
}

// ---- unrolled emitter ------------------------------------------------------

std::vector<StatementBuilder> Unrolled(Role role, const tables::ChannelTables& t,
                                       const ChannelData& d,
                                       const ProgramOptions& o) {
  if (d.rows->size() > kMaxUnrolledRows)
    throw std::runtime_error(
        "unrolled emitter: " + std::string(RoleName(role)) + " has " +
        std::to_string(d.rows->size()) + " rows (limit " +
        std::to_string(kMaxUnrolledRows) + "); use the block loop");
  auto s = Init(role, t, d, o);
  RowVars v;
  for (const Row& r : *d.rows) {
    s.push_back(v.at = I32(r.at()));
    for (int k = 2; k < 2 + WordsUsed(r.kind()); ++k)
      s.push_back(v.w[k] = I32(r.w[k]));
    for (auto& x : RowCalls(r.kind(), v, t, d, o)) s.push_back(std::move(x));
  }
  return s;
}

// ---- block-loop emitter ----------------------------------------------------

std::vector<StatementBuilder> BlockLoop(Role role,
                                        const tables::ChannelTables& t,
                                        const ChannelData& d,
                                        _stream<fmt::Raw>& table,
                                        const ProgramOptions& o) {
  auto s = Init(role, t, d, o);
  RowVars v;
  _variable i, pos, kind;

  // One _case per row kind in this channel's table (the END row, always last,
  // exits). Reads happen inside the case: the stream pointer walks the row in
  // order, and rf()/gradient() moving it (FINDINGS S5) is undone by the next
  // row's seek.
  bool present[6] = {};
  for (const Row& r : *d.rows) present[static_cast<int>(r.kind())] = true;
  auto makeCase = [&](RowKind k) {
    std::vector<StatementBuilder> c;
    for (int w = 2; w < 2 + WordsUsed(k); ++w) c.push_back(v.w[w] = read());
    for (auto& x : RowCalls(k, v, t, d, o)) c.push_back(std::move(x));
    return _case(Kind(k))(_compound(c));
  };
  const RowKind playKinds[] = {RowKind::kRf, RowKind::kGrad, RowKind::kAdc};
  std::optional<RowKind> play;
  for (RowKind k : playKinds)
    if (present[static_cast<int>(k)]) {
      if (play)
        throw std::runtime_error(std::string(RoleName(role)) +
                                 ": table mixes row kinds");
      play = k;
    }
  // _switch is an if-chain (plato docs 6.1.17): most frequent kind first.
  const bool hasOrigin = present[static_cast<int>(RowKind::kOrigin)];
  const bool hasTrigger = present[static_cast<int>(RowKind::kTrigger)];
  StatementBuilder dispatch = [&]() -> StatementBuilder {
    const auto end = [&] { return makeCase(RowKind::kEnd); };
    const auto org = [&] { return makeCase(RowKind::kOrigin); };
    const auto trg = [&] { return makeCase(RowKind::kTrigger); };
    if (play && hasOrigin && hasTrigger)
      return _switch(kind)(makeCase(*play), org(), trg(), end());
    if (play && hasOrigin) return _switch(kind)(makeCase(*play), org(), end());
    if (play && hasTrigger) return _switch(kind)(makeCase(*play), trg(), end());
    if (play) return _switch(kind)(makeCase(*play), end());
    if (hasOrigin && hasTrigger) return _switch(kind)(org(), trg(), end());
    if (hasOrigin) return _switch(kind)(org(), end());
    if (hasTrigger) return _switch(kind)(trg(), end());
    return _switch(kind)(end());
  }();

  const auto rows = static_cast<std::int32_t>(d.rows->size());
  s.push_back(pos = 0);
  s.push_back(_for(i = 0, i < rows, ++i)(
      seek(GetStreamAddress(table.get()) + pos),
      kind = read(),
      v.at = read(),
      std::move(dispatch),
      pos += static_cast<std::int32_t>(Row::kBytes)));
  // Not reached: the END row exits. Kept so a malformed table cannot run on.
  s.push_back(ppl::exit(1));
  return s;
}

}  // namespace

const char* RoleName(Role r) {
  switch (r) {
    case Role::kTx: return "Tx";
    case Role::kRx: return "Rx";
    case Role::kGradA: return "GA";
    case Role::kGradB: return "GB";
    case Role::kGradC: return "GC";
    case Role::kUio: return "UIO";
  }
  return "?";
}

Program BuildProgram(const tables::ChannelTables& t, const ProgramOptions& o) {
  Program p;

  // One library stream per channel type. All three gradient axes live on one
  // module, so the shared gradient library is loaded once (plato docs 4.6).
  std::optional<_stream<fmt::Modulation>> rfLib;
  if (!t.rfLib.points().empty()) {
    std::vector<fmt::Modulation::ValueType> v;
    v.reserve(t.rfLib.points().size());
    for (const auto& q : t.rfLib.points())
      v.push_back({q.amplitude, q.phase, q.frequency});
    rfLib.emplace(v.begin(), v.end());
  }
  std::optional<_stream<fmt::Gradient>> gradLib;
  if (!t.gradLib.values().empty())
    gradLib.emplace(t.gradLib.values().begin(), t.gradLib.values().end());

  const struct {
    Role role;
    const std::vector<Row>* rows;
  } channels[] = {{Role::kTx, &t.tx},        {Role::kRx, &t.rx},
                  {Role::kGradA, &t.grad[0]}, {Role::kGradB, &t.grad[1]},
                  {Role::kGradC, &t.grad[2]}, {Role::kUio, &t.uio}};

  for (const auto& ch : channels) {
    // UIO: PTS output bits and/or the respiratory gates.
    if (ch.role == Role::kUio && o.uioOutWord == 0 && t.triggers == 0) continue;
    if (ch.rows->empty() || ch.rows->back().kind() != RowKind::kEnd)
      throw std::runtime_error(std::string(RoleName(ch.role)) +
                               ": table does not end with an END row");
    ChannelData d;
    d.rows = ch.rows;
    bool usesRf = false, usesGrad = false;
    for (const Row& r : *ch.rows) {
      usesRf |= r.kind() == RowKind::kRf;
      usesGrad |= r.kind() == RowKind::kGrad;
    }
    if (usesRf) d.rfLib = rfLib;
    if (usesGrad) d.gradLib = gradLib;

    std::vector<StatementBuilder> body;
    if (o.emitter == Emitter::kUnrolled) {
      body = Unrolled(ch.role, t, d, o);
    } else {
      const auto words = tables::Flatten(*ch.rows);
      _stream<fmt::Raw> table(words.begin(), words.end());
      body = BlockLoop(ch.role, t, d, table, o);
    }

    const std::string name = RoleName(ch.role);
    auto fn = _function(_compound(body));
    auto comp = ch.role != Role::kRx
                    ? _component(name, std::move(fn))
                    : _component(name, std::move(fn),
                                 o.cicCompensation ? _filter::CicCompensation()
                                                   : _filter::Transparent());
    p.sequence.get()->components().push_back(comp.release());
    p.components.push_back({name, ch.role, ch.rows->size()});
  }
  return p;
}

}  // namespace pp::realtime
