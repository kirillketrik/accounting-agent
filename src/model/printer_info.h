#pragma once
#include <string>
#include <cstdint>

enum class PortType {
    TcpIp,     // Win32_TCPIPPrinterPort, real IP/host known
    Wsd,       // WSD-xxxx port, host/IP parsed out of Location
    LocalUsb,  // USB*, COM*, LPT*
    Network,   // not installed on this PC at all - found by network discovery (SNMP sweep)
    Other      // anything else (file ports, redirected ports, unknown)
};

inline const char* PortTypeName(PortType t) {
    switch (t) {
        case PortType::TcpIp:    return "TCP/IP";
        case PortType::Wsd:      return "WSD";
        case PortType::LocalUsb: return "Local/USB";
        case PortType::Network:  return "Network";
        case PortType::Other:    return "Other";
    }
    return "Other";
}

enum class PageCountSource {
    None,
    Snmp,
    RegistryDriverData,
    SpoolerCounter,
    Pjl,
};

inline const char* PageCountSourceName(PageCountSource s) {
    switch (s) {
        case PageCountSource::None:               return "none";
        case PageCountSource::Snmp:                return "snmp";
        case PageCountSource::RegistryDriverData:  return "registry:PrinterDriverData";
        case PageCountSource::SpoolerCounter:      return "wmi:PrintQueue.TotalPagesPrinted";
        case PageCountSource::Pjl:                 return "pjl:INFO_PAGECOUNT";
    }
    return "none";
}

// Reachability status, tracked across cycles (see LivenessTracker) so a single
// blip doesn't flip a printer offline - only `offlineAfterConsecutiveFailures`
// consecutive bad cycles do. `Unknown` means no reachability signal exists for
// this port type (PortType::Other) - it never counts toward offline detection.
enum class PrinterStatus {
    Online,
    Offline,
    Unknown,
};

inline const char* PrinterStatusName(PrinterStatus s) {
    switch (s) {
        case PrinterStatus::Online:  return "online";
        case PrinterStatus::Offline: return "offline";
        case PrinterStatus::Unknown: return "unknown";
    }
    return "unknown";
}

// Everything collected about a single installed print queue (WMI Win32_Printer
// instance), or about a printer network discovery found (PortType::Network,
// where the WMI fields are filled in from SNMP instead). One physical printer
// can legitimately produce two of these (e.g. installed once via WSD and once
// via a direct TCP/IP port) - the agent reports both as-is; de-duplication is
// the server's job.
struct PrinterInfo {
    // Raw WMI fields.
    std::string name;
    std::string driverName;
    std::string portName;
    std::string location;
    bool shared = false;
    std::string shareName;
    bool network = false;
    bool workOffline = false;
    uint32_t printerStatus = 0;

    // Derived.
    PortType portType = PortType::Other;

    // Resolved network endpoint, when applicable (TCP/IP, WSD or Network only).
    bool hostResolved = false;
    std::string resolvedHost;   // literal IPv4/IPv6 address, or a hostname we could not resolve
    bool isIPv6 = false;
    uint32_t ipv6ScopeId = 0;   // zone id, for link-local IPv6 (WSD) addresses

    // Page counter, from whichever source applied.
    PageCountSource pageCountSource = PageCountSource::None;
    int64_t pageCount = -1;

    // SNMP diagnostics.
    bool snmpAttempted = false;
    bool snmpReachable = false;
    std::string snmpSysDescr;

    // Reachability, tracked across cycles by LivenessTracker (see
    // collector/liveness_tracker.h). `status` only ever becomes Offline after
    // several consecutive bad cycles, not on a single blip.
    PrinterStatus status = PrinterStatus::Unknown;
    int consecutiveFailures = 0;

    // Free-form note for anything that couldn't be determined, e.g.
    // "could not determine IP: mDNS/.local hostname not resolvable".
    std::string note;
};
