#include "check.hpp"
#include "config/scan_settings.hpp"
#include "config/yaml_lite.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace pp;
namespace fs = std::filesystem;

static const std::string kConfigDir = PP_CONFIG_DIR;
static const std::string kTmp = PP_TEST_TMP_DIR;

static std::string WriteTmp(const std::string& name, const std::string& text) {
  const std::string p = kTmp + "/" + name;
  std::ofstream(p, std::ios::binary) << text;
  return p;
}

// True if loading `text` fails with a message containing `needle`.
static bool FailsWith(const std::string& text, const std::string& needle) {
  try {
    config::LoadScanSettings(WriteTmp("bad.yml", text));
  } catch (const config::SettingsError& e) {
    if (std::string(e.what()).find(needle) != std::string::npos) return true;
    std::printf("  message was: %s\n", e.what());
    return false;
  }
  std::printf("  no error for: %s\n", text.c_str());
  return false;
}

static void Yaml() {
  using yaml::Node;
  const Node n = yaml::Parse(
      "# comment\n"
      "a: 1\n"
      "b:\n"
      "  c: 'x # not a comment'   # a comment\n"
      "  d: \"q\\\"uote\"\n"
      "list:\n"
      "  - 1\n"
      "  - {k: v, n: [1, 2, 3]}\n"
      "  - name: m\n"
      "    port: 5\n"
      "same_indent:\n"
      "- p\n"
      "- q\n"
      "empty:\n"
      "tilde: ~\n",
      "inline");
  CHECK(n.kind == Node::Kind::kMap);
  CHECK(n.Find("a")->scalar == "1");
  CHECK(n.Find("b")->Find("c")->scalar == "x # not a comment");
  CHECK(n.Find("b")->Find("c")->quoted);
  CHECK(n.Find("b")->Find("d")->scalar == "q\"uote");
  const Node& list = *n.Find("list");
  CHECK_EQ(list.seq.size(), 3u);
  CHECK(list.seq[0].scalar == "1");
  CHECK(list.seq[1].Find("k")->scalar == "v");
  CHECK_EQ(list.seq[1].Find("n")->seq.size(), 3u);
  CHECK(list.seq[2].Find("name")->scalar == "m");
  CHECK(list.seq[2].Find("port")->scalar == "5");
  CHECK_EQ(n.Find("same_indent")->seq.size(), 2u);
  CHECK(n.Find("empty")->IsNull());
  CHECK(n.Find("tilde")->IsNull());
  CHECK_EQ(n.Find("list")->line, 7);  // a block list reports its first item

  CHECK_THROWS(yaml::Parse("a:\n\tb: 1\n", "t"), yaml::ParseError);
  CHECK_THROWS(yaml::Parse("a: 1\na: 2\n", "t"), yaml::ParseError);
  CHECK_THROWS(yaml::Parse("a: 1\n   b: 2\n", "t"), yaml::ParseError);
  CHECK_THROWS(yaml::Parse("a: &x 1\n", "t"), yaml::ParseError);
  CHECK_THROWS(yaml::Parse("a: {b: 1\n", "t"), yaml::ParseError);
  CHECK_THROWS(yaml::Parse("a: b: c\n", "t"), yaml::ParseError);
  CHECK_THROWS(yaml::Parse("a: 'open\n", "t"), yaml::ParseError);
}

static void SimReference() {
  const auto s = config::LoadScanSettings(kConfigDir + "/sim_reference.yml");
  CHECK(s.sequence.has_value());
  CHECK(fs::exists(*s.sequence));  // resolved relative to the settings file
  CHECK(s.sequence->ends_with("test/seqs/fid_v150.seq"));
  CHECK(*s.output == "fid_sim");
  CHECK_NEAR(*s.frequencyHz, 27998450, 0);
  CHECK_NEAR(*s.rfMaxHz, 100, 0);
  CHECK_NEAR(*s.gradMaxHzPerM[0], 57290000, 0);
  CHECK_NEAR(*s.gradMaxHzPerM[1], 61090000, 0);
  CHECK_NEAR(*s.gradMaxHzPerM[2], 60712000, 0);
  CHECK_EQ(*s.txMask, 1ul);
  CHECK_EQ(*s.rxMask, 1ul);
  CHECK_EQ(*s.ptsMask, 77ul);
  CHECK(*s.cic == false);
  CHECK(!s.modulesFile.has_value());
  CHECK_EQ(s.modules.size(), 3u);
  CHECK(s.modules[0].name == "MR4600");
  CHECK_EQ(s.modules[0].ip[0], 127);
  CHECK_EQ(s.modules[0].ip[3], 1);
  CHECK_EQ(s.modules[2].port, 21702);
  CHECK(!s.emit.has_value());
}

static void Scanner() {
  const auto s = config::LoadScanSettings(kConfigDir + "/scanner.yml");
  CHECK(!s.sequence.has_value());
  CHECK_NEAR(*s.gradMaxHzPerM[1], 61090000, 0);  // flow mapping form
  CHECK_EQ(s.modules.size(), 3u);
  CHECK_EQ(s.modules[2].ip[3], 104);
  CHECK_EQ(s.modules[2].port, 4700);
}

static void Errors() {
  CHECK(FailsWith("rf:\n  frequncy_hz: 1\n", ":2: unknown key 'frequncy_hz' in rf"));
  CHECK(FailsWith("rf:\n  frequency_hz: 27.9MHz\n", "is not a number"));
  CHECK(FailsWith("rf:\n  frequency_hz: -5\n", "must be > 0"));
  CHECK(FailsWith("receiver:\n  cic: maybe\n", "expected on/off"));
  CHECK(FailsWith("channels:\n  tx_mask: 0x1G\n", "is not an integer"));
  CHECK(FailsWith("modules_file: a.txt\nmodules:\n  - {name: A, ip: 1.2.3.4, port: 1}\n",
                  "either 'modules' or 'modules_file'"));
  CHECK(FailsWith("modules:\n  - {name: A, ip: 1.2.3, port: 1}\n", "not an IPv4 address"));
  CHECK(FailsWith("modules:\n  - {name: A, ip: 1.2.3.4}\n", "needs name, ip and port"));
  CHECK(FailsWith("modules:\n  - {name: A, ip: 1.2.3.4, port: 70000}\n", "out of range"));
  CHECK(FailsWith("modules:\n  - {name: A, ip: 1.2.3.4, port: 1}\n"
                  "  - {name: A, ip: 1.2.3.5, port: 2}\n", "listed twice"));
  CHECK(FailsWith("advanced:\n  emit: fast\n", "loop or unrolled"));
  CHECK(FailsWith("rf:\n  pts_mask: 78\n",
                  ":2: rf.pts_mask 78 is not a known PTS setting"));
  CHECK(FailsWith("channels:\n  pts_mask: 77\n", "unknown key 'pts_mask' in channels"));
  CHECK(FailsWith("gradients:\n  max_hz_per_m: {x: 1, w: 2}\n", "unknown key 'w'"));
  CHECK(FailsWith("rf: 5\n", "rf must be a mapping"));
  CHECK_THROWS(config::LoadScanSettings(kTmp + "/missing.yml"), config::SettingsError);

  // Hex masks and an empty file are fine.
  const auto h = config::LoadScanSettings(WriteTmp("hex.yml", "rf: {pts_mask: 0x4d}\n"));
  CHECK_EQ(*h.ptsMask, 77ul);
  // 13C on this scanner (smis.ini): 104 MHz - NCO 29046641 Hz.
  const auto c = config::LoadScanSettings(
      WriteTmp("c13.yml", "rf: {pts_mask: 190, frequency_hz: 29046641}\n"));
  CHECK_EQ(*c.ptsMask, 190ul);
  CHECK_NEAR(*c.frequencyHz, 29046641, 0);
  CHECK(FailsWith("rf:\n  pts_mask: 15\n", "is not a known PTS setting"));
  const auto e = config::LoadScanSettings(WriteTmp("empty.yml", "# nothing\n"));
  CHECK(!e.frequencyHz.has_value());
}

static void FileNames() {
  CHECK(config::IsSettingsFileName("config/sim_reference.yml"));
  CHECK(config::IsSettingsFileName("C:/x/Scan.YAML"));
  CHECK(!config::IsSettingsFileName("27998450"));
  CHECK(!config::IsSettingsFileName("4.2e6"));
  CHECK(!config::IsSettingsFileName("modules.txt"));
}

static void Run() {
  Yaml();
  SimReference();
  Scanner();
  Errors();
  FileNames();
}

TEST_MAIN(Run)
