#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "common/logger.h"

// One value found under PrinterDriverData whose name looked like a counter.
struct RegistryCounterValue {
    std::string name;   // e.g. "HPMediaCount"
    int64_t value = 0;
};

struct RegistryCounterResult {
    bool keyFound = false;
    std::vector<RegistryCounterValue> counters; // in enumeration order
};

// Best-effort read of
//   HKLM\SYSTEM\CurrentControlSet\Control\Print\Printers\<printerName>\PrinterDriverData
// looking for values whose name contains "count", "page", or "counter"
// (case-insensitive). This is vendor-specific and undocumented - confirmed
// working for HP (HPMediaCount, HPTrayCount reflect real device state, not a
// per-queue count) per the brief; other vendors may have nothing usable here.
// Never throws; missing key/values simply yield an empty result.
RegistryCounterResult ReadPrinterDriverDataCounters(const std::string& printerName, Logger& logger);
