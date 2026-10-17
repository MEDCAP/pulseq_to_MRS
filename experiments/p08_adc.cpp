// Probe 8: receiver timing and the host data path.
//
// Rx:  setfreq(0); acquire2(16, DWELL, GRID, 20000); acquire2(8, DWELL, GRID, 60000)
// with the Transparent FIR (cic off, GRID = DWELL) or the CicCompensation FIR
// (cic on, GRID = DWELL / 4 - the FIR decimates by 4).
//
// Questions:
//   - where do the ACQ sample lines land relative to `at` (first sample at
//     at + DWELL per the header doc, + engine latency)?
//   - what does the pump deliver: bytes per acquisition (16 samples * 6 bytes?),
//     fragment offsets, `last` flag, acquisitionId numbering?
// Usage: p08_adc [on|off] [DWELL]
#include "probe_common.hpp"

#include <acquisition_data_pump.hpp>

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace {

class Counter : public mrs::plato::AcquisitionDataPump::Delegate {
public:
  void AcquisitionDataPump_Submit(mrs::plato::AcquisitionDataPump*,
                                  unsigned channel, std::uint32_t acqId,
                                  std::uint32_t fragmentOffset, bool last,
                                  const void* payload,
                                  std::size_t size) override {
    std::lock_guard<std::mutex> lock(m_);
    bytes_[acqId] += size;
    auto* b = static_cast<const std::uint8_t*>(payload);
    // First sample of each fragment: 24-bit big-endian I then Q.
    auto s24 = [](const std::uint8_t* p) {
      std::int32_t v = (p[0] << 16) | (p[1] << 8) | p[2];
      return (v & 0x800000) ? v - 0x1000000 : v;
    };
    std::printf("pump: ch=%u acq=%u frag=%u last=%d size=%zu firstIQ=(%d,%d)\n",
                channel, acqId, fragmentOffset, last, size,
                size >= 6 ? s24(b) : 0, size >= 6 ? s24(b + 3) : 0);
  }
  void Print() {
    std::lock_guard<std::mutex> lock(m_);
    for (auto [id, n] : bytes_)
      std::printf("acq %u: %zu bytes = %zu samples\n", id, n, n / 6);
  }

private:
  std::mutex m_;
  std::map<std::uint32_t, std::size_t> bytes_;
};

}  // namespace

int main(int argc, char** argv) {
  using namespace mrs::plato;
  using namespace mrs::plato::ppl;
  const bool cic = argc > 1 && std::strcmp(argv[1], "on") == 0;
  const int dwell = argc > 2 ? std::atoi(argv[2]) : 1000;
  const int grid = cic ? dwell / 4 : dwell;

  auto body = _function(
      setfreq(0),
      acquire2(16, dwell, grid, 20000),
      acquire2(8, dwell, grid, 60000),
      ppl::exit(0));
  auto rx = cic ? _component("Rx", body, _filter::CicCompensation())
                : _component("Rx", body, _filter::Transparent());
  _sequence seq(std::move(rx));

  std::printf("cic=%d dwell=%d grid=%d\n", cic, dwell, grid);
  Counter counter;
  AcquisitionDataPump pump(&counter);
  std::thread worker;
  probe::Session s;
  const int rc = s.Run(
      seq, {probe::Rx()},
      [&](SequenceRunner& r) {
        auto src = r.GetAcquisitionDataSource("Rx");
        if (!src || pump.Connect(*src, 0) !=
                        AcquisitionDataPump::ConnectResult::kSuccess) {
          std::fprintf(stderr, "pump connect failed\n");
          return;
        }
        worker = std::thread([&] { pump.Run(); });
      },
      [&](SequenceRunner&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        pump.Stop();
        if (worker.joinable()) worker.join();
      });
  counter.Print();
  return rc;
}
