#include "ber.h"
#include <sstream>
#include <cstdlib>

namespace ber {

Bytes EncodeLength(size_t length) {
    Bytes out;
    if (length < 0x80) {
        out.push_back((uint8_t)length);
        return out;
    }
    Bytes lenBytes;
    size_t v = length;
    while (v > 0) {
        lenBytes.insert(lenBytes.begin(), (uint8_t)(v & 0xFF));
        v >>= 8;
    }
    out.push_back((uint8_t)(0x80 | lenBytes.size()));
    out.insert(out.end(), lenBytes.begin(), lenBytes.end());
    return out;
}

Bytes EncodeTlv(uint8_t tag, const Bytes& value) {
    Bytes out;
    out.push_back(tag);
    Bytes lengthBytes = EncodeLength(value.size());
    out.insert(out.end(), lengthBytes.begin(), lengthBytes.end());
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

Bytes EncodeInteger(uint32_t value) {
    Bytes content;
    if (value == 0) {
        content.push_back(0x00);
    } else {
        uint32_t v = value;
        while (v > 0) {
            content.insert(content.begin(), (uint8_t)(v & 0xFF));
            v >>= 8;
        }
        if (content[0] & 0x80) content.insert(content.begin(), 0x00); // stay nonnegative
    }
    return EncodeTlv(TAG_INTEGER, content);
}

Bytes EncodeOctetString(const std::string& value) {
    Bytes content(value.begin(), value.end());
    return EncodeTlv(TAG_OCTET_STRING, content);
}

Bytes EncodeNull() {
    return EncodeTlv(TAG_NULL, Bytes());
}

namespace {
// Appends `value` to `out` using the base-128 continuation-bit encoding
// shared by every OID subidentifier, including the merged first one
// (X.690 lets the merged "40*X+Y" value exceed 127 when X==2, e.g. arc
// "2.48" -> 128, which itself needs the multi-byte form).
void AppendBase128(Bytes& out, unsigned long value) {
    uint8_t buf[10];
    int n = 0;
    if (value == 0) {
        buf[n++] = 0x00;
    } else {
        unsigned long tmp = value;
        while (tmp > 0) {
            buf[n++] = (uint8_t)(tmp & 0x7F);
            tmp >>= 7;
        }
    }
    for (int j = n - 1; j >= 0; j--) {
        uint8_t b = buf[j];
        if (j != 0) b |= 0x80;
        out.push_back(b);
    }
}
} // namespace

Bytes EncodeOid(const std::string& dottedOid) {
    std::vector<unsigned long> parts;
    std::stringstream ss(dottedOid);
    std::string token;
    while (std::getline(ss, token, '.')) {
        if (token.empty()) continue;
        parts.push_back(strtoul(token.c_str(), nullptr, 10));
    }

    Bytes content;
    if (parts.size() >= 2) {
        AppendBase128(content, parts[0] * 40 + parts[1]);
        for (size_t i = 2; i < parts.size(); i++) {
            AppendBase128(content, parts[i]);
        }
    }
    return EncodeTlv(TAG_OID, content);
}

TlvView ParseTlv(const uint8_t* data, size_t size, size_t offset) {
    TlvView v;
    if (offset + 2 > size) return v;

    v.tag = data[offset];
    uint8_t lenByte = data[offset + 1];
    size_t headerLen = 2;
    size_t length = 0;

    if ((lenByte & 0x80) == 0) {
        length = lenByte;
    } else {
        size_t numLenBytes = lenByte & 0x7F;
        // Cap at 4 length-bytes (values above 4GB never occur in a ~2KB SNMP
        // datagram); this also keeps `length` itself bounded so the check
        // below can't overflow on a 32-bit size_t (x86 builds).
        if (numLenBytes == 0 || numLenBytes > 4 || offset + 2 + numLenBytes > size) return v;
        for (size_t i = 0; i < numLenBytes; i++) {
            length = (length << 8) | data[offset + 2 + i];
        }
        headerLen = 2 + numLenBytes;
    }

    // offset + headerLen is already known <= size at this point (checked
    // above); compare via subtraction instead of addition so a huge
    // (attacker-controlled) `length` can't wrap size_t and pass the check.
    if (length > size - offset - headerLen) return v; // truncated

    v.valueOffset = offset + headerLen;
    v.valueLength = length;
    v.totalLength = headerLen + length;
    v.ok = true;
    return v;
}

std::string DecodeOid(const uint8_t* data, size_t len) {
    if (len == 0) return "";
    std::ostringstream out;

    // The merged first identifier ("40*X+Y") is itself base-128 encoded and
    // can span multiple bytes (e.g. arc "2.48" -> 128), so decode it with
    // the same continuation-bit loop as every other subidentifier before
    // splitting it back into X.Y.
    size_t i = 0;
    unsigned long first = 0;
    while (i < len) {
        first = (first << 7) | (data[i] & 0x7F);
        bool last = (data[i] & 0x80) == 0;
        i++;
        if (last) break;
    }
    unsigned long x = (first < 80) ? (first / 40) : 2;
    unsigned long y = first - x * 40;
    out << x << "." << y;

    unsigned long value = 0;
    for (; i < len; i++) {
        value = (value << 7) | (data[i] & 0x7F);
        if ((data[i] & 0x80) == 0) {
            out << "." << value;
            value = 0;
        }
    }
    return out.str();
}

int64_t DecodeIntegerValue(const uint8_t* data, size_t len, bool isSigned) {
    if (len == 0) return 0;
    if (len > 8) len = 8; // clamp; SNMP integers we handle never legitimately need more than 4-5 bytes
    // Accumulate into an unsigned type: left-shifting a negative signed
    // value is undefined behavior pre-C++20, even though it happens to work
    // on MSVC's two's-complement representation.
    uint64_t value = (isSigned && (data[0] & 0x80)) ? ~0ULL : 0ULL;
    for (size_t i = 0; i < len; i++) {
        value = (value << 8) | data[i];
    }
    return (int64_t)value;
}

std::string DecodeOctetStringValue(const uint8_t* data, size_t len) {
    return std::string(reinterpret_cast<const char*>(data), len);
}

} // namespace ber
