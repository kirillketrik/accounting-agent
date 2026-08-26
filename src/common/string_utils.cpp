#include "string_utils.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace strutil {

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring result(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), result.data(), needed);
    return result;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string result(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), result.data(), needed, nullptr, nullptr);
    return result;
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

bool IContains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    std::string h = ToLower(haystack);
    std::string n = ToLower(needle);
    return h.find(n) != std::string::npos;
}

bool IEquals(const std::string& a, const std::string& b) {
    return ToLower(a) == ToLower(b);
}

bool IStartsWith(const std::string& s, const std::string& prefix) {
    if (prefix.size() > s.size()) return false;
    return ToLower(s.substr(0, prefix.size())) == ToLower(prefix);
}

std::string Trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace((unsigned char)s[start])) start++;
    size_t end = s.size();
    while (end > start && std::isspace((unsigned char)s[end - 1])) end--;
    return s.substr(start, end - start);
}

std::string JsonEscape(const std::string& s) {
    std::ostringstream out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out << buf;
                } else {
                    out << (char)c;
                }
        }
    }
    return out.str();
}

} // namespace strutil
