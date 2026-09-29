#pragma once
#include <string>
#include <vector>
#include "common/logger.h"

struct HostAddress {
    std::string ip;       // literal address (dotted IPv4 or colon-hex IPv6)
    bool isIPv6 = false;
    std::string adapterName;
    // On-link prefix length (e.g. 24 for a 255.255.255.0 mask); used by
    // network discovery to know which subnet to sweep. Not sent to the server.
    int prefixLength = 0;
};

// Enumerates this machine's own IP addresses across all active, non-loopback
// network interfaces (per the brief: "all active network interfaces,
// excluding loopback"). Never throws; returns an empty vector on failure
// (logged internally).
std::vector<HostAddress> GetHostIpAddresses(Logger& logger);

// Best-effort local computer name (NetBIOS/DNS hostname), UTF-8.
std::string GetLocalComputerName();
