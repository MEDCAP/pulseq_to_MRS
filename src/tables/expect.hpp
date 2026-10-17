// Expected simulator log times of every row (for test/compare.py expect), so
// lateness can be checked from the log: the simulator never sets
// engineTimingViolation (FINDINGS T2).
#pragma once

#include "tables/channel_table.hpp"

#include <string>

namespace pp::tables {

// Writes one line per checked event:
//   <log file index> <command> <logged time> <allowed lateness> <row>
// Returns false if the file cannot be written.
bool WriteExpect(const std::string& path, const ChannelTables& t,
                 const ScanConfig& cfg);

}  // namespace pp::tables
