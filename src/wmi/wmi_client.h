#pragma once
#include <windows.h>
#include <objbase.h>   // CoInitializeEx / CoInitializeSecurity / CoCreateInstance / CoSetProxyBlanket
#include <wbemidl.h>
#include <comdef.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include "common/logger.h"

// Thin RAII wrapper around a WMI (ROOT\CIMV2) connection, used to enumerate
// Win32_Printer / Win32_TCPIPPrinterPort instances. COM initialization is
// process-wide and handled once by WmiSession; construct/destroy it around
// the lifetime of the worker thread that uses it.
class WmiSession {
public:
    explicit WmiSession(Logger& logger);
    ~WmiSession();

    WmiSession(const WmiSession&) = delete;
    WmiSession& operator=(const WmiSession&) = delete;

    // Initializes COM + security + connects to ROOT\CIMV2. Safe to call once
    // per thread that will use this session. Returns false on any failure
    // (logged internally); callers should skip WMI-dependent work for this cycle.
    bool Connect();

    // Runs a WQL query and invokes `callback` once per result row. Returns
    // false if the query itself could not be executed (logged internally).
    // A false return from `callback` stops enumeration early (not an error).
    bool Query(const std::wstring& wql, const std::function<bool(IWbemClassObject*)>& callback);

    bool IsConnected() const { return services_ != nullptr; }

private:
    Logger& logger_;
    bool comInitialized_ = false;
    bool securityInitialized_ = false;
    IWbemLocator* locator_ = nullptr;
    IWbemServices* services_ = nullptr;
};

// Property accessor helpers - all tolerate missing/NULL/wrong-type properties
// by returning the given default, which is the common case for optional WMI
// fields (e.g. ShareName when Shared=false).
std::wstring WmiGetString(IWbemClassObject* obj, const wchar_t* property, const std::wstring& fallback = L"");
bool WmiGetBool(IWbemClassObject* obj, const wchar_t* property, bool fallback = false);
uint32_t WmiGetUInt32(IWbemClassObject* obj, const wchar_t* property, uint32_t fallback = 0);

// Doubles every single-quote so a value can be safely embedded in a WQL
// string literal (WQL/SQL-style escaping - there is no parameterized query
// API here).
std::wstring EscapeWqlLiteral(const std::wstring& s);
