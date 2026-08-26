#pragma once
#include <string>
#include <mutex>
#include <cstdint>
#include <windows.h>

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

LogLevel ParseLogLevel(const std::string& s, LogLevel fallback = LogLevel::Info);

// Thread-safe file logger with size-based rotation (app.log -> app.1.log -> ...).
// Uses Win32 file APIs directly (not iostream) so behavior is well-defined for
// wide (unicode) paths on every supported OS version, including Windows 7.
class Logger {
public:
    Logger() = default;
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // filePath: full path to the active log file, e.g. C:\...\agent.log
    // maxSizeBytes: rotate once the file would exceed this size.
    // maxBackups: how many rotated files (agent.1.log .. agent.N.log) to keep.
    bool Init(const std::wstring& filePath, LogLevel minLevel, uint64_t maxSizeBytes, int maxBackups);

    void Log(LogLevel level, const std::string& message);
    void Debug(const std::string& m) { Log(LogLevel::Debug, m); }
    void Info(const std::string& m) { Log(LogLevel::Info, m); }
    void Warn(const std::string& m) { Log(LogLevel::Warn, m); }
    void Error(const std::string& m) { Log(LogLevel::Error, m); }

private:
    void RotateIfNeeded_NoLock();
    void OpenForAppend_NoLock();
    void CloseHandle_NoLock();

    std::mutex mutex_;
    std::wstring filePath_;
    LogLevel minLevel_ = LogLevel::Info;
    uint64_t maxSizeBytes_ = 5ull * 1024 * 1024;
    int maxBackups_ = 5;
    HANDLE fileHandle_ = INVALID_HANDLE_VALUE;
    bool initialized_ = false;
};
