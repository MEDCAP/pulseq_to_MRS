// Probe 10: minimum row-to-row gap the block loop sustains, per channel kind.
//
// Builds a synthetic model with 16 events of one kind whose gap, as the table
// builder's slack check measures it (next row's first timed call minus the
// previous row's last timed action), steps down from 400 to 0 clocks. Runs it
// through the real block-loop program and writes the expected log times; then
//   python test/compare.py expect test/out/p10_<kind>.expect test/out/p10_<kind>
// shows which gaps run late.
// With `accepted`, the default slack rules apply and the rows the table
// builder flags as too close are left out of the expect file: every row it
// accepts must then be on time (test/regress_phase3.sh).
// Usage: p10_gap_stress tx|rx|grad <modules file> <expect file> [accepted]
#include "plato/session.hpp"
#include "realtime/program.hpp"
#include "tables/channel_table.hpp"
#include "tables/expect.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  using namespace pp;
  if (argc != 4 && !(argc == 5 && !std::strcmp(argv[4], "accepted"))) {
    std::fprintf(stderr,
                 "usage: %s tx|rx|grad <modules> <expect> [accepted]\n",
                 argv[0]);
    return 2;
  }
  const bool accepted = argc == 5;
  const std::string kind = argv[1];
  const int gaps[] = {400, 300, 200, 150, 120, 100, 90, 80,
                      70,  60,  50,  40,  30,  20,  10, 0};

  tables::ScanConfig cfg;
  cfg.larmorHz = 27998450;
  cfg.rfMaxHz = 100;
  cfg.gradMaxHzPerM = {57290000, 61090000, 60712000};
  // Measuring: report nothing, the log is the measurement.
  if (!accepted) cfg.txSlackClk = cfg.rxSlackClk = cfg.gradSlackClk = 0;

  model::SequenceModel m;
  model::Clk t = 1000;  // sequence time of the first event
  for (int k = 0; k < 16; ++k) {
    if (kind == "tx") {
      // 4-point pulse, 1 us raster; distinct amplitude per row. Row span:
      // first = start - txPhaseLead(50) (gate on at start), last = gateOff =
      // start + 50 + 4 * 100 + 50.
      model::RfEvent rf;
      rf.block = k;
      rf.start = t;
      rf.rasterClk = 100;
      rf.amplitudeHz = 5.0 * (k + 1);
      rf.magnitude.assign(4, 1.0f);
      rf.phaseRad.assign(4, 0.0f);
      m.rf.push_back(rf);
      std::printf("row %2d gap %3d start %lld\n", k, k ? gaps[k - 1] : -1,
                  static_cast<long long>(t));
      t += 500 + 50 + gaps[k];
    } else if (kind == "rx") {
      // 4 samples at 1 us: first = at - rxFreqLead(51), last = at + 400.
      model::AdcEvent a;
      a.block = k;
      a.start = t;
      a.numSamples = 4;
      a.dwellClk = 100;
      m.adc.push_back(a);
      std::printf("row %2d gap %3d start %lld\n", k, k ? gaps[k - 1] : -1,
                  static_cast<long long>(t));
      t += 400 + 51 + gaps[k];
    } else if (kind == "grad") {
      // 3 edge points 0, v, 0 at 1 us on x: first = start, last = start + 200.
      // Gap 0 would join the rows (shared edge), so the last gap is 10.
      model::GradEvent g;
      g.block = k;
      g.axis = 0;
      g.start = t;
      g.rasterClk = 100;
      g.samplesHzPerM = {0.0, 0.01 * (k + 1) * cfg.gradMaxHzPerM[0], 0.0};
      m.grad.push_back(g);
      std::printf("row %2d gap %3d start %lld\n", k, k ? gaps[k - 1] : -1,
                  static_cast<long long>(t));
      t += 200 + (gaps[k] ? gaps[k] : 10);
    } else {
      std::fprintf(stderr, "unknown kind %s\n", argv[1]);
      return 2;
    }
  }
  m.duration = t + 10000;

  const auto tables = tables::BuildTables(m, cfg);
  if (!tables::WriteExpect(argv[3], tables, cfg)) return 1;
  if (accepted) {
    // Timing issues name "block <k + 1>"; row k is event k (no origins here).
    std::set<std::string> flagged;
    for (const auto& w : tables.timingIssues) {
      std::printf("timing: %s\n", w.c_str());
      const auto p = w.find("block ");
      const int block = std::atoi(w.c_str() + p + 6);
      flagged.insert(std::to_string(block - 1));
    }
    std::ifstream in(argv[3]);
    std::vector<std::string> keep;
    std::string line;
    while (std::getline(in, line)) {
      // Fields: file command time lateness row [grid period points peak].
      std::istringstream fields(line);
      std::string file, cmd, time, late, row;
      fields >> file >> cmd >> time >> late >> row;
      const auto digit = row.find_first_of("0123456789");
      if (line[0] != '#' && digit != std::string::npos &&
          flagged.count(row.substr(digit)))
        continue;
      keep.push_back(line);
    }
    in.close();
    std::ofstream out(argv[3]);
    for (const auto& l : keep) out << l << "\n";
    std::printf("accepted mode: %zu rows flagged and not checked\n",
                flagged.size());
  }
  realtime::ProgramOptions po;
  const auto program = realtime::BuildProgram(tables, po);

  plato::RunOptions ro;
  ro.modules = plato::ReadModulesFile(argv[2]);
  for (const auto& r : tables.rx)
    if (r.kind() == tables::RowKind::kAdc) ro.expectedSamples += r.w[2];
  ro.sequenceSeconds = 0.01;
  return plato::Run(program, ro);
}
