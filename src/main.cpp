// pulseq_plato: runs a Pulseq 1.5 .seq file on PLATO modules or simulator.exe.
// A replacement for plato/bin/pulseq.exe with the same options, plus a few
// for testing (see Usage()). All settings can also come from one YAML file
// given as `-f settings.yml` (see src/config/scan_settings.hpp); a numeric -f
// is the frequency as in the vendor exe. Options on the command line override
// the file.
#include "config/scan_settings.hpp"
#include "plato/session.hpp"
#include "realtime/program.hpp"
#include "seq/seq_loader.hpp"
#include "tables/channel_table.hpp"
#include "tables/expect.hpp"
#include "units/units.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

namespace {

void Usage(const char* argv0) {
  std::fprintf(stderr,
      "usage: %s [-f settings.yml] [options...] [sequence-file]\n"
      "Options (as plato/bin/pulseq.exe):\n"
      "  -f       settings.yml | rf-frequency-hz\n"
      "           a .yml/.yaml file: all settings in one file (config/*.yml);\n"
      "           other options on the command line override it.\n"
      "           a number: NCO frequency (Hz), the offset from the PTS base\n"
      "  -ptsmask PTS base frequency: 77 = 270 MHz (C:\\smis\\smis.ini),\n"
      "           15 = 170, 93 = 230, 109 = 250, 13 = 370 MHz, 0 = none\n"
      "  -m       module-configuration-file\n"
      "  -o       output-file (writes <output-file>_0.cfl)\n"
      "  -stdout  write data to standard output\n"
      "  -rfmax   max-rf-amp-hz\n"
      "  -rxmask  receiver-mask, bit i = receiver i (only 1 is supported yet)\n"
      "  -txmask  transmitter-mask, bit i = transmitter i (only 1 supported yet)\n"
      "  -cic     on/off\n"
      "  -gx      max-gradient-amp-hz/m\n"
      "  -gy      max-gradient-amp-hz/m\n"
      "  -gz      max-gradient-amp-hz/m\n"
      "Testing options:\n"
      "  -emit    loop|unrolled   realtime program form (default loop)\n"
      "  -t0      clocks          RT clock of sequence t = 0 (default 10000)\n"
      "  -epoch   clocks          rebase with origin() this often (default 2^30)\n"
      "  -timeout seconds         wait limit (default duration + 10 s)\n"
      "  -expect  file            write the expected simulator log times\n"
      "  -n                       build everything, do not connect or run\n"
      "  -allow-late              run even if rows are too close for the loop\n",
      argv0);
}

}  // namespace

int main(int argc, char** argv) {
  pp::seq::LoadOptions lo;
  pp::tables::ScanConfig cfg;
  pp::realtime::ProgramOptions po;
  pp::plato::RunOptions ro;
  std::string modulesFile, expectFile, seqPathStore;
  const char* seqPath = nullptr;
  bool dryRun = false, allowLate = false;
  unsigned long rxMask = 1, txMask = 1, ptsMask = 0;

  // 1. A settings file (-f x.yml, wherever it appears), then 2. the other
  //    options, which therefore override it.
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "-f") ||
        !pp::config::IsSettingsFileName(argv[i + 1]))
      continue;
    try {
      const auto s = pp::config::LoadScanSettings(argv[i + 1]);
      if (s.sequence) seqPath = (seqPathStore = *s.sequence).c_str();
      if (s.output) ro.outputPrefix = *s.output;
      if (s.ptsMask) ptsMask = *s.ptsMask;
      if (s.frequencyHz) cfg.larmorHz = *s.frequencyHz;
      if (s.rfMaxHz) cfg.rfMaxHz = *s.rfMaxHz;
      for (int a = 0; a < 3; ++a)
        if (s.gradMaxHzPerM[a]) cfg.gradMaxHzPerM[a] = *s.gradMaxHzPerM[a];
      if (s.txMask) txMask = *s.txMask;
      if (s.rxMask) rxMask = *s.rxMask;
      if (s.cic) cfg.cicCompensation = po.cicCompensation = *s.cic;
      if (s.modulesFile) modulesFile = *s.modulesFile;
      for (const auto& m : s.modules) {
        pp::plato::ModuleAddress a;
        a.name = m.name;
        for (int k = 0; k < 4; ++k) a.ip[k] = m.ip[k];
        a.port = m.port;
        ro.modules.push_back(a);
      }
      if (s.emit)
        po.emitter = *s.emit == "unrolled" ? pp::realtime::Emitter::kUnrolled
                                           : pp::realtime::Emitter::kBlockLoop;
      if (s.t0Clocks) cfg.startOffset = *s.t0Clocks;
      if (s.epochClocks) cfg.epochLimitClk = *s.epochClocks;
      if (s.timeoutSeconds) ro.timeoutSeconds = *s.timeoutSeconds;
      if (s.allowLate) allowLate = *s.allowLate;
      if (s.toStdout) ro.dataToStdout = *s.toStdout;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "error: %s\n", e.what());
      return 2;
    }
  }

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a);
        std::exit(2);
      }
      return argv[++i];
    };
    if (!std::strcmp(a, "-f")) {
      const char* v = next();
      if (pp::config::IsSettingsFileName(v)) continue;  // loaded above
      char* end = nullptr;
      const double f = std::strtod(v, &end);
      if (*v == '\0' || *end != '\0' || !(f > 0)) {
        std::fprintf(stderr, "-f expects a frequency in Hz or a .yml settings "
                             "file, got '%s'\n", v);
        return 2;
      }
      cfg.larmorHz = f;
    }
    else if (!std::strcmp(a, "-m")) {
      modulesFile = next();
      ro.modules.clear();  // -m overrides a module list from the settings file
    }
    else if (!std::strcmp(a, "-o")) ro.outputPrefix = next();
    else if (!std::strcmp(a, "-stdout")) ro.dataToStdout = true;
    else if (!std::strcmp(a, "-rfmax")) cfg.rfMaxHz = std::atof(next());
    else if (!std::strcmp(a, "-rxmask")) rxMask = std::strtoul(next(), nullptr, 0);
    else if (!std::strcmp(a, "-txmask")) txMask = std::strtoul(next(), nullptr, 0);
    else if (!std::strcmp(a, "-ptsmask")) ptsMask = std::strtoul(next(), nullptr, 0);
    else if (!std::strcmp(a, "-cic")) {
      const char* v = next();
      if (std::strcmp(v, "on") && std::strcmp(v, "off")) {
        std::fprintf(stderr, "-cic expects on or off\n");
        return 2;
      }
      cfg.cicCompensation = po.cicCompensation = !std::strcmp(v, "on");
    }
    else if (!std::strcmp(a, "-gx")) cfg.gradMaxHzPerM[0] = std::atof(next());
    else if (!std::strcmp(a, "-gy")) cfg.gradMaxHzPerM[1] = std::atof(next());
    else if (!std::strcmp(a, "-gz")) cfg.gradMaxHzPerM[2] = std::atof(next());
    else if (!std::strcmp(a, "-emit")) {
      const char* v = next();
      if (!std::strcmp(v, "loop")) po.emitter = pp::realtime::Emitter::kBlockLoop;
      else if (!std::strcmp(v, "unrolled")) po.emitter = pp::realtime::Emitter::kUnrolled;
      else {
        std::fprintf(stderr, "-emit expects loop or unrolled\n");
        return 2;
      }
    }
    else if (!std::strcmp(a, "-t0")) cfg.startOffset = std::atoll(next());
    else if (!std::strcmp(a, "-epoch")) cfg.epochLimitClk = std::atoll(next());
    else if (!std::strcmp(a, "-timeout")) ro.timeoutSeconds = std::atof(next());
    else if (!std::strcmp(a, "-expect")) expectFile = next();
    else if (!std::strcmp(a, "-n")) dryRun = true;
    else if (!std::strcmp(a, "-allow-late")) allowLate = true;
    else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
      Usage(argv[0]);
      return 0;
    } else if (a[0] == '-') {
      std::fprintf(stderr, "unknown option %s\n", a);
      Usage(argv[0]);
      return 2;
    } else seqPath = a;
  }
  if (!seqPath) {
    Usage(argv[0]);
    return 2;
  }
  std::FILE* log = ro.dataToStdout ? stderr : stdout;
  ro.log = log;

  // Frequency: the PTS synthesizer gives the base (pts_mask, driven on UIO
  // bits 8-15 like the vendor), the NCO adds -f, or subtracts it for the
  // low-side masks (13C etc.). The NCO is programmed with -f; the physical
  // frequency base +- -f is what the Pulseq 1.5 ppm offsets scale with.
  const auto pts = pp::units::LookupPts(ptsMask);
  if (!pts) {
    std::fprintf(stderr, "error: -ptsmask %lu is not a known PTS setting (%s)\n",
                 ptsMask, pp::units::kPtsMaskList);
    return 2;
  }
  lo.larmorHz = pts->baseHz + pts->sign * cfg.larmorHz;
  cfg.sideband = pts->sign;
  ro.conjugateData = pts->sign < 0;
  po.uioOutWord = static_cast<std::uint32_t>(ptsMask) << 8;
  std::fprintf(log, "frequency: PTS base %.6f MHz (pts_mask %lu) %c NCO %.6f MHz "
                    "= %.6f MHz%s\n",
               pts->baseHz * 1e-6, ptsMask, pts->sign < 0 ? '-' : '+',
               cfg.larmorHz * 1e-6, lo.larmorHz * 1e-6,
               pts->sign < 0 ? " (low side: offsets and phases mirrored, data "
                               "conjugated)"
                             : "");

  if (rxMask != 1 || txMask != 1) {
    std::fprintf(stderr, "error: only -rxmask 1 and -txmask 1 are supported "
                         "yet (several channels: Phase 4)\n");
    return 2;
  }
  if (!dryRun && modulesFile.empty() && ro.modules.empty()) {
    std::fprintf(stderr, "error: no modules: give -m module-configuration-file, "
                         "or 'modules'/'modules_file' in the settings file\n");
    return 2;
  }
  po.rxFreqLeadClk = cfg.rxFreqLeadClk;
  po.txPhaseLeadClk = cfg.txPhaseLeadClk;
  if (cfg.startOffset <= po.ncoResetAt) {
    std::fprintf(stderr, "error: -t0 must be > %lld (NCO reset time)\n",
                 static_cast<long long>(po.ncoResetAt));
    return 2;
  }

  try {
    const auto m = pp::seq::LoadSequence(seqPath, lo);
    std::fprintf(log, "%s: Pulseq %d.%d.%d, %zu blocks, %.3f ms\n", seqPath,
                 m.version / 1000000, m.version / 1000 % 1000,
                 m.version % 1000, m.blocks.size(), m.duration * 1e-5);
    const auto t = pp::tables::BuildTables(m, cfg);
    for (const auto& w : t.warnings) std::fprintf(log, "warning: %s\n", w.c_str());
    for (const auto& w : t.timingIssues)
      std::fprintf(log, "timing: %s\n", w.c_str());
    if (!expectFile.empty() && !pp::tables::WriteExpect(expectFile, t, cfg)) {
      std::fprintf(stderr, "error: cannot write %s\n", expectFile.c_str());
      return 1;
    }

    if (!t.timingIssues.empty() && !allowLate) {
      std::fprintf(stderr, "error: %zu row(s) are too close to the previous "
                           "one for the block loop and would play late (see "
                           "'timing:' above); -allow-late runs anyway\n",
                   t.timingIssues.size());
      return 3;
    }
    const auto program = pp::realtime::BuildProgram(t, po);
    std::fprintf(log, "program: %s, rf library %zu points, gradient library "
                      "%zu points, %d origin() rows\n",
                 po.emitter == pp::realtime::Emitter::kBlockLoop ? "block loop"
                                                                 : "unrolled",
                 t.rfLib.points().size(), t.gradLib.values().size(),
                 t.origins);
    // Stream memory per module (plato docs 4.6: a stream is loaded once per
    // module that uses it). The unrolled program loads no tables.
    const bool loop = po.emitter == pp::realtime::Emitter::kBlockLoop;
    auto tableBytes = [&](const std::vector<pp::tables::Row>& r) {
      return loop ? r.size() * pp::tables::Row::kBytes : 0;
    };
    std::fprintf(log, "stream memory: Tx module %zu B (rf library %zu + table "
                      "%zu), Rx module %zu B, gradient module %zu B (library "
                      "%zu + tables %zu)\n",
                 t.rfLib.points().size() * pp::shapes::RfLibrary::kBytesPerPoint +
                     tableBytes(t.tx),
                 t.rfLib.points().size() * pp::shapes::RfLibrary::kBytesPerPoint,
                 tableBytes(t.tx), tableBytes(t.rx),
                 t.gradLib.values().size() * pp::shapes::GradLibrary::kBytesPerPoint +
                     tableBytes(t.grad[0]) + tableBytes(t.grad[1]) +
                     tableBytes(t.grad[2]),
                 t.gradLib.values().size() * pp::shapes::GradLibrary::kBytesPerPoint,
                 tableBytes(t.grad[0]) + tableBytes(t.grad[1]) +
                     tableBytes(t.grad[2]));
    if (dryRun) return 0;

    if (!modulesFile.empty()) ro.modules = pp::plato::ReadModulesFile(modulesFile);
    for (const auto& r : t.rx)
      if (r.kind() == pp::tables::RowKind::kAdc) ro.expectedSamples += r.w[2];
    ro.sequenceSeconds = static_cast<double>(t.end) / cfg.clocks.rtClockHz;
    return pp::plato::Run(program, ro);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
