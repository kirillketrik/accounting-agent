#pragma once
#include <string>
#include <cstdint>
#include "common/logger.h"

// Well-known Printer-MIB OIDs used by this agent (validated against a real
// Kyocera ECOSYS M2235dn per the brief).
namespace snmp_oids {
    constexpr const char* kSysDescr = "1.3.6.1.2.1.1.1.0";
    constexpr const char* kPrtMarkerLifeCount = "1.3.6.1.2.1.43.10.2.1.4.1.1";
    // Column (table entry, no fixed index) for the GETNEXT-based walk used as
    // a fallback when the device doesn't have a marker at index "1.1" (e.g.
    // per-color-plane markers indexed differently) - see snmp_client.cpp.
    constexpr const char* kPrtMarkerLifeCountColumn = "1.3.6.1.2.1.43.10.2.1.4";
}

struct SnmpTarget {
    std::string host;      // literal IP (v4/v6, zone stripped) or a hostname to resolve
    bool isIPv6 = false;
    uint32_t scopeId = 0;  // IPv6 zone id, for link-local WSD addresses
    uint16_t port = 161;
};

struct SnmpOptions {
    std::string community = "public";
    int timeoutMs = 2000;     // per-attempt timeout
    int retries = 3;          // total attempts (not extra retries on top of 1)
    int retryDelayMs = 500;

    // Optional service stop event. When set, the retry-delay waits are
    // interruptible (WaitForSingleObject instead of Sleep) so a shutdown
    // request doesn't have to wait out a full SNMP retry cycle. May be null.
    HANDLE stopEvent = nullptr;
};

struct SnmpGetResult {
    bool hostResolved = true;   // false only if the host/hostname could not be resolved at all
    bool success = false;       // true once a well-formed GetResponse was received and parsed
    std::string sysDescr;
    bool hasPageCount = false;
    int64_t pageCount = 0;
    std::string error;          // set whenever success/hostResolved is false, for logging
};

// Must be called once (e.g. at service startup) before any SnmpGetSysDescrAndPageCount
// call, and SnmpGlobalCleanup() once at shutdown. Wraps WSAStartup/WSACleanup.
bool SnmpGlobalInit(Logger& logger);
void SnmpGlobalCleanup();

// Performs an SNMP v1 GET for sysDescr + prtMarkerLifeCount against `target`,
// with retries/timeout per `options`. Never throws; all failure modes are
// reported via the returned struct and logged at an appropriate level.
SnmpGetResult SnmpGetSysDescrAndPageCount(
    const SnmpTarget& target,
    const SnmpOptions& options,
    Logger& logger,
    const std::string& printerNameForLogging);
