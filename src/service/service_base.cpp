#include "service_base.h"
#include "common/config.h"
#include "common/logger.h"
#include "common/string_utils.h"
#include "snmp/snmp_client.h"
#include "collector/data_collector.h"
#include "ws/ws_client.h"
#include <cstdio>
#include <thread>

SERVICE_STATUS_HANDLE PrinterAgentService::statusHandle_ = nullptr;
SERVICE_STATUS PrinterAgentService::status_ = {};
std::atomic<HANDLE> PrinterAgentService::stopEvent_{nullptr};

const wchar_t* PrinterAgentService::ServiceName() { return L"PrinterInventoryAgent"; }
const wchar_t* PrinterAgentService::ServiceDisplayName() { return L"Printer Inventory Agent"; }

namespace {

// Shared by both service mode and console mode: load config, init logger,
// init SNMP, then loop RunCollectionCycle() until `stopEvent` is signaled.
void WorkerLoop(HANDLE stopEvent) {
    std::wstring exeDir = GetExecutableDir();
    std::wstring configPath = exeDir + L"\\agent.ini";

    AgentConfig config;
    std::string configError;
    bool configLoaded = AgentConfig::LoadFromFile(configPath, config, &configError);

    bool logPathIsAbsolute = config.logFileName.size() > 1 &&
        (config.logFileName[1] == ':' || (config.logFileName[0] == '\\' && config.logFileName[1] == '\\'));
    std::wstring logPath = logPathIsAbsolute
        ? strutil::Utf8ToWide(config.logFileName)
        : exeDir + L"\\" + strutil::Utf8ToWide(config.logFileName);

    Logger logger;
    logger.Init(logPath, config.logLevel, config.logMaxSizeBytes, config.logMaxBackups);

    logger.Info("=== Printer Inventory Agent starting ===");
    if (!configLoaded) {
        logger.Warn("Config: " + configError + " - using built-in defaults.");
    }

    if (!SnmpGlobalInit(logger)) {
        logger.Error("SNMP subsystem failed to initialize; network printer page counts will be unavailable this run.");
    }

    // Auto-reset: one signal from the server wakes the loop for exactly one
    // extra out-of-cycle collection, then goes back to waiting.
    HANDLE wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wakeEvent) {
        logger.Warn("CreateEventW for the websocket wake event failed, error=" + std::to_string(GetLastError()) +
            "; the websocket signal channel will be unavailable this run.");
    }

    std::thread wsThread;
    bool wsStarted = false;
    std::atomic<HINTERNET> activeWsSocket{nullptr};

    // Owned here (not inside RunCollectionCycle) so consecutive-failure
    // counts and "last known printers" survive across cycles for the
    // lifetime of this process run, resetting only on a service restart.
    LivenessTracker livenessTracker;
    if (wakeEvent && config.wsEnabled) {
        if (config.wsUrl.empty()) {
            logger.Warn("WebSocket: enabled in config but websocket.url is empty; skipping.");
        } else {
            wsThread = std::thread(RunWebSocketLoop, std::cref(config), std::ref(logger), stopEvent, wakeEvent,
                                   &activeWsSocket);
            wsStarted = true;
        }
    }

    for (;;) {
        RunCollectionCycle(config, logger, livenessTracker, stopEvent);

        // int64_t multiply-then-clamp avoids signed 32-bit overflow for a
        // pathological interval_seconds value in agent.ini.
        int64_t waitMs64 = (int64_t)config.intervalSeconds * 1000;
        DWORD waitMs = (waitMs64 > (int64_t)UINT32_MAX) ? (UINT32_MAX - 1) : (DWORD)waitMs64;

        HANDLE waitHandles[2] = { stopEvent, wakeEvent };
        DWORD handleCount = wakeEvent ? 2 : 1;
        DWORD rc = WaitForMultipleObjects(handleCount, waitHandles, FALSE, waitMs);

        if (rc == WAIT_TIMEOUT) continue; // normal interval elapsed
        if (rc == WAIT_OBJECT_0) {
            logger.Info("Worker loop exiting (stop requested).");
            break;
        }
        if (wakeEvent && rc == WAIT_OBJECT_0 + 1) {
            logger.Info("Worker loop: immediate collection requested via WebSocket signal.");
            continue;
        }
        // WAIT_FAILED or anything unexpected - do not spin: stop the loop.
        logger.Info("Worker loop exiting (WaitForMultipleObjects rc=" + std::to_string(rc) + ").");
        break;
    }

    if (wsStarted) {
        // Force-close whatever WebSocket handle is currently live so a
        // WinHttpWebSocketReceive call blocked on the ws thread returns right
        // away, instead of join() below waiting on a receive timeout that's
        // proven unreliable in practice (see the doc comment on
        // RunWebSocketLoop) - without this, a stop request could leave the
        // whole process wedged in STOP_PENDING indefinitely.
        HINTERNET stuckSocket = activeWsSocket.exchange(nullptr);
        if (stuckSocket) WinHttpCloseHandle(stuckSocket);
        if (wsThread.joinable()) wsThread.join();
    }
    if (wakeEvent) CloseHandle(wakeEvent);

    SnmpGlobalCleanup();
    logger.Info("=== Printer Inventory Agent stopped ===");
}

} // namespace

void PrinterAgentService::RunAsService() {
    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(ServiceName()), (LPSERVICE_MAIN_FUNCTIONW)ServiceMain },
        { nullptr, nullptr }
    };
    // Blocks until the service stops; failures here almost always mean we
    // were launched interactively without --console (not by the SCM).
    if (!StartServiceCtrlDispatcherW(table)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            fwprintf(stderr,
                L"This must be started by the Service Control Manager.\n"
                L"Use --console to run interactively, or --install to register the service.\n");
        }
    }
}

void WINAPI PrinterAgentService::ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
    // Create the stop event *before* registering the control handler: once
    // registered, a STOP/SHUTDOWN control can arrive on another thread at
    // any moment, and ServiceCtrlHandlerEx must never observe a null handle
    // for a stop request that genuinely needs to be honored.
    HANDLE localStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!localStopEvent) return;
    stopEvent_.store(localStopEvent);

    statusHandle_ = RegisterServiceCtrlHandlerExW(ServiceName(), ServiceCtrlHandlerEx, nullptr);
    if (!statusHandle_) {
        CloseHandle(localStopEvent);
        stopEvent_.store(nullptr);
        return;
    }

    ZeroMemory(&status_, sizeof(status_));
    status_.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status_.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);
    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    WorkerLoop(localStopEvent);

    // Atomically detach the handle before closing it, narrowing (though not
    // fully eliminating without a full handshake) the window where a
    // concurrent control-handler invocation could SetEvent a handle that's
    // about to be/was just closed.
    stopEvent_.exchange(nullptr);
    CloseHandle(localStopEvent);

    ReportStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

DWORD WINAPI PrinterAgentService::ServiceCtrlHandlerEx(DWORD ctrl, DWORD /*eventType*/, LPVOID /*eventData*/, LPVOID /*context*/) {
    switch (ctrl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN: {
            // Generous wait hint: a cycle can have an in-flight SNMP attempt
            // (up to snmp.timeout_ms) or HTTP POST (up to server.timeout_ms)
            // that cannot be canceled mid-flight; RunCollectionCycle checks
            // the stop event between printers, so worst case is roughly one
            // of those plus a little slack.
            ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 20000);
            HANDLE h = stopEvent_.load();
            if (h) SetEvent(h);
            return NO_ERROR;
        }
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void PrinterAgentService::ReportStatus(DWORD currentState, DWORD exitCode, DWORD waitHint) {
    static DWORD checkPoint = 1;
    status_.dwCurrentState = currentState;
    status_.dwWin32ExitCode = exitCode;
    status_.dwWaitHint = waitHint;
    status_.dwCheckPoint = (currentState == SERVICE_RUNNING || currentState == SERVICE_STOPPED) ? 0 : checkPoint++;
    if (statusHandle_) SetServiceStatus(statusHandle_, &status_);
}

namespace {
HANDLE g_consoleStopEvent = nullptr;

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_consoleStopEvent) SetEvent(g_consoleStopEvent);
            return TRUE;
        default:
            return FALSE;
    }
}
} // namespace

void PrinterAgentService::RunInConsole() {
    g_consoleStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_consoleStopEvent) {
        fwprintf(stderr, L"CreateEventW failed, error=%lu\n", GetLastError());
        return;
    }
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    wprintf(L"Printer Inventory Agent running in console mode. Press Ctrl+C to stop.\n");
    WorkerLoop(g_consoleStopEvent);

    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    CloseHandle(g_consoleStopEvent);
    g_consoleStopEvent = nullptr;
}

bool PrinterAgentService::Install(std::wstring* errorOut) {
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        if (errorOut) *errorOut = L"GetModuleFileNameW failed";
        return false;
    }
    // Quoted: an unquoted path containing spaces (e.g. "C:\Program Files\...")
    // makes the SCM launch it as multiple arguments, which both breaks
    // startup (main.cpp sees an unrecognized argv[1] and exits) and is the
    // classic unquoted-service-path privilege-escalation footgun.
    std::wstring quotedExePath = L"\"" + std::wstring(exePath) + L"\"";

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        if (errorOut) *errorOut = L"OpenSCManagerW failed, error=" + std::to_wstring(GetLastError()) +
            L" (are you running as Administrator?)";
        return false;
    }

    SC_HANDLE svc = CreateServiceW(
        scm, ServiceName(), ServiceDisplayName(),
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        quotedExePath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);

    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_EXISTS) {
            if (errorOut) *errorOut = L"Service already installed.";
        } else if (errorOut) {
            *errorOut = L"CreateServiceW failed, error=" + std::to_wstring(err);
        }
        return false;
    }

    // Best-effort failure recovery: restart the service automatically if the
    // process crashes, so one unhandled fault doesn't silently stop collection
    // on a machine nobody is watching.
    SERVICE_FAILURE_ACTIONSW failureActions{};
    SC_ACTION actions[3] = {
        { SC_ACTION_RESTART, 60000 },
        { SC_ACTION_RESTART, 60000 },
        { SC_ACTION_RESTART, 60000 },
    };
    failureActions.dwResetPeriod = 86400; // reset failure count after 1 day
    failureActions.cActions = 3;
    failureActions.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failureActions);

    SERVICE_DESCRIPTIONW desc{};
    std::wstring descText = L"Collects installed-printer inventory and page counts and reports them to a central server.";
    desc.lpDescription = const_cast<LPWSTR>(descText.c_str());
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

bool PrinterAgentService::Uninstall(std::wstring* errorOut) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        if (errorOut) *errorOut = L"OpenSCManagerW failed, error=" + std::to_wstring(GetLastError()) +
            L" (are you running as Administrator?)";
        return false;
    }

    SC_HANDLE svc = OpenServiceW(scm, ServiceName(), SERVICE_STOP | DELETE);
    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        if (errorOut) *errorOut = L"OpenServiceW failed, error=" + std::to_wstring(err);
        return false;
    }

    SERVICE_STATUS status{};
    ControlService(svc, SERVICE_CONTROL_STOP, &status); // best-effort; ignore failure if already stopped

    bool ok = DeleteService(svc) != 0;
    if (!ok && errorOut) *errorOut = L"DeleteService failed, error=" + std::to_wstring(GetLastError());

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}
