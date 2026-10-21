// pulseq_plato_dump: loads a .seq and prints the host model and the per-channel
// block-row tables that the realtime loop will read. No hardware needed.
//
// usage: pulseq_plato_dump [-f Hz] [-ptsmask N] [-rfmax Hz] [-gx|-gy|-gz Hz/m] [-cic on|off]
//                          [-rows N] [-wave N] [-export waves.csv] sequence.seq
// -export writes every played sample in physical units for
// test/plot_waveforms.py (format: tables/export.hpp).
// Defaults mirror the vendor reference run: -f 27995000 -rfmax 100 -g* 4.2e6.
#include "seq/seq_loader.hpp"
#include "tables/channel_table.hpp"
#include "tables/export.hpp"
#include "units/units.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// -wave N: also print the first N waveform values of each gradient row, as
// int16 DAC counts (value * 32767, rounded).
const std::vector<double>* gWaveLib = nullptr;
std::size_t gWavePoints = 0;

void PrintRows(const char* name, const std::vector<pp::tables::Row>& rows,
               std::size_t limit) {
  using pp::tables::RowKind;
  std::printf("%s: %zu rows\n", name, rows.size());
  for (std::size_t i = 0; i < rows.size() && i < limit; ++i) {
    const auto& r = rows[i];
    switch (r.kind()) {
      case RowKind::kRf:
        std::printf("  %4zu RF   at=%-10u lib=+%-8u points=%-6u period=%u "
                    "gate=[%u, %u] ph16=0x%04x\n",
                    i, r.at(), r.w[2], r.w[3] + 1, r.w[4], r.w[5], r.w[6],
                    r.w[7]);
        break;
      case RowKind::kGrad:
        std::printf("  %4zu GRAD at=%-10u lib=+%-8u points=%-6u period=%u", i,
                    r.at(), r.w[2], r.w[3] + 1, r.w[4]);
        if (gWaveLib) {
          std::printf(" counts:");
          for (std::size_t j = 0; j < gWavePoints && j <= r.w[3]; ++j)
            std::printf(" %.2f", (*gWaveLib)[r.w[2] / 4 + j] * 32767.0);
        }
        std::printf("\n");
        break;
      case RowKind::kAdc:
        std::printf("  %4zu ADC  at=%-10u n=%u dwell=%u freqWord=0x%08x "
                    "ph16=0x%04x restoreAt=%u grid=%u (first sample %u)\n",
                    i, r.at(), r.w[2], r.w[3], r.w[4], r.w[5], r.w[6], r.w[7],
                    r.at() + r.w[3]);
        break;
      case RowKind::kEnd:
        std::printf("  %4zu END  at=%u\n", i, r.at());
        break;
      case RowKind::kTrigger:
        std::printf("  %4zu TRIGGER halt=%u originDelay=%u grid=%u setgrid=%u "
                    "arm=%u (later rows count from the release)\n",
                    i, r.at(), r.w[2], r.w[3], r.w[4], r.w[5]);
        break;
      case RowKind::kOrigin:
        std::printf("  %4zu ORIGIN at=%u (later rows count from here + 10)\n",
                    i, r.at());
        break;
    }
  }
  if (rows.size() > limit) std::printf("  ... (%zu more)\n", rows.size() - limit);
}

}  // namespace

int main(int argc, char** argv) {
  pp::seq::LoadOptions lo;
  pp::tables::ScanConfig cfg;
  cfg.larmorHz = 27995000;  // NCO frequency (-f), offset from the PTS base
  unsigned long ptsMask = 0;
  cfg.rfMaxHz = 100;
  cfg.gradMaxHzPerM = {4.2e6, 4.2e6, 4.2e6};
  std::size_t rowLimit = 20;
  const char* path = nullptr;
  const char* exportPath = nullptr;

  for (int i = 1; i < argc; ++i) {
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", argv[i]);
        std::exit(2);
      }
      return argv[++i];
    };
    if (!std::strcmp(argv[i], "-f")) cfg.larmorHz = std::atof(next());
    else if (!std::strcmp(argv[i], "-ptsmask")) ptsMask = std::strtoul(next(), nullptr, 0);
    else if (!std::strcmp(argv[i], "-rfmax")) cfg.rfMaxHz = std::atof(next());
    else if (!std::strcmp(argv[i], "-gx")) cfg.gradMaxHzPerM[0] = std::atof(next());
    else if (!std::strcmp(argv[i], "-gy")) cfg.gradMaxHzPerM[1] = std::atof(next());
    else if (!std::strcmp(argv[i], "-gz")) cfg.gradMaxHzPerM[2] = std::atof(next());
    else if (!std::strcmp(argv[i], "-cic")) cfg.cicCompensation = !std::strcmp(next(), "on");
    else if (!std::strcmp(argv[i], "-trigger")) cfg.respiratoryTrigger = !std::strcmp(next(), "on");
    else if (!std::strcmp(argv[i], "-rows")) rowLimit = std::strtoul(next(), nullptr, 10);
    else if (!std::strcmp(argv[i], "-wave")) gWavePoints = std::strtoul(next(), nullptr, 10);
    else if (!std::strcmp(argv[i], "-export")) exportPath = next();
    else if (argv[i][0] == '-') {
      std::fprintf(stderr, "unknown option %s\n", argv[i]);
      return 2;
    } else path = argv[i];
  }
  if (!path) {
    std::fprintf(stderr, "usage: %s [options] sequence.seq\n", argv[0]);
    return 2;
  }
  // ppm offsets scale with the physical frequency: PTS base +- NCO (-f).
  const auto pts = pp::units::LookupPts(ptsMask);
  if (!pts) {
    std::fprintf(stderr, "unknown -ptsmask %lu (%s)\n", ptsMask,
                 pp::units::kPtsMaskList);
    return 2;
  }
  lo.larmorHz = pts->baseHz + pts->sign * cfg.larmorHz;
  cfg.sideband = pts->sign;

  try {
    const auto m = pp::seq::LoadSequence(path, lo);
    std::printf("%s: Pulseq %d.%d.%d '%s'\n", path, m.version / 1000000,
                m.version / 1000 % 1000, m.version % 1000, m.name.c_str());
    std::printf("rasters: block %g us, grad %g us, rf %g us, adc %g us\n",
                m.rasters.blockUs, m.rasters.gradUs, m.rasters.rfUs,
                m.rasters.adcUs);
    std::printf("blocks %zu, rf %zu, grad %zu, adc %zu, duration %lld clk "
                "(%.3f ms)\n",
                m.blocks.size(), m.rf.size(), m.grad.size(), m.adc.size(),
                static_cast<long long>(m.duration), m.duration * 1e-5);
    for (const auto& w : m.readerMessages) std::printf("reader: %s\n", w.c_str());

    const auto t = pp::tables::BuildTables(m, cfg);
    std::printf("rf library: %zu unique waveforms, %zu points (%zu B); grad "
                "library: %zu unique, %zu points (%zu B)\n",
                t.rfLib.unique(), t.rfLib.points().size(),
                t.rfLib.points().size() * 8, t.gradLib.unique(),
                t.gradLib.values().size(), t.gradLib.values().size() * 4);
    std::printf("base freq word 0x%08x, end at %lld\n", t.baseFreqWord,
                static_cast<long long>(t.end));
    if (gWavePoints) gWaveLib = &t.gradLib.values();
    PrintRows("Tx", t.tx, rowLimit);
    PrintRows("GA", t.grad[0], rowLimit);
    PrintRows("GB", t.grad[1], rowLimit);
    PrintRows("GC", t.grad[2], rowLimit);
    PrintRows("Rx", t.rx, rowLimit);
    if (t.triggers) PrintRows("UIO", t.uio, rowLimit);
    for (const auto& w : t.warnings) std::printf("warning: %s\n", w.c_str());
    for (const auto& w : t.timingIssues) std::printf("timing: %s\n", w.c_str());
    if (exportPath) {
      if (!pp::tables::WriteWaveforms(exportPath, t, cfg)) {
        std::fprintf(stderr, "error: cannot write %s\n", exportPath);
        return 1;
      }
      std::printf("waveforms written to %s\n", exportPath);
    }
    return t.timingIssues.empty() ? 0 : 3;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
