// Waveform export for test/plot_waveforms.py: every sample the tables and
// libraries will play, back in physical units on the sequence timeline.
#pragma once

#include "tables/channel_table.hpp"

#include <string>

namespace pp::tables {

// CSV lines "channel,t_s,value,extra" with t_s in seconds from sequence t=0:
//   rf      sample centre   |B1| Hz        phase rad (carrier-referenced)
//   gate    gate edge       1 / 0
//   gx/gy/gz raster edge    Hz/m
//   adc     sample centre   freq offset Hz  phase rad (row phase word)
// Returns false if the file can't be written.
bool WriteWaveforms(const std::string& path, const ChannelTables& t,
                    const ScanConfig& cfg);

}  // namespace pp::tables
