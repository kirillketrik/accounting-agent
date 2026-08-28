#include "ws_client.h"
#include "common/string_utils.h"
#include "common/json_writer.h"
#include "network/network_info.h"
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib> // _countof

namespace {

typedef HINTERNET (WINAPI *PFN_CompleteUpgrade)(HINTERNET hRequest, DWORD_PTR pContext);
typedef DWORD (WINAPI *PFN_Send)(HINTERNET hWebSocket, WINHTTP_WEB_SOCKET_BUFFER_TYPE eBufferType,
                                  PVOID pvBuffer, DWORD dwBufferLength);
typedef DWORD (WINAPI *PFN_Receive)(HINTERNET hWebSocket, PVOID pvBuffer, DWORD dwBufferLength,
                                     DWORD* pdwBytesRead, WINHTTP_WEB_SOCKET_BUFFER_TYPE* peBufferType);
typedef DWORD (WINAPI *PFN_Close)(HINTERNET hWebSocket, USHORT usStatus, PVOID pvReason, DWORD dwReasonLength);

struct WebSocketApi {
    PFN_CompleteUpgrade completeUpgrade = nullptr;
    PFN_Send send = nullptr;
    PFN_Receive receive = nullptr;
    PFN_Close close = nullptr;
    bool Loaded() const { return completeUpgrade && send && receive && close; }
};

// The four WinHttpWebSocket* entry points were added in Windows 8.1;
// winhttp.dll on Windows 7 (our minimum supported OS per the brief) does not
// export them. Resolved by name via GetProcAddress - deliberately never
// referenced directly - so a missing export degrades this one feature
// instead of the whole process failing to load from an unresolved import.
WebSocketApi LoadWebSocketApi(Logger& logger) {
    WebSocketApi api;
    // Already loaded: the rest of the agent (http_client.cpp) links against
    // winhttp.lib for the plain HTTP entry points, so winhttp.dll is in this
    // process's module list by the time any thread runs.
    HMODULE mod = GetModuleHandleW(L"winhttp.dll");
    if (!mod) {
        logger.Warn("WebSocket: winhttp.dll is not loaded in this process; skipping control channel.");
        return api;
    }
    api.completeUpgrade = (PFN_CompleteUpgrade)GetProcAddress(mod, "WinHttpWebSocketCompleteUpgrade");
    api.send = (PFN_Send)GetProcAddress(mod, "WinHttpWebSocketSend");
    api.receive = (PFN_Receive)GetProcAddress(mod, "WinHttpWebSocketReceive");
    api.close = (PFN_Close)GetProcAddress(mod, "WinHttpWebSocketClose");
    if (!api.Loaded()) {
        logger.Warn("WebSocket: this OS's winhttp.dll does not export the WebSocket API "
                     "(requires Windows 8.1 or later) - the websocket.url signal channel is "
                     "unavailable. The agent continues to work normally via polling alone.");
    }
    return api;
}

struct UrlParts {
    bool ok = false;
    bool secure = false;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring pathAndQuery;
};

UrlParts CrackUrl(const std::wstring& url) {
    // WinHttpCrackUrl doesn't know the ws:// / wss:// schemes; swap in the
    // http(s) equivalent purely for parsing. `secure` (captured separately)
    // is what actually decides WINHTTP_FLAG_SECURE below, not the string.
    std::wstring effective = url;
    bool secure = false;
    if (_wcsnicmp(effective.c_str(), L"wss://", 6) == 0) {
        secure = true;
        effective = L"https://" + effective.substr(6);
    } else if (_wcsnicmp(effective.c_str(), L"ws://", 5) == 0) {
        effective = L"http://" + effective.substr(5);
    }

    UrlParts result;
    wchar_t hostBuf[256] = {0};
    wchar_t pathBuf[2048] = {0};
    wchar_t extraBuf[2048] = {0};

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = _countof(hostBuf);
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = _countof(pathBuf);
    uc.lpszExtraInfo = extraBuf;
    uc.dwExtraInfoLength = _countof(extraBuf);

    if (!WinHttpCrackUrl(effective.c_str(), (DWORD)effective.size(), 0, &uc)) return result;

    result.ok = true;
    result.secure = secure;
    result.host = hostBuf;
    result.port = uc.nPort;
    result.pathAndQuery = std::wstring(pathBuf) + std::wstring(extraBuf);
    if (result.pathAndQuery.empty()) result.pathAndQuery = L"/";
    return result;
}

// Minimal extraction of a top-level string field from a small flat JSON
// object, e.g. ExtractJsonStringField(R"({"type":"send_report"})", "type").
// Not a general JSON parser (see json_writer.h for why this project doesn't
// have one) - good enough for the tiny, fixed control-message shape this
// channel uses.
bool ExtractJsonStringField(const std::string& json, const std::string& key, std::string& out) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    pos = json.find('"', pos);
    if (pos == std::string::npos) return false;
    size_t start = pos + 1;
    size_t end = json.find('"', start);
    while (end != std::string::npos && end > start && json[end - 1] == '\\') {
        end = json.find('"', end + 1); // skip escaped quotes
    }
    if (end == std::string::npos) return false;
    out = json.substr(start, end - start);
    return true;
}

bool IsSendReportSignal(const std::string& message) {
    std::string trimmed = strutil::Trim(message);
    if (trimmed.empty()) return false;

    std::string type;
    if (ExtractJsonStringField(trimmed, "type", type)) {
        std::string t = strutil::ToLower(type);
        return t == "send_report" || t == "report_now" || t == "trigger_report";
    }

    // Not JSON (or no "type" field): also accept a few bare words, so the
    // channel is trivially testable by hand (e.g. with wscat) without having
    // to type JSON.
    std::string lower = strutil::ToLower(trimmed);
    return lower == "send_report" || lower == "report_now" || lower == "trigger_report" || lower == "report";
}

// Interruptible sleep for the reconnect backoff. Returns true if `stopEvent`
// was signaled during the wait (caller should stop retrying and exit).
bool InterruptibleSleep(HANDLE stopEvent, DWORD ms) {
    if (!stopEvent) {
        Sleep(ms);
        return false;
    }
    return WaitForSingleObject(stopEvent, ms) == WAIT_OBJECT_0;
}

// One connect -> handshake -> receive-until-disconnected attempt. Returns
// once the connection drops, is closed by the server, or `stopEvent` fires.
void RunOneConnection(const WebSocketApi& api, const AgentConfig& config, Logger& logger,
                       HANDLE stopEvent, HANDLE wakeEvent, const UrlParts& parts,
                       std::atomic<HINTERNET>* activeSocketOut) {
    HINTERNET hSession = WinHttpOpen(L"PrinterInventoryAgent/1.0 (websocket)",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        logger.Warn("WebSocket: WinHttpOpen failed, GetLastError=" + std::to_string(GetLastError()));
        return;
    }

    // A finite receive timeout (not "wait forever") so the loop below wakes
    // up periodically to re-check stopEvent even with no server traffic. Per
    // WinHTTP docs, the timeouts set here also govern
    // WinHttpWebSocketSend/Receive once the connection is upgraded. Kept
    // under the service's 20s stop wait-hint (see service_base.cpp) so a
    // stop request is never left waiting on this alone.
    WinHttpSetTimeouts(hSession, config.httpTimeoutMs, config.httpTimeoutMs, config.httpTimeoutMs, 10000);

    HINTERNET hConnect = WinHttpConnect(hSession, parts.host.c_str(), parts.port, 0);
    if (!hConnect) {
        logger.Warn("WebSocket: WinHttpConnect failed, GetLastError=" + std::to_string(GetLastError()));
        WinHttpCloseHandle(hSession);
        return;
    }

    DWORD requestFlags = parts.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", parts.pathAndQuery.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, requestFlags);
    if (!hRequest) {
        logger.Warn("WebSocket: WinHttpOpenRequest failed, GetLastError=" + std::to_string(GetLastError()));
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    if (parts.secure) {
        // TLS 1.2 minimum - same reasoning as http_client.cpp: TLS 1.0/1.1
        // are deprecated and must not be offered. Same Win7 caveat: still
        // needs the OS-level TLS 1.2 update (KB3140245) to actually
        // negotiate TLS1.2 on Windows 7.
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }

    if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        logger.Warn("WebSocket: failed to request protocol upgrade, GetLastError=" + std::to_string(GetLastError()));
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    std::wstring headers;
    if (!config.authToken.empty()) {
        headers = L"Authorization: Bearer " + strutil::Utf8ToWide(config.authToken) + L"\r\n";
    }

    BOOL sent = WinHttpSendRequest(hRequest,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), (DWORD)-1,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(hRequest, nullptr)) {
        logger.Warn("WebSocket: handshake failed, GetLastError=" + std::to_string(GetLastError()));
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    HINTERNET hWebSocket = api.completeUpgrade(hRequest, 0);
    // Per MSDN, the request handle is no longer needed once the upgrade
    // attempt completes (successfully or not) - the websocket handle (if any)
    // takes over from here.
    WinHttpCloseHandle(hRequest);
    if (!hWebSocket) {
        logger.Warn("WebSocket: upgrade to WebSocket failed, GetLastError=" + std::to_string(GetLastError()));
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    logger.Info("WebSocket: connected to " + config.wsUrl);

    // Published so a stop request on another thread can force-close this
    // exact handle - see the doc comment on RunWebSocketLoop for why that's
    // needed instead of relying on the receive timeout.
    if (activeSocketOut) activeSocketOut->store(hWebSocket);

    // Best-effort identify message so the server can associate this
    // connection with a host without waiting for the next HTTP report.
    {
        JsonWriter hello;
        hello.BeginObject();
        hello.Field("type", "hello");
        hello.Field("hostname", GetLocalComputerName());
        hello.EndObject();
        std::string helloJson = hello.Str();
        api.send(hWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            (PVOID)helloJson.data(), (DWORD)helloJson.size());
    }

    std::string messageBuffer;
    std::vector<char> chunk(4096);

    for (;;) {
        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;

        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType;
        DWORD rc = api.receive(hWebSocket, chunk.data(), (DWORD)chunk.size(), &bytesRead, &bufferType);

        if (rc == ERROR_WINHTTP_TIMEOUT) {
            continue; // periodic wakeup with no data - just re-check stopEvent
        }
        if (rc != NO_ERROR) {
            logger.Warn("WebSocket: receive failed, error=" + std::to_string(rc) + "; will reconnect.");
            break;
        }

        if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            logger.Info("WebSocket: server closed the connection.");
            break;
        }
        if (bufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
            bufferType == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
            logger.Debug("WebSocket: ignoring unexpected binary frame.");
            messageBuffer.clear();
            continue;
        }

        messageBuffer.append(chunk.data(), bytesRead);
        if (bufferType == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
            continue; // more fragments still to come for this message
        }

        logger.Debug("WebSocket: received: " + messageBuffer);
        if (IsSendReportSignal(messageBuffer)) {
            logger.Info("WebSocket: 'send report' signal received; triggering an immediate collection cycle.");
            if (wakeEvent) SetEvent(wakeEvent);
        }
        messageBuffer.clear();
    }

    // Claim the handle before closing it: if a stop request on another
    // thread already force-closed it (to unblock a pending api.receive()
    // above), the compare_exchange below fails and we must not touch the
    // handle again - it's already gone.
    bool weOwnClose = true;
    if (activeSocketOut) {
        HINTERNET expected = hWebSocket;
        weOwnClose = activeSocketOut->compare_exchange_strong(expected, nullptr);
    }

    if (weOwnClose) {
        bool stopRequested = stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0;
        if (stopRequested) {
            // Best-effort graceful close handshake; if the peer is already gone
            // this just fails harmlessly and we fall through to handle cleanup.
            api.close(hWebSocket, 1000 /* WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS */, nullptr, 0);
        }
        WinHttpCloseHandle(hWebSocket);
    }
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
}

} // namespace

void RunWebSocketLoop(const AgentConfig& config, Logger& logger, HANDLE stopEvent, HANDLE wakeEvent,
                       std::atomic<HINTERNET>* activeSocketOut) {
    if (config.wsUrl.empty()) return; // caller already checks this; defensive

    WebSocketApi api = LoadWebSocketApi(logger);
    if (!api.Loaded()) return; // already logged; agent proceeds via polling only

    std::wstring wUrl = strutil::Utf8ToWide(config.wsUrl);
    UrlParts parts = CrackUrl(wUrl);
    if (!parts.ok) {
        logger.Error("WebSocket: invalid websocket.url: " + config.wsUrl);
        return;
    }

    // Reconnect pattern: retry every wsRetryIntervalMs. After
    // wsMaxRetriesBeforeRelax consecutive failed attempts, back off for
    // wsRelaxDelayMs, then reset the counter and resume the fast retries.
    // A connection that stayed up for a while counts as success and resets
    // the failure streak, so one later hiccup doesn't inherit a long outage's
    // near-miss count.
    const int kRetryIntervalMs = std::max(config.wsRetryIntervalMs, 250);
    const int kMaxRetriesBeforeRelax = std::max(config.wsMaxRetriesBeforeRelax, 1);
    const int kRelaxDelayMs = std::max(config.wsRelaxDelayMs, 1000);
    int consecutiveFailures = 0;

    while (!stopEvent || WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        ULONGLONG connectedAt = GetTickCount64();
        try {
            RunOneConnection(api, config, logger, stopEvent, wakeEvent, parts, activeSocketOut);
        } catch (const std::exception& ex) {
            logger.Warn(std::string("WebSocket: unexpected exception: ") + ex.what());
        } catch (...) {
            logger.Warn("WebSocket: unknown unexpected exception.");
        }

        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;

        ULONGLONG connectedForMs = GetTickCount64() - connectedAt;
        bool wasStable = connectedForMs > 30000;

        int waitMs;
        if (wasStable) {
            consecutiveFailures = 0;
            waitMs = kRetryIntervalMs;
            logger.Info("WebSocket: disconnected; retrying in " + std::to_string(waitMs) + "ms.");
        } else if (++consecutiveFailures >= kMaxRetriesBeforeRelax) {
            waitMs = kRelaxDelayMs;
            logger.Info("WebSocket: " + std::to_string(consecutiveFailures) +
                " failed attempts in a row; relaxing for " + std::to_string(waitMs) + "ms.");
            consecutiveFailures = 0;
        } else {
            waitMs = kRetryIntervalMs;
            logger.Info("WebSocket: disconnected; retrying in " + std::to_string(waitMs) + "ms.");
        }

        if (InterruptibleSleep(stopEvent, (DWORD)waitMs)) break;
    }

    logger.Info("WebSocket: control-channel thread exiting.");
}
