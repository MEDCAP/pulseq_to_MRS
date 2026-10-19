#include "config/scan_settings.hpp"

#include "config/yaml_lite.hpp"
#include "units/units.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace pp::config {

namespace {

namespace fs = std::filesystem;
using yaml::Node;

class Reader {
public:
  explicit Reader(std::string source) : source_(std::move(source)) {}

  [[noreturn]] void Fail(const Node& n, const std::string& msg) const {
    throw SettingsError(source_ + ":" + std::to_string(n.line) + ": " + msg);
  }

  // Checks that `n` is a mapping containing only `allowed` keys.
  void ExpectMap(const Node& n, const std::string& what,
                 std::initializer_list<const char*> allowed) const {
    if (n.kind != Node::Kind::kMap) Fail(n, what + " must be a mapping");
    for (const auto& [k, v] : n.map) {
      bool ok = false;
      for (const char* a : allowed) ok = ok || k == a;
      if (!ok) {
        std::string list;
        for (const char* a : allowed) list += std::string(list.empty() ? "" : ", ") + a;
        Fail(v.line ? v : n, "unknown key '" + k + "' in " + what +
                                 " (expected one of: " + list + ")");
      }
    }
  }

  const std::string& Text(const Node& n, const std::string& what) const {
    if (n.kind != Node::Kind::kScalar) Fail(n, what + " must be a single value");
    return n.scalar;
  }

  double Number(const Node& n, const std::string& what) const {
    const std::string& t = Text(n, what);
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(t.c_str(), &end);
    if (t.empty() || *end != '\0' || errno == ERANGE || !std::isfinite(v))
      Fail(n, what + ": '" + t + "' is not a number");
    return v;
  }

  double Positive(const Node& n, const std::string& what) const {
    const double v = Number(n, what);
    if (!(v > 0)) Fail(n, what + " must be > 0");
    return v;
  }

  long long Integer(const Node& n, const std::string& what) const {
    const std::string& t = Text(n, what);
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(t.c_str(), &end, 0);
    if (t.empty() || *end != '\0' || errno == ERANGE)
      Fail(n, what + ": '" + t + "' is not an integer");
    return v;
  }

  unsigned long Mask(const Node& n, const std::string& what) const {
    const long long v = Integer(n, what);  // decimal, or 0x.. hex
    if (v < 0 || v > 0xFFFFFFFFLL) Fail(n, what + " out of range");
    return static_cast<unsigned long>(v);
  }

  bool Bool(const Node& n, const std::string& what) const {
    const std::string& t = Text(n, what);
    if (t == "true" || t == "on" || t == "yes") return true;
    if (t == "false" || t == "off" || t == "no") return false;
    Fail(n, what + ": expected on/off or true/false, got '" + t + "'");
  }

  // Relative paths in the file are relative to the file itself.
  std::string Path(const Node& n, const std::string& what) const {
    const fs::path p(Text(n, what));
    if (p.is_absolute()) return p.generic_string();
    return (fs::path(source_).parent_path() / p).lexically_normal().generic_string();
  }

  const std::string& source() const { return source_; }

private:
  std::string source_;
};

}  // namespace

bool IsSettingsFileName(const std::string& value) {
  const auto ext = fs::path(value).extension().string();
  std::string e;
  for (char c : ext) e += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e == ".yml" || e == ".yaml";
}

std::array<std::uint8_t, 4> ParseIPv4(const std::string& text,
                                      const std::string& what) {
  unsigned a, b, c, d;
  char d1, d2, d3, extra;
  if (std::sscanf(text.c_str(), "%u%c%u%c%u%c%u%c", &a, &d1, &b, &d2, &c, &d3,
                  &d, &extra) != 7 ||
      d1 != '.' || d2 != '.' || d3 != '.' || a > 255 || b > 255 || c > 255 ||
      d > 255)
    throw SettingsError(what + ": '" + text + "' is not an IPv4 address");
  return {static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b),
          static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(d)};
}

ScanSettings LoadScanSettings(const std::string& path) {
  Node root;
  try {
    root = yaml::ParseFile(path);
  } catch (const yaml::ParseError& e) {
    throw SettingsError(e.what());
  }
  Reader r(path);
  ScanSettings s;
  if (root.IsNull()) return s;  // empty file
  r.ExpectMap(root, "the settings file",
              {"sequence", "output", "rf", "gradients", "channels", "receiver",
               "triggering", "modules", "modules_file", "advanced"});

  if (const Node* n = root.Find("sequence")) s.sequence = r.Path(*n, "sequence");
  if (const Node* n = root.Find("output")) s.output = r.Text(*n, "output");

  if (const Node* rf = root.Find("rf"); rf && !rf->IsNull()) {
    r.ExpectMap(*rf, "rf", {"pts_mask", "frequency_hz", "max_amplitude_hz"});
    if (const Node* n = rf->Find("pts_mask")) {
      s.ptsMask = r.Mask(*n, "rf.pts_mask");
      if (!units::PtsBaseFrequencyHz(*s.ptsMask))
        r.Fail(*n, "rf.pts_mask " + std::to_string(*s.ptsMask) +
                       " is not a known PTS setting (0 = none, 15 = 170 MHz, "
                       "93 = 230 MHz, 109 = 250 MHz, 77 = 270 MHz, 13 = 370 MHz)");
    }
    if (const Node* n = rf->Find("frequency_hz"))
      s.frequencyHz = r.Positive(*n, "rf.frequency_hz");
    if (const Node* n = rf->Find("max_amplitude_hz"))
      s.rfMaxHz = r.Positive(*n, "rf.max_amplitude_hz");
  }

  if (const Node* g = root.Find("gradients"); g && !g->IsNull()) {
    r.ExpectMap(*g, "gradients", {"max_hz_per_m"});
    if (const Node* m = g->Find("max_hz_per_m")) {
      r.ExpectMap(*m, "gradients.max_hz_per_m", {"x", "y", "z"});
      static const char* kAxes[3] = {"x", "y", "z"};
      for (int a = 0; a < 3; ++a)
        if (const Node* n = m->Find(kAxes[a]))
          s.gradMaxHzPerM[a] =
              r.Positive(*n, std::string("gradients.max_hz_per_m.") + kAxes[a]);
    }
  }

  if (const Node* c = root.Find("channels"); c && !c->IsNull()) {
    r.ExpectMap(*c, "channels", {"tx_mask", "rx_mask"});
    if (const Node* n = c->Find("tx_mask")) s.txMask = r.Mask(*n, "channels.tx_mask");
    if (const Node* n = c->Find("rx_mask")) s.rxMask = r.Mask(*n, "channels.rx_mask");
  }

  if (const Node* rx = root.Find("receiver"); rx && !rx->IsNull()) {
    r.ExpectMap(*rx, "receiver", {"cic"});
    if (const Node* n = rx->Find("cic")) s.cic = r.Bool(*n, "receiver.cic");
  }

  if (const Node* tr = root.Find("triggering"); tr && !tr->IsNull()) {
    r.ExpectMap(*tr, "triggering", {"respiratory"});
    if (const Node* n = tr->Find("respiratory"))
      s.respiratory = r.Bool(*n, "triggering.respiratory");
  }

  const Node* mods = root.Find("modules");
  const Node* modsFile = root.Find("modules_file");
  if (mods && modsFile)
    r.Fail(*modsFile, "give either 'modules' or 'modules_file', not both");
  if (modsFile) s.modulesFile = r.Path(*modsFile, "modules_file");
  if (mods) {
    if (mods->kind != Node::Kind::kSeq || mods->seq.empty())
      r.Fail(*mods, "modules must be a non-empty list of {name, ip, port}");
    for (std::size_t i = 0; i < mods->seq.size(); ++i) {
      const Node& m = mods->seq[i];
      const std::string what = "modules[" + std::to_string(i) + "]";
      r.ExpectMap(m, what, {"name", "ip", "port"});
      const Node* name = m.Find("name");
      const Node* ip = m.Find("ip");
      const Node* port = m.Find("port");
      if (!name || !ip || !port) r.Fail(m, what + " needs name, ip and port");
      ModuleEntry e;
      e.name = r.Text(*name, what + ".name");
      try {
        e.ip = ParseIPv4(r.Text(*ip, what + ".ip"), what + ".ip");
      } catch (const SettingsError& err) {
        r.Fail(*ip, err.what());
      }
      const long long p = r.Integer(*port, what + ".port");
      if (p <= 0 || p > 65535) r.Fail(*port, what + ".port out of range");
      e.port = static_cast<std::uint16_t>(p);
      for (const auto& prev : s.modules)
        if (prev.name == e.name) r.Fail(*name, "module '" + e.name + "' listed twice");
      s.modules.push_back(e);
    }
  }

  if (const Node* a = root.Find("advanced"); a && !a->IsNull()) {
    r.ExpectMap(*a, "advanced",
                {"emit", "t0_clocks", "epoch_clocks", "timeout_s", "allow_late",
                 "message_latency_clocks", "stdout"});
    if (const Node* n = a->Find("emit")) {
      s.emit = r.Text(*n, "advanced.emit");
      if (*s.emit != "loop" && *s.emit != "unrolled")
        r.Fail(*n, "advanced.emit must be loop or unrolled");
    }
    if (const Node* n = a->Find("t0_clocks")) s.t0Clocks = r.Integer(*n, "advanced.t0_clocks");
    if (const Node* n = a->Find("epoch_clocks")) s.epochClocks = r.Integer(*n, "advanced.epoch_clocks");
    if (const Node* n = a->Find("timeout_s")) s.timeoutSeconds = r.Positive(*n, "advanced.timeout_s");
    if (const Node* n = a->Find("allow_late")) s.allowLate = r.Bool(*n, "advanced.allow_late");
    if (const Node* n = a->Find("message_latency_clocks")) {
      s.messageLatencyClocks = r.Integer(*n, "advanced.message_latency_clocks");
      if (*s.messageLatencyClocks < 0)
        r.Fail(*n, "advanced.message_latency_clocks must be >= 0");
    }
    if (const Node* n = a->Find("stdout")) s.toStdout = r.Bool(*n, "advanced.stdout");
  }
  return s;
}

}  // namespace pp::config
