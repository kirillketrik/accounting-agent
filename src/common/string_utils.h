#pragma once
#include <string>

// UTF-8 <-> UTF-16 conversion helpers. All internal string handling uses
// UTF-8 (std::string); Win32/WMI/registry APIs need UTF-16 (std::wstring).
namespace strutil {

std::wstring Utf8ToWide(const std::string& utf8);
std::string WideToUtf8(const std::wstring& wide);

// Case-insensitive ASCII helpers (printer/port names are ASCII in practice).
bool IContains(const std::string& haystack, const std::string& needle);
bool IEquals(const std::string& a, const std::string& b);
bool IStartsWith(const std::string& s, const std::string& prefix);

std::string ToLower(const std::string& s);
std::string Trim(const std::string& s);

// JSON string escaping (quotes, backslashes, control chars).
std::string JsonEscape(const std::string& s);

} // namespace strutil
