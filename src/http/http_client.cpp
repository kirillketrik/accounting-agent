#include "http_client.h"
#include "common/string_utils.h"
#include <windows.h>
#include <winhttp.h>
#include <vector>
#include <algorithm>
#include <cstdlib> // _countof
#include <cstdio> // snprintf

namespace {

struct UrlParts {
    bool ok = false;
    bool secure = false;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring pathAndQuery;
};

UrlParts CrackUrl(const std::wstring& url) {
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

    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
        return result;
    }

    result.ok = true;
    result.secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    result.host = hostBuf;
    result.port = uc.nPort;
    result.pathAndQuery = std::wstring(pathBuf) + std::wstring(extraBuf);
    if (result.pathAndQuery.empty()) result.pathAndQuery = L"/";
    return result;
}

// WinHttpSendRequest/WinHttpReceiveResponse only ever surface a TLS problem
// as the generic ERROR_WINHTTP_SECURE_FAILURE (12175) via GetLastError() -
// no detail on *why* (expired cert? untrusted CA? hostname mismatch?). The
// specific reason is only available through this status callback, delivered
// synchronously even without WINHTTP_FLAG_ASYNC.
struct SecureFailureInfo {
    bool captured = false;
    DWORD flags = 0;
};

void CALLBACK OnWinHttpStatus(HINTERNET /*hInternet*/, DWORD_PTR dwContext, DWORD dwInternetStatus,
                               LPVOID lpvStatusInformation, DWORD dwStatusInformationLength) {
    if (dwInternetStatus != WINHTTP_CALLBACK_STATUS_SECURE_FAILURE) return;
    if (dwContext == 0 || lpvStatusInformation == nullptr || dwStatusInformationLength < sizeof(DWORD)) return;
    auto* info = reinterpret_cast<SecureFailureInfo*>(dwContext);
    info->captured = true;
    info->flags = *reinterpret_cast<DWORD*>(lpvStatusInformation);
}

std::string DescribeSecureFailure(const SecureFailureInfo& info) {
    if (!info.captured) return "TLS handshake failed (no further detail available)";
    std::vector<std::string> reasons;
    if (info.flags & WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA)
        reasons.push_back("issuing CA is untrusted (self-signed certificate or unknown/missing root CA)");
    if (info.flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_CN_INVALID)
        reasons.push_back("certificate name does not match the server hostname/IP in the URL");
    if (info.flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_DATE_INVALID)
        reasons.push_back("certificate has expired or is not yet valid");
    if (info.flags & WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CERT)
        reasons.push_back("certificate is malformed/invalid");
    if (info.flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REVOKED)
        reasons.push_back("certificate was revoked");
    if (info.flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED)
        reasons.push_back("certificate revocation check could not be completed");
    if (info.flags & WINHTTP_CALLBACK_STATUS_FLAG_CERT_WRONG_USAGE)
        reasons.push_back("certificate has the wrong key usage for TLS server auth");
    if (info.flags & WINHTTP_CALLBACK_STATUS_FLAG_SECURITY_CHANNEL_ERROR)
        reasons.push_back("secure channel/protocol negotiation error");
    if (reasons.empty()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)info.flags);
        return std::string("TLS handshake failed, unrecognized flags=") + buf;
    }
    std::string out;
    for (size_t i = 0; i < reasons.size(); i++) {
        if (i) out += "; ";
        out += reasons[i];
    }
    return out;
}

} // namespace

HttpPostResult HttpPostJson(const std::string& url, const std::string& jsonBody,
                             const std::string& authToken, int timeoutMs, Logger& logger) {
    HttpPostResult result;

    std::wstring wUrl = strutil::Utf8ToWide(url);
    UrlParts parts = CrackUrl(wUrl);
    if (!parts.ok) {
        result.error = "invalid server URL: " + url;
        logger.Error("HTTP: " + result.error);
        return result;
    }

    HINTERNET hSession = WinHttpOpen(
        L"PrinterInventoryAgent/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) {
        result.error = "WinHttpOpen failed, GetLastError=" + std::to_string(GetLastError());
        logger.Error("HTTP: " + result.error);
        return result;
    }

    WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, parts.host.c_str(), parts.port, 0);
    if (!hConnect) {
        result.error = "WinHttpConnect failed, GetLastError=" + std::to_string(GetLastError());
        logger.Error("HTTP: " + result.error);
        WinHttpCloseHandle(hSession);
        return result;
    }

    DWORD requestFlags = parts.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", parts.pathAndQuery.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, requestFlags);
    if (!hRequest) {
        result.error = "WinHttpOpenRequest failed, GetLastError=" + std::to_string(GetLastError());
        logger.Error("HTTP: " + result.error);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    SecureFailureInfo secureFailure;
    if (parts.secure) {
        // TLS 1.2 minimum - TLS 1.0/1.1 are deprecated (RFC 8996: known
        // downgrade/BEAST/POODLE-family weaknesses) and must not be offered.
        // On Windows 7 this still requires the OS-level TLS 1.2 update/
        // registry change (KB3140245); this flag alone does not add protocol
        // support the underlying Schannel lacks.
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

        // See OnWinHttpStatus: the only way to learn *why* a TLS handshake
        // failed (untrusted CA, hostname mismatch, expired cert, ...) instead
        // of just the generic ERROR_WINHTTP_SECURE_FAILURE.
        DWORD_PTR ctx = reinterpret_cast<DWORD_PTR>(&secureFailure);
        WinHttpSetOption(hRequest, WINHTTP_OPTION_CONTEXT_VALUE, &ctx, sizeof(ctx));
        WinHttpSetStatusCallback(hRequest, OnWinHttpStatus, WINHTTP_CALLBACK_FLAG_SECURE_FAILURE, 0);
    }

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!authToken.empty()) {
        headers += L"Authorization: Bearer " + strutil::Utf8ToWide(authToken) + L"\r\n";
    }

    BOOL sent = WinHttpSendRequest(
        hRequest,
        headers.c_str(), (DWORD)-1,
        (LPVOID)jsonBody.data(), (DWORD)jsonBody.size(), (DWORD)jsonBody.size(),
        0);
    if (!sent) {
        DWORD err = GetLastError();
        result.error = "WinHttpSendRequest failed, GetLastError=" + std::to_string(err);
        if (err == ERROR_WINHTTP_SECURE_FAILURE) result.error += " (" + DescribeSecureFailure(secureFailure) + ")";
        logger.Error("HTTP: " + result.error);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD err = GetLastError();
        result.error = "WinHttpReceiveResponse failed, GetLastError=" + std::to_string(err);
        if (err == ERROR_WINHTTP_SECURE_FAILURE) result.error += " (" + DescribeSecureFailure(secureFailure) + ")";
        logger.Error("HTTP: " + result.error);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(
        hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
    result.statusCode = (int)statusCode;

    // Drain a small preview of the response body for diagnostics; the server's
    // actual reply format isn't finalized yet, so we don't parse it.
    std::string bodyPreview;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &available) || available == 0) break;
        DWORD toRead = std::min(available, (DWORD)512);
        std::vector<char> buf(toRead);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, buf.data(), toRead, &read) || read == 0) break;
        bodyPreview.append(buf.data(), read);
        if (bodyPreview.size() >= 512) break;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (statusCode >= 200 && statusCode < 300) {
        result.success = true;
    } else {
        result.error = "server returned HTTP " + std::to_string(statusCode) +
            (bodyPreview.empty() ? "" : (": " + bodyPreview));
        logger.Warn("HTTP: " + result.error);
    }

    return result;
}
