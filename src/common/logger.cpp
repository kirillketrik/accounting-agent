#include "logger.h"
#include "string_utils.h"
#include <cstdio>
#include <ctime>

LogLevel ParseLogLevel(const std::string& s, LogLevel fallback) {
    std::string u = strutil::ToLower(s);
    if (u == "debug") return LogLevel::Debug;
    if (u == "info") return LogLevel::Info;
    if (u == "warn" || u == "warning") return LogLevel::Warn;
    if (u == "error") return LogLevel::Error;
    return fallback;
}

static const char* LevelName(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseHandle_NoLock();
}

bool Logger::Init(const std::wstring& filePath, LogLevel minLevel, uint64_t maxSizeBytes, int maxBackups) {
    std::lock_guard<std::mutex> lock(mutex_);
    filePath_ = filePath;
    minLevel_ = minLevel;
    maxSizeBytes_ = maxSizeBytes;
    maxBackups_ = maxBackups;
    OpenForAppend_NoLock();
    initialized_ = (fileHandle_ != INVALID_HANDLE_VALUE);
    return initialized_;
}

void Logger::OpenForAppend_NoLock() {
    CloseHandle_NoLock();
    fileHandle_ = CreateFileW(
        filePath_.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ, // allow other tools (tail, log viewers) to read concurrently
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
}

void Logger::CloseHandle_NoLock() {
    if (fileHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(fileHandle_);
        fileHandle_ = INVALID_HANDLE_VALUE;
    }
}

void Logger::RotateIfNeeded_NoLock() {
    if (fileHandle_ == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(fileHandle_, &size)) return;
    if ((uint64_t)size.QuadPart < maxSizeBytes_) return;

    CloseHandle_NoLock();

    // Shift agent.(N-1).log -> agent.N.log ... agent.log -> agent.1.log
    auto backupPath = [this](int n) {
        std::wstring p = filePath_;
        // Only split on a dot within the file name itself - a dot in a
        // parent directory (e.g. "C:\Tools\agent v1.2\agent-log") must not
        // be mistaken for the extension separator.
        size_t lastSep = p.find_last_of(L"\\/");
        size_t searchFrom = (lastSep == std::wstring::npos) ? 0 : lastSep + 1;
        size_t dot = p.find_last_of(L'.');
        if (dot != std::wstring::npos && dot < searchFrom) dot = std::wstring::npos;
        std::wstring base = (dot == std::wstring::npos) ? p : p.substr(0, dot);
        std::wstring ext = (dot == std::wstring::npos) ? L"" : p.substr(dot);
        return base + L"." + std::to_wstring(n) + ext;
    };

    if (maxBackups_ > 0) {
        std::wstring oldest = backupPath(maxBackups_);
        DeleteFileW(oldest.c_str());
        for (int n = maxBackups_ - 1; n >= 1; n--) {
            std::wstring from = backupPath(n);
            std::wstring to = backupPath(n + 1);
            MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        MoveFileExW(filePath_.c_str(), backupPath(1).c_str(), MOVEFILE_REPLACE_EXISTING);
    } else {
        DeleteFileW(filePath_.c_str());
    }

    OpenForAppend_NoLock();
}

void Logger::Log(LogLevel level, const std::string& message) {
    if (level < minLevel_) return;

    std::lock_guard<std::mutex> lock(mutex_);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::string line = std::string("[") + timeBuf + "] [" + LevelName(level) + "] " + message + "\r\n";

    // Always mirror to the debugger / console so `console` mode and
    // `DebugView` both work without touching the log file.
    OutputDebugStringA(line.c_str());

    if (!initialized_ || fileHandle_ == INVALID_HANDLE_VALUE) return;

    RotateIfNeeded_NoLock();
    if (fileHandle_ == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    WriteFile(fileHandle_, line.data(), (DWORD)line.size(), &written, nullptr);
}
