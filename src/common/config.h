#pragma once
#include <string>
#include <cstdint>
#include "logger.h"

// Agent configuration, loaded from a simple INI file next to the executable
// (agent.ini). Every field has a sensible default so the service still runs
// (in a degraded/logging-only fashion) if the file is missing or partial.
struct AgentConfig {
    // [server]
    std::string serverUrl = "http://localhost:8080/api/printer-reports";
    std::string authToken;               // sent as "Authorization: Bearer <token>" when non-empty
    int httpTimeoutMs = 15000;

    // [polling]
    int intervalSeconds = 3600;          // how often a full collection+send cycle runs
    // A printer only flips to "offline" after this many consecutive bad
    // cycles in a row (SNMP unreachable, or WMI PrinterStatus==Offline for
    // local/USB) - a single blip doesn't count, mirroring the websocket
    // burst-retry pattern below.
    int offlineAfterConsecutiveFailures = 3;

    // [websocket]
    // Optional persistent control channel (ws:// or wss://). When enabled,
    // the agent holds this connection open and treats a recognized "send
    // report now" text message from the server as a trigger to run an
    // immediate collection+send cycle, independent of interval_seconds. Not
    // required for normal operation - the agent works fine on polling alone
    // when this is disabled or unset.
    bool wsEnabled = false;
    std::string wsUrl;                   // e.g. wss://your-server.local:8080/api/agent-ws
    // Reconnect pattern: retry every wsRetryIntervalMs; after
    // wsMaxRetriesBeforeRelax consecutive failed attempts, back off for
    // wsRelaxDelayMs before resuming the fast retries.
    int wsRetryIntervalMs = 2000;
    int wsMaxRetriesBeforeRelax = 15;
    int wsRelaxDelayMs = 60000;

    // [snmp]
    std::string snmpCommunity = "public";
    int snmpTimeoutMs = 2000;            // per-attempt timeout
    int snmpRetries = 3;                 // total attempts
    int snmpRetryDelayMs = 500;
    uint16_t snmpPort = 161;

    // [pjl]
    // Optional highest-priority USB/local page-count source: sends a tiny raw
    // PJL status query print job to the device and reads its reply for the
    // printer's own true lifetime page count (unlike the WMI spooler counter,
    // which resets to 0 whenever the Print Spooler service restarts). OFF by
    // default: a printer that does not understand PJL could print a garbled
    // or blank page in response to this query. Only enable after confirming
    // (e.g. by watching the printer physically during one test cycle) that a
    // given fleet's printers handle it safely.
    bool pjlEnabled = false;
    int pjlTimeoutMs = 5000;

    // [logging]
    LogLevel logLevel = LogLevel::Info;
    std::string logFileName = "agent.log"; // resolved next to the executable
    uint64_t logMaxSizeBytes = 5ull * 1024 * 1024;
    int logMaxBackups = 5;

    // Load from an INI file. Returns false (and logs nothing itself - caller
    // decides how to report it, since the logger may not exist yet) if the
    // file could not be opened; `outConfig` is still populated with defaults.
    static bool LoadFromFile(const std::wstring& path, AgentConfig& outConfig, std::string* errorOut = nullptr);
};

// Resolves a path relative to the running executable's directory (used for
// the default config file location and the default log file location).
std::wstring GetExecutableDir();
