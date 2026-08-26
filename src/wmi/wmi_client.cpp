#include "wmi_client.h"
#include "common/string_utils.h"
#include <cstdio>

namespace {
// HRESULTs are conventionally read as hex; std::to_string() prints decimal,
// which made every WMI diagnostic in the log unreadable (e.g. "hr=0x2147942405").
std::string HrToString(HRESULT hr) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)hr);
    return buf;
}
} // namespace

WmiSession::WmiSession(Logger& logger) : logger_(logger) {}

WmiSession::~WmiSession() {
    if (services_) services_->Release();
    if (locator_) locator_->Release();
    if (comInitialized_) CoUninitialize();
}

bool WmiSession::Connect() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE means this thread already has COM initialized with a
    // different concurrency model (unusual for our worker thread, but not fatal -
    // we just don't own uninitialization in that case).
    if (SUCCEEDED(hr)) {
        comInitialized_ = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        logger_.Error("WMI: CoInitializeEx failed, hr=" + HrToString(hr));
        return false;
    }

    // CoInitializeSecurity may only be called once per process; if another
    // component already called it (e.g. WinHTTP/COM elsewhere), ignore
    // RPC_E_TOO_LATE and proceed - the existing security settings are fine
    // for a local WMI connection.
    hr = CoInitializeSecurity(
        nullptr, -1, nullptr, nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE, nullptr);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
        logger_.Warn("WMI: CoInitializeSecurity failed, hr=" + HrToString(hr) + " (continuing)");
    }

    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
        IID_IWbemLocator, (LPVOID*)&locator_);
    if (FAILED(hr)) {
        logger_.Error("WMI: failed to create IWbemLocator, hr=" + HrToString(hr));
        return false;
    }

    hr = locator_->ConnectServer(
        _bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr,
        0, nullptr, nullptr, &services_);
    if (FAILED(hr)) {
        logger_.Error("WMI: ConnectServer(ROOT\\CIMV2) failed, hr=" + HrToString(hr));
        return false;
    }

    hr = CoSetProxyBlanket(
        services_, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) {
        logger_.Error("WMI: CoSetProxyBlanket failed, hr=" + HrToString(hr));
        return false;
    }

    return true;
}

bool WmiSession::Query(const std::wstring& wql, const std::function<bool(IWbemClassObject*)>& callback) {
    if (!services_) return false;

    IEnumWbemClassObject* enumerator = nullptr;
    HRESULT hr = services_->ExecQuery(
        _bstr_t(L"WQL"), _bstr_t(wql.c_str()),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &enumerator);
    if (FAILED(hr) || enumerator == nullptr) {
        logger_.Error("WMI: query failed: " + strutil::WideToUtf8(wql) + " hr=" + HrToString(hr));
        return false;
    }

    IWbemClassObject* obj = nullptr;
    ULONG returned = 0;
    try {
        while (true) {
            hr = enumerator->Next(WBEM_INFINITE, 1, &obj, &returned);
            if (FAILED(hr) || returned == 0) break;
            bool keepGoing = true;
            if (callback) keepGoing = callback(obj);
            obj->Release();
            obj = nullptr;
            if (!keepGoing) break;
        }
    } catch (...) {
        // A callback can throw (e.g. std::bad_alloc while building a
        // PrinterInfo) - release whatever's outstanding before propagating,
        // so we don't leak the enumerator/object and leave the WMI proxy
        // pinned past this WmiSession's lifetime.
        if (obj) obj->Release();
        enumerator->Release();
        throw;
    }

    enumerator->Release();
    return true;
}

std::wstring WmiGetString(IWbemClassObject* obj, const wchar_t* property, const std::wstring& fallback) {
    VARIANT v;
    VariantInit(&v);
    std::wstring result = fallback;
    if (SUCCEEDED(obj->Get(property, 0, &v, nullptr, nullptr))) {
        if (v.vt == VT_BSTR && v.bstrVal != nullptr) {
            result = std::wstring(v.bstrVal, SysStringLen(v.bstrVal));
        }
    }
    VariantClear(&v);
    return result;
}

bool WmiGetBool(IWbemClassObject* obj, const wchar_t* property, bool fallback) {
    VARIANT v;
    VariantInit(&v);
    bool result = fallback;
    if (SUCCEEDED(obj->Get(property, 0, &v, nullptr, nullptr))) {
        if (v.vt == VT_BOOL) result = (v.boolVal != VARIANT_FALSE);
    }
    VariantClear(&v);
    return result;
}

std::wstring EscapeWqlLiteral(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'\'') out += L"''";
        else out += c;
    }
    return out;
}

uint32_t WmiGetUInt32(IWbemClassObject* obj, const wchar_t* property, uint32_t fallback) {
    VARIANT v;
    VariantInit(&v);
    uint32_t result = fallback;
    if (SUCCEEDED(obj->Get(property, 0, &v, nullptr, nullptr))) {
        switch (v.vt) {
            case VT_I4: result = (uint32_t)v.lVal; break;
            case VT_UI4: result = v.ulVal; break;
            case VT_I2: result = (uint32_t)v.iVal; break;
            case VT_UI2: result = v.uiVal; break;
            default: break;
        }
    }
    VariantClear(&v);
    return result;
}
