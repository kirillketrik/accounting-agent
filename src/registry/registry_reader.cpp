#include "registry_reader.h"
#include "common/string_utils.h"
#include <windows.h>
#include <vector>
#include <cstring>

namespace {

bool NameLooksLikeCounter(const std::string& valueName) {
    return strutil::IContains(valueName, "count") ||
           strutil::IContains(valueName, "page") ||
           strutil::IContains(valueName, "counter");
}

// Best-effort interpretation of a registry value as an integer counter.
// Returns false if the type/size doesn't look like a plausible integer.
bool TryParseAsInteger(DWORD type, const std::vector<BYTE>& data, int64_t& outValue) {
    switch (type) {
        case REG_DWORD: // == REG_DWORD_LITTLE_ENDIAN
            if (data.size() >= sizeof(DWORD)) {
                DWORD v;
                memcpy(&v, data.data(), sizeof(DWORD));
                outValue = (int64_t)v;
                return true;
            }
            return false;
        case REG_DWORD_BIG_ENDIAN:
            if (data.size() >= sizeof(DWORD)) {
                outValue = (int64_t)((uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
                                      (uint32_t)data[2] << 8 | (uint32_t)data[3]);
                return true;
            }
            return false;
        case REG_QWORD:
            if (data.size() >= sizeof(uint64_t)) {
                uint64_t v;
                memcpy(&v, data.data(), sizeof(uint64_t));
                outValue = (int64_t)v;
                return true;
            }
            return false;
        case REG_BINARY:
            // Undocumented vendor blobs are frequently just a raw 4- or 8-byte
            // little-endian counter with no wrapper - matches HPMediaCount in
            // practice (see brief).
            if (data.size() == 4) {
                uint32_t v;
                memcpy(&v, data.data(), 4);
                outValue = (int64_t)v;
                return true;
            }
            if (data.size() == 8) {
                uint64_t v;
                memcpy(&v, data.data(), 8);
                outValue = (int64_t)v;
                return true;
            }
            return false;
        case REG_SZ:
        case REG_EXPAND_SZ: {
            if (data.empty()) return false;
            std::wstring ws(reinterpret_cast<const wchar_t*>(data.data()), data.size() / sizeof(wchar_t));
            // Strip trailing NUL(s).
            while (!ws.empty() && ws.back() == L'\0') ws.pop_back();
            if (ws.empty()) return false;
            try {
                size_t idx = 0;
                long long v = std::stoll(ws, &idx);
                if (idx == 0) return false;
                outValue = (int64_t)v;
                return true;
            } catch (...) {
                return false;
            }
        }
        default:
            return false;
    }
}

} // namespace

RegistryCounterResult ReadPrinterDriverDataCounters(const std::string& printerName, Logger& logger) {
    RegistryCounterResult result;

    std::wstring keyPath = L"SYSTEM\\CurrentControlSet\\Control\\Print\\Printers\\" +
        strutil::Utf8ToWide(printerName) + L"\\PrinterDriverData";

    HKEY hKey = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_READ, &hKey);
    if (rc != ERROR_SUCCESS) {
        logger.Debug("Registry: PrinterDriverData not found for '" + printerName + "' (rc=" + std::to_string(rc) + ")");
        return result;
    }
    result.keyFound = true;

    DWORD valueCount = 0, maxNameLen = 0, maxValueLen = 0;
    rc = RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        &valueCount, &maxNameLen, &maxValueLen, nullptr, nullptr);
    if (rc != ERROR_SUCCESS) {
        logger.Warn("Registry: RegQueryInfoKeyW failed for '" + printerName + "' (rc=" + std::to_string(rc) + ")");
        RegCloseKey(hKey);
        return result;
    }

    std::vector<wchar_t> nameBuf(maxNameLen + 1);
    std::vector<BYTE> dataBuf(maxValueLen > 0 ? maxValueLen : 1);

    for (DWORD i = 0; i < valueCount; i++) {
        DWORD nameLen = (DWORD)nameBuf.size();
        DWORD dataLen = (DWORD)dataBuf.size();
        DWORD type = 0;

        rc = RegEnumValueW(hKey, i, nameBuf.data(), &nameLen, nullptr, &type, dataBuf.data(), &dataLen);
        if (rc != ERROR_SUCCESS) continue; // skip this value, keep enumerating the rest

        std::string name = strutil::WideToUtf8(std::wstring(nameBuf.data(), nameLen));
        if (!NameLooksLikeCounter(name)) continue;

        std::vector<BYTE> data(dataBuf.begin(), dataBuf.begin() + dataLen);
        int64_t value = 0;
        if (TryParseAsInteger(type, data, value)) {
            logger.Debug("Registry: '" + printerName + "' PrinterDriverData['" + name + "'] = " + std::to_string(value));
            result.counters.push_back({name, value});
        } else {
            logger.Debug("Registry: '" + printerName + "' PrinterDriverData['" + name +
                "'] present but not integer-like (type=" + std::to_string(type) + "), skipped");
        }
    }

    RegCloseKey(hKey);
    return result;
}
