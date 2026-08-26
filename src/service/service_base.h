#pragma once
#include <windows.h>
#include <string>
#include <atomic>

// SCM registration/install/uninstall plus the worker loop that periodically
// runs a collection+send cycle. main.cpp picks one of RunAsService (normal
// deployment, invoked with no arguments by the SCM) or RunInConsole
// (interactive debugging, e.g. running/debugging directly from CLion).
class PrinterAgentService {
public:
    static const wchar_t* ServiceName();
    static const wchar_t* ServiceDisplayName();

    // Registers with the SCM and blocks until the service stops. Must be
    // called from the thread that invoked StartServiceCtrlDispatcher (i.e.
    // straight from main() with no prior console/thread setup).
    static void RunAsService();

    // Runs the same worker loop directly in the current process, blocking
    // until Ctrl+C / console close. For local development only.
    static void RunInConsole();

    static bool Install(std::wstring* errorOut);
    static bool Uninstall(std::wstring* errorOut);

private:
    static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
    static DWORD WINAPI ServiceCtrlHandlerEx(DWORD ctrl, DWORD eventType, LPVOID eventData, LPVOID context);
    static void ReportStatus(DWORD currentState, DWORD exitCode, DWORD waitHint);

    static SERVICE_STATUS_HANDLE statusHandle_;
    static SERVICE_STATUS status_;
    // Atomic so ServiceCtrlHandlerEx (called on an SCM-owned thread) can
    // safely read the handle while ServiceMain's thread creates/closes it.
    static std::atomic<HANDLE> stopEvent_;
};
