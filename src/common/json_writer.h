#pragma once
#include <string>
#include <sstream>
#include <cstdint>
#include <vector>
#include <type_traits>
#include "string_utils.h"

// Minimal append-only JSON writer. We only ever *produce* JSON (the outbound
// report payload); there is no need for a general-purpose parser, so this
// avoids pulling in a third-party JSON library.
//
// Usage:
//   JsonWriter w;
//   w.BeginObject();
//     w.Field("name", "value");
//     w.BeginArray("items");
//       w.BeginObjectElement();
//         w.Field("x", 1);
//       w.EndObject();
//     w.EndArray();
//   w.EndObject();
//   std::string json = w.Str();
class JsonWriter {
public:
    void BeginObject() {
        Comma();
        out_ << "{";
        needComma_.push_back(false);
    }
    void EndObject() {
        out_ << "}";
        needComma_.pop_back();
        if (!needComma_.empty()) needComma_.back() = true;
    }
    void BeginArray(const std::string& key) {
        Comma();
        out_ << Key(key) << "[";
        needComma_.push_back(false);
    }
    void BeginArray() { // unkeyed (array element that is itself an array) - rarely needed
        Comma();
        out_ << "[";
        needComma_.push_back(false);
    }
    void EndArray() {
        out_ << "]";
        needComma_.pop_back();
        if (!needComma_.empty()) needComma_.back() = true;
    }
    // Start an object as an element of the currently-open array.
    void BeginObjectElement() {
        Comma();
        out_ << "{";
        needComma_.push_back(false);
    }

    void Field(const std::string& key, const std::string& value) {
        Comma();
        out_ << Key(key) << "\"" << strutil::JsonEscape(value) << "\"";
    }
    void Field(const std::string& key, const char* value) { Field(key, std::string(value)); }
    // Deleted rather than left to fall through to the bool overload: a
    // std::string parameter with a `const wchar_t*` argument doesn't compile,
    // but bool does (via an unwanted pointer-to-bool conversion), so an
    // accidental wide-string literal would otherwise silently emit `true`.
    void Field(const std::string& key, const wchar_t* value) = delete;

    void Field(const std::string& key, bool value) {
        Comma();
        out_ << Key(key) << (value ? "true" : "false");
    }

    // Single constrained template instead of a hand-enumerated set of
    // integer overloads (int/int64_t/uint32_t/...): every call site used to
    // need an exact-match overload to avoid ambiguity - e.g. a size_t or
    // unsigned long argument had no exact match and would fail to compile
    // with "ambiguous call". This accepts any integral type except bool
    // (handled above) and formats it as a JSON number.
    template <typename T, typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value, int>::type = 0>
    void Field(const std::string& key, T value) {
        Comma();
        out_ << Key(key) << (int64_t)value;
    }

    void FieldNull(const std::string& key) {
        Comma();
        out_ << Key(key) << "null";
    }
    void ArrayValue(const std::string& value) {
        Comma();
        out_ << "\"" << strutil::JsonEscape(value) << "\"";
    }

    std::string Str() const { return out_.str(); }

private:
    std::string Key(const std::string& key) {
        return "\"" + strutil::JsonEscape(key) + "\":";
    }
    void Comma() {
        if (!needComma_.empty()) {
            if (needComma_.back()) out_ << ",";
            needComma_.back() = true;
        }
    }

    std::ostringstream out_;
    std::vector<bool> needComma_;
};
