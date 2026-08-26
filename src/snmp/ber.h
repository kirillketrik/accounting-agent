#pragma once
#include <vector>
#include <cstdint>
#include <string>

// Minimal ASN.1 BER encode/decode, just enough for SNMP v1 GET/GetResponse.
// Deliberately hand-rolled (per the brief) instead of pulling in net-snmp,
// porting the encoding logic already validated in the PowerShell prototype.
namespace ber {

using Bytes = std::vector<uint8_t>;

constexpr uint8_t TAG_INTEGER = 0x02;
constexpr uint8_t TAG_OCTET_STRING = 0x04;
constexpr uint8_t TAG_NULL = 0x05;
constexpr uint8_t TAG_OID = 0x06;
constexpr uint8_t TAG_SEQUENCE = 0x30;

// RFC 1155 / SNMPv1 application-class types (used for varbind values).
constexpr uint8_t TAG_COUNTER32 = 0x41;
constexpr uint8_t TAG_GAUGE32   = 0x42;
constexpr uint8_t TAG_TIMETICKS = 0x43;
constexpr uint8_t TAG_OPAQUE    = 0x44;
constexpr uint8_t TAG_COUNTER64 = 0x46;

// SNMP PDU types (context-class, constructed).
constexpr uint8_t TAG_GET_REQUEST  = 0xA0;
constexpr uint8_t TAG_GET_NEXT     = 0xA1;
constexpr uint8_t TAG_GET_RESPONSE = 0xA2;
constexpr uint8_t TAG_SET_REQUEST  = 0xA3;

Bytes EncodeLength(size_t length);
Bytes EncodeTlv(uint8_t tag, const Bytes& value);

// Nonnegative integers only (sufficient for SNMP v1 version/request-id/
// error-status/error-index in a GET request - this client never sends SET).
Bytes EncodeInteger(uint32_t value);
Bytes EncodeOctetString(const std::string& value);
Bytes EncodeNull();
Bytes EncodeOid(const std::string& dottedOid);
std::string DecodeOid(const uint8_t* data, size_t len);

struct TlvView {
    uint8_t tag = 0;
    size_t valueOffset = 0;
    size_t valueLength = 0;
    size_t totalLength = 0; // tag + length + value bytes consumed from `offset`
    bool ok = false;
};

// Parses one TLV starting at `offset`. Never throws; ok=false on malformed
// or truncated input (bounds are checked against `size`).
TlvView ParseTlv(const uint8_t* data, size_t size, size_t offset);

// Decodes an INTEGER (sign-extended) or an unsigned application type
// (Counter32/Gauge32/TimeTicks - same big-endian encoding, no sign bit).
int64_t DecodeIntegerValue(const uint8_t* data, size_t len, bool isSigned);
std::string DecodeOctetStringValue(const uint8_t* data, size_t len);

} // namespace ber
