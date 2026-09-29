#include "config.h"
#include "string_utils.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <map>

std::wstring GetExecutableDir() {
    wchar_t path[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return L".";
    std::wstring full(path, len);
    size_t pos = full.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return full.substr(0, pos);
}

namespace {

// section -> key -> value, all lowercased keys/sections, values as-is (trimmed).
using IniData = std::map<std::string, std::map<std::string, std::string>>;

bool ParseIni(const std::wstring& path, IniData& out) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file.is_open()) return false;

    // Skip a UTF-8 BOM if present - otherwise it sticks to the front of the
    // first "[section]" line, the '[' check fails, and every key in that
    // section silently falls back to its default with no error reported.
    {
        char bom[3] = {0};
        file.read(bom, 3);
        bool hasBom = file.gcount() == 3 &&
            (unsigned char)bom[0] == 0xEF && (unsigned char)bom[1] == 0xBB && (unsigned char)bom[2] == 0xBF;
        if (!hasBom) {
            file.clear();
            file.seekg(0);
        }
    }

    std::string line;
    std::string currentSection;
    while (std::getline(file, line)) {
        // Strip trailing CR for CRLF files.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = strutil::Trim(line);
        if (trimmed.empty()) continue;
        if (trimmed[0] == ';' || trimmed[0] == '#') continue;

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            currentSection = strutil::ToLower(strutil::Trim(trimmed.substr(1, trimmed.size() - 2)));
            continue;
        }

        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = strutil::ToLower(strutil::Trim(trimmed.substr(0, eq)));
        std::string value = strutil::Trim(trimmed.substr(eq + 1));
        out[currentSection][key] = value;
    }
    return true;
}

std::string GetStr(const IniData& ini, const std::string& section, const std::string& key, const std::string& fallback) {
    auto sIt = ini.find(section);
    if (sIt == ini.end()) return fallback;
    auto kIt = sIt->second.find(key);
    if (kIt == sIt->second.end() || kIt->second.empty()) return fallback;
    return kIt->second;
}

int GetInt(const IniData& ini, const std::string& section, const std::string& key, int fallback) {
    std::string s = GetStr(ini, section, key, "");
    if (s.empty()) return fallback;
    try {
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
}

uint64_t GetU64(const IniData& ini, const std::string& section, const std::string& key, uint64_t fallback) {
    std::string s = GetStr(ini, section, key, "");
    if (s.empty()) return fallback;
    try {
        return std::stoull(s);
    } catch (...) {
        return fallback;
    }
}

bool GetBool(const IniData& ini, const std::string& section, const std::string& key, bool fallback) {
    std::string s = strutil::ToLower(GetStr(ini, section, key, ""));
    if (s.empty()) return fallback;
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

} // namespace

bool AgentConfig::LoadFromFile(const std::wstring& path, AgentConfig& cfg, std::string* errorOut) {
    IniData ini;
    if (!ParseIni(path, ini)) {
        if (errorOut) *errorOut = "config file not found or unreadable: " + strutil::WideToUtf8(path);
        return false;
    }

    cfg.serverUrl = GetStr(ini, "server", "url", cfg.serverUrl);
    cfg.authToken = GetStr(ini, "server", "auth_token", cfg.authToken);
    cfg.httpTimeoutMs = GetInt(ini, "server", "timeout_ms", cfg.httpTimeoutMs);

    cfg.intervalSeconds = GetInt(ini, "polling", "interval_seconds", cfg.intervalSeconds);
    if (cfg.intervalSeconds < 10) cfg.intervalSeconds = 10; // sanity floor
    const int kMaxIntervalSeconds = 30 * 24 * 3600; // 30 days
    if (cfg.intervalSeconds > kMaxIntervalSeconds) cfg.intervalSeconds = kMaxIntervalSeconds;
    cfg.offlineAfterConsecutiveFailures = GetInt(ini, "polling", "offline_after_consecutive_failures",
        cfg.offlineAfterConsecutiveFailures);
    if (cfg.offlineAfterConsecutiveFailures < 1) cfg.offlineAfterConsecutiveFailures = 1;

    cfg.wsEnabled = GetBool(ini, "websocket", "enabled", cfg.wsEnabled);
    cfg.wsUrl = GetStr(ini, "websocket", "url", cfg.wsUrl);
    cfg.wsRetryIntervalMs = GetInt(ini, "websocket", "retry_interval_ms", cfg.wsRetryIntervalMs);
    if (cfg.wsRetryIntervalMs < 250) cfg.wsRetryIntervalMs = 250; // sanity floor
    cfg.wsMaxRetriesBeforeRelax = GetInt(ini, "websocket", "max_retries_before_relax", cfg.wsMaxRetriesBeforeRelax);
    if (cfg.wsMaxRetriesBeforeRelax < 1) cfg.wsMaxRetriesBeforeRelax = 1;
    cfg.wsRelaxDelayMs = GetInt(ini, "websocket", "relax_delay_ms", cfg.wsRelaxDelayMs);
    if (cfg.wsRelaxDelayMs < 1000) cfg.wsRelaxDelayMs = 1000; // sanity floor

    cfg.snmpCommunity = GetStr(ini, "snmp", "community", cfg.snmpCommunity);
    cfg.snmpTimeoutMs = GetInt(ini, "snmp", "timeout_ms", cfg.snmpTimeoutMs);
    cfg.snmpRetries = GetInt(ini, "snmp", "retries", cfg.snmpRetries);
    cfg.snmpRetryDelayMs = GetInt(ini, "snmp", "retry_delay_ms", cfg.snmpRetryDelayMs);
    cfg.snmpPort = (uint16_t)GetInt(ini, "snmp", "port", cfg.snmpPort);

    cfg.discoveryEnabled = GetBool(ini, "discovery", "enabled", cfg.discoveryEnabled);
    cfg.discoveryScanLocalSubnets = GetBool(ini, "discovery", "scan_local_subnets", cfg.discoveryScanLocalSubnets);
    cfg.discoverySubnets = GetStr(ini, "discovery", "subnets", cfg.discoverySubnets);
    cfg.discoveryMinPrefixLength = GetInt(ini, "discovery", "min_prefix_length", cfg.discoveryMinPrefixLength);
    if (cfg.discoveryMinPrefixLength < 16) cfg.discoveryMinPrefixLength = 16; // a /16 is already 65k addresses
    if (cfg.discoveryMinPrefixLength > 30) cfg.discoveryMinPrefixLength = 30;
    cfg.discoveryMaxHosts = GetInt(ini, "discovery", "max_hosts", cfg.discoveryMaxHosts);
    if (cfg.discoveryMaxHosts < 1) cfg.discoveryMaxHosts = 1;
    if (cfg.discoveryMaxHosts > 65536) cfg.discoveryMaxHosts = 65536;
    cfg.discoveryTimeoutMs = GetInt(ini, "discovery", "timeout_ms", cfg.discoveryTimeoutMs);
    if (cfg.discoveryTimeoutMs < 200) cfg.discoveryTimeoutMs = 200;
    cfg.discoveryRetries = GetInt(ini, "discovery", "retries", cfg.discoveryRetries);
    if (cfg.discoveryRetries < 1) cfg.discoveryRetries = 1;

    cfg.pjlEnabled = GetBool(ini, "pjl", "enabled", cfg.pjlEnabled);
    cfg.pjlTimeoutMs = GetInt(ini, "pjl", "timeout_ms", cfg.pjlTimeoutMs);
    if (cfg.pjlTimeoutMs < 500) cfg.pjlTimeoutMs = 500;

    cfg.logLevel = ParseLogLevel(GetStr(ini, "logging", "level", "info"), LogLevel::Info);
    cfg.logFileName = GetStr(ini, "logging", "file", cfg.logFileName);
    cfg.logMaxSizeBytes = GetU64(ini, "logging", "max_size_kb", cfg.logMaxSizeBytes / 1024) * 1024;
    cfg.logMaxBackups = GetInt(ini, "logging", "max_files", cfg.logMaxBackups);

    return true;
}
