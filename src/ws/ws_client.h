#pragma once
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include "common/config.h"
#include "common/logger.h"

// Maintains a persistent WebSocket connection to config.wsUrl (only when
// config.wsEnabled and wsUrl are set), auto-reconnecting with backoff on any
// disconnect. This is a one-way control channel from server to agent: when
// the server sends a recognized "send report now" text message, `wakeEvent`
// is signaled so the worker loop in service_base.cpp runs an out-of-cycle
// collection immediately instead of waiting for the next polling interval.
//
// Intended to run on its own thread for the lifetime of the process; blocks
// until `stopEvent` is signaled, then closes the connection and returns.
// Never throws and never crashes the process: a bad/unreachable URL, a
// malformed server message, or (on Windows 7, which lacks the WinHTTP
// WebSocket entry points added in Windows 8.1) a missing OS feature all just
// log a warning and leave the agent running on polling alone.
//
// `activeSocketOut` (optional): while a connection is up, this function
// publishes the live WinHTTP WebSocket handle here so a caller that just
// signaled `stopEvent` can force-close it. This matters because
// WinHttpWebSocketReceive's blocking wait was found NOT to reliably honor
// the receive timeout set on the session once idle (observed in practice: a
// service stop request left the thread blocked in it indefinitely, wedging
// the whole process in STOP_PENDING) - closing the handle from another
// thread is WinHTTP's documented way to cancel a blocked synchronous call,
// and is this function's real exit path on stop, not the receive timeout.
void RunWebSocketLoop(const AgentConfig& config, Logger& logger, HANDLE stopEvent, HANDLE wakeEvent,
                       std::atomic<HINTERNET>* activeSocketOut = nullptr);
