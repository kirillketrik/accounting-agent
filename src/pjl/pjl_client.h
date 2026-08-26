#pragma once
#include <string>
#include <cstdint>
#include "common/logger.h"

// Queries a locally-installed printer's TRUE lifetime page count via a raw
// PJL (Printer Job Language) status request, sent as a print job through the
// standard Windows spooler bidirectional I/O path (OpenPrinter/WritePrinter/
// ReadPrinter - available since Windows 2000, no Win7 compatibility concern).
//
// This is NOT the same as the WMI Win32_PerfRawData_Spooler_PrintQueue
// counter used elsewhere in this codebase: that one resets to 0 whenever the
// Print Spooler service restarts, while a PJL PAGECOUNT reply comes from the
// device's own firmware/NVRAM and survives spooler restarts, driver
// reinstalls, etc.
//
// Caution (see agent.ini [pjl] comment): this submits a real, tiny print job.
// A printer that does not understand PJL may print a garbled or blank page in
// response instead of silently ignoring it. Callers must only invoke this
// when explicitly enabled by configuration - never unconditionally.
//
// Never throws. Returns false (and logs why at DEBUG) if the printer could
// not be opened, the job could not be submitted, no reply arrived within
// `timeoutMs`, or the reply didn't contain a parseable page count.
bool TryGetPjlPageCount(const std::string& printerName, int timeoutMs, int64_t& outPages, Logger& logger);
