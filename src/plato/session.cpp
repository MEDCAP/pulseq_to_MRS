#include "plato/session.hpp"

#include <acquisition_data_pump.hpp>
#include <report.hpp>
#include <sequence_component_mapping.hpp>
#include <spectrometer.hpp>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <WinSock2.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace pp::plato {

namespace mp = mrs::plato;

std::vector<ModuleAddress> ReadModulesFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open modules file " + path);
  std::vector<ModuleAddress> out;
  std::string line;
  int lineNo = 0;
  while (std::getline(in, line)) {
    ++lineNo;
    if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
    std::istringstream ls(line);
    ModuleAddress m;
    std::string ip;
    unsigned port = 0;
    if (!(ls >> m.name)) continue;  // blank line
    unsigned a, b, c, d;
    char d1, d2, d3;
    if (!(ls >> ip >> port) ||
        std::sscanf(ip.c_str(), "%u%c%u%c%u%c%u", &a, &d1, &b, &d2, &c, &d3,
                    &d) != 7 ||
        d1 != '.' || d2 != '.' || d3 != '.' || a > 255 || b > 255 || c > 255 ||
        d > 255 || port == 0 || port > 65535)
      throw std::runtime_error(path + ":" + std::to_string(lineNo) +
                               ": expected '<name> <a.b.c.d> <port>'");
    m.ip[0] = static_cast<std::uint8_t>(a);
    m.ip[1] = static_cast<std::uint8_t>(b);
    m.ip[2] = static_cast<std::uint8_t>(c);
    m.ip[3] = static_cast<std::uint8_t>(d);
    m.port = static_cast<std::uint16_t>(port);
    out.push_back(m);
  }
  if (out.empty()) throw std::runtime_error(path + ": no modules listed");
  return out;
}

namespace {

// Collects acquisitions in order and writes them as complex64 (.cfl, the BART
// data layout: interleaved float32 real/imag). Samples are two 24-bit
// big-endian words, I then Q (FINDINGS R3). The sequence appends no metadata.
class CflWriter : public mp::AcquisitionDataPump::Delegate {
public:
  explicit CflWriter(std::FILE* out) : out_(out) {}

  void AcquisitionDataPump_Submit(mp::AcquisitionDataPump*, unsigned,
                                  std::uint32_t acqId,
                                  std::uint32_t fragmentOffset, bool last,
                                  const void* payload,
                                  std::size_t size) override {
    std::lock_guard<std::mutex> lock(m_);
    auto& a = pending_[acqId];
    if (a.bytes.size() < fragmentOffset + size)
      a.bytes.resize(fragmentOffset + size);
    const auto* p = static_cast<const std::uint8_t*>(payload);
    std::copy(p, p + size, a.bytes.begin() + fragmentOffset);
    a.received += size;
    if (last) a.total = fragmentOffset + size;
    // Write every acquisition that is complete, in id order.
    for (auto it = pending_.begin();
         it != pending_.end() && it->first == nextId_ && it->second.total &&
         it->second.received >= it->second.total;
         it = pending_.erase(it), ++nextId_)
      Write(it->second.bytes);
    cv_.notify_all();
  }

  // Waits until `samples` have been written or the deadline passes.
  bool WaitFor(std::uint64_t samples,
               std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(m_);
    return cv_.wait_until(lock, deadline,
                          [&] { return samplesWritten_ >= samples; });
  }

  std::uint64_t samples() {
    std::lock_guard<std::mutex> lock(m_);
    return samplesWritten_;
  }
  std::size_t incomplete() {
    std::lock_guard<std::mutex> lock(m_);
    return pending_.size();
  }

private:
  struct Acq {
    std::vector<std::uint8_t> bytes;
    std::size_t received = 0;
    std::size_t total = 0;  // known once the last fragment arrived
  };

  static std::int32_t S24(const std::uint8_t* b) {
    const std::int32_t v = (b[0] << 16) | (b[1] << 8) | b[2];
    return (v & 0x800000) ? v - 0x1000000 : v;
  }

  void Write(const std::vector<std::uint8_t>& bytes) {
    std::vector<float> iq;
    iq.reserve(bytes.size() / 3);
    for (std::size_t k = 0; k + 6 <= bytes.size(); k += 6) {
      iq.push_back(static_cast<float>(S24(&bytes[k])));
      iq.push_back(static_cast<float>(S24(&bytes[k + 3])));
    }
    if (out_) std::fwrite(iq.data(), sizeof(float), iq.size(), out_);
    samplesWritten_ += iq.size() / 2;
  }

  std::FILE* out_;
  std::mutex m_;
  std::condition_variable cv_;
  std::map<std::uint32_t, Acq> pending_;
  std::uint32_t nextId_ = 0;
  std::uint64_t samplesWritten_ = 0;
};

struct Placement {
  std::string module;
  mp::ModuleChannel channel;
};

}  // namespace

int Run(const realtime::Program& program, const RunOptions& o) {
  std::FILE* log = o.log;
#ifdef _WIN32
  WSADATA wsaData;
  WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
  auto spec = mp::CreateSpectrometer();

  // Connect and find the channels.
  std::optional<std::string> txMod, rxMod, gradMod;
  for (const auto& m : o.modules) {
    auto [r, mod] = spec->CreateModule(
        m.name, mp::IPv4Endpoint(mp::IPv4Address(m.ip[0], m.ip[1], m.ip[2],
                                                 m.ip[3]),
                                 m.port));
    if (r != mp::Module::Result::kSuccess || !mod) {
      std::fprintf(log, "error: cannot connect to module %s at %u.%u.%u.%u:%u "
                        "(result %d)\n",
                   m.name.c_str(), m.ip[0], m.ip[1], m.ip[2], m.ip[3], m.port,
                   static_cast<int>(r));
      return 1;
    }
    std::fprintf(log, "module %s: %s serial %s, tx %u rx %u grad %u, stream "
                      "memory %u MiB\n",
                 m.name.c_str(), mod->GetType().c_str(),
                 mod->GetSerial().c_str(), mod->GetTransmitterCount(),
                 mod->GetReceiverCount(), mod->GetGradientPartialCount(),
                 mod->GetStreamMemorySize() >> 20);
    if (!txMod && mod->GetTransmitterCount() > 0) txMod = m.name;
    if (!rxMod && mod->GetReceiverCount() > 0) rxMod = m.name;
    if (!gradMod && mod->GetGradientPartialCount() > 0) gradMod = m.name;
  }
  spec->SetMasterModuleName(o.modules.front().name);

  mp::SequenceComponentMapping mapping;
  std::map<std::string, Placement> placed;
  for (const auto& c : program.components) {
    std::optional<std::string> mod;
    mp::ModuleChannel ch = mp::ModuleChannel::Transmitter(0);
    switch (c.role) {
      case realtime::Role::kTx: mod = txMod; break;
      case realtime::Role::kRx:
        mod = rxMod;
        ch = mp::ModuleChannel::Receiver(0);
        break;
      case realtime::Role::kGradA:
        mod = gradMod;
        ch = mp::ModuleChannel::GradientA(0);
        break;
      case realtime::Role::kGradB:
        mod = gradMod;
        ch = mp::ModuleChannel::GradientB(0);
        break;
      case realtime::Role::kGradC:
        mod = gradMod;
        ch = mp::ModuleChannel::GradientC(0);
        break;
      case realtime::Role::kUio:
        mod = gradMod;  // the uncommitted I/O sits on the gradient module (MR4600)
        ch = mp::ModuleChannel::Uio(0);
        break;
    }
    if (!mod) {
      std::fprintf(log, "error: no module provides a channel for %s\n",
                   c.name.c_str());
      return 1;
    }
    mapping.Insert(c.name, {*mod, ch});
    placed.emplace(c.name, Placement{*mod, ch});
  }

  mp::Report report;
  auto [res, runner] = spec->CreateSequenceRunner(
      "pulseq_plato", program.sequence, mapping, &report);
  for (const auto& r : report.records())
    std::fprintf(log, "report: %s\n", r.description.c_str());
  if (res != mp::SequenceRuntimeResult::kSuccess || !runner) {
    std::fprintf(log, "error: CreateSequenceRunner failed (%d)\n",
                 static_cast<int>(res));
    return 1;
  }
  for (const auto& c : program.components) {
    std::uint32_t code = 0, data = 0;
    (void)runner->GetCodeSize(c.name, code);
    (void)runner->GetDataSize(c.name, data);
    mp::ModuleChannelInformation info{};
    const auto& pl = placed.at(c.name);
    (void)spec->GetModule(pl.module)->GetChannelInformation(pl.channel, info);
    std::fprintf(log, "[%s] %zu rows, code %u/%u B, data %u/%u B\n",
                 c.name.c_str(), c.rows, code, info.engineInstructionMemorySize,
                 data, info.engineDataMemorySize);
  }

  // Data output: <prefix>_0.cfl (vendor naming) or stdout.
  std::FILE* data = nullptr;
  if (o.dataToStdout) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    data = stdout;
  } else if (!o.outputPrefix.empty()) {
    const std::string path = o.outputPrefix + "_0.cfl";
    data = std::fopen(path.c_str(), "wb");
    if (!data) {
      std::fprintf(log, "error: cannot write %s\n", path.c_str());
      spec->DeleteSequenceRunner("pulseq_plato");
      return 1;
    }
  }
  CflWriter writer(data);
  mp::AcquisitionDataPump pump(&writer);
  std::thread pumpThread;
  const bool hasRx = o.expectedSamples > 0;
  if (hasRx) {
    auto src = runner->GetAcquisitionDataSource(realtime::RoleName(realtime::Role::kRx));
    if (!src || pump.Connect(*src, 0) !=
                    mp::AcquisitionDataPump::ConnectResult::kSuccess) {
      std::fprintf(log, "error: cannot connect the acquisition data pump\n");
      spec->DeleteSequenceRunner("pulseq_plato");
      return 1;
    }
    pumpThread = std::thread([&] { pump.Run(); });
  }

  int rc = 0;
  if (runner->Run() != mp::SequenceRuntimeResult::kSuccess) {
    std::fprintf(log, "error: Run failed\n");
    rc = 1;
  }
  std::fprintf(log, "sequence running (%.3f s)\n", o.sequenceSeconds);

  // Wait for every component to exit. kCommunicationFailure: the simulator
  // reached --until and closed its sockets (FINDINGS T5): nothing more runs.
  const double timeout =
      o.timeoutSeconds > 0 ? o.timeoutSeconds : o.sequenceSeconds + 10;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration<double>(timeout);
  bool allDone = rc != 0, gone = false;
  while (!allDone && std::chrono::steady_clock::now() < deadline) {
    allDone = true;
    for (const auto& c : program.components) {
      bool running = true;
      std::uint16_t status = 0;
      const auto q = runner->GetExitStatus(c.name, running, status);
      if (q == mp::SequenceRuntimeResult::kCommunicationFailure) {
        gone = true;
        running = false;
      } else if (q != mp::SequenceRuntimeResult::kSuccess) {
        running = true;
      }
      allDone = allDone && !running;
    }
    if (!allDone) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!allDone) {
    std::fprintf(log, "error: timeout after %.1f s waiting for the sequence\n",
                 timeout);
    (void)runner->Stop();
    rc = 1;
  }

  for (const auto& c : program.components) {
    bool running = true;
    std::uint16_t status = 0;
    const auto q = runner->GetExitStatus(c.name, running, status);
    mp::ModuleChannelStatus st{};
    const auto& pl = placed.at(c.name);
    (void)spec->GetModule(pl.module)->GetChannelStatus(pl.channel, st);
    if (q == mp::SequenceRuntimeResult::kSuccess) {
      std::fprintf(log, "[%s] exit %u%s%s%s\n", c.name.c_str(), status,
                   st.engineTimingViolation ? " TIMING-VIOLATION" : "",
                   st.engineDivisionByZero ? " DIV0" : "",
                   st.gradientOverrun ? " GRADIENT-OVERRUN" : "");
      if (status != 0 || st.engineTimingViolation || st.engineDivisionByZero ||
          st.gradientOverrun)
        rc = 1;
    } else {
      std::fprintf(log, "[%s] exit status unavailable (%d)\n", c.name.c_str(),
                   static_cast<int>(q));
    }
  }
  if (gone)
    std::fprintf(log, "note: the spectrometer closed the connection (the "
                      "simulator exits once every channel has exited, or at "
                      "--until)\n");

  if (hasRx) {
    const bool got = writer.WaitFor(
        o.expectedSamples,
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    pump.Stop();
    if (pumpThread.joinable()) pumpThread.join();
    std::fprintf(log, "acquired %llu of %llu samples\n",
                 static_cast<unsigned long long>(writer.samples()),
                 static_cast<unsigned long long>(o.expectedSamples));
    if (!got || writer.incomplete()) {
      std::fprintf(log, "error: acquisition data incomplete\n");
      rc = 1;
    }
  }
  if (data && data != stdout) std::fclose(data);
  spec->DeleteSequenceRunner("pulseq_plato");
  return rc;
}

}  // namespace pp::plato
