// winsock2.h must precede any transitive <windows.h> include (pulled in via
// snmp_client.h -> common/logger.h). See network_info.cpp for the same note;
// CMakeLists.txt's global WIN32_LEAN_AND_MEAN already prevents the classic
// collision, this ordering just keeps the file correct independent of that.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "snmp_client.h"
#include "ber.h"
#include <windows.h>
#include <vector>

namespace {

struct ParsedVarbind {
    std::string oid;
    uint8_t valueTag = 0;
    const uint8_t* valueData = nullptr;
    size_t valueLen = 0;
};

// One GetRequest carries exactly one OID. SNMPv1 has no per-varbind error
// reporting - if a device doesn't implement one OID in a multi-varbind
// request, error-status applies to the *whole* PDU and no values come back
// for any of them. Querying sysDescr and prtMarkerLifeCount as two
// independent GETs means an unsupported page-count OID doesn't also cost us
// sysDescr (and vice versa), at the cost of up to 2x the round trips.
ber::Bytes BuildOidRequest(uint8_t pduTag, const std::string& community, uint32_t requestId, const std::string& oid) {
    ber::Bytes pair;
    ber::Bytes oidBytes = ber::EncodeOid(oid);
    ber::Bytes nullBytes = ber::EncodeNull();
    pair.insert(pair.end(), oidBytes.begin(), oidBytes.end());
    pair.insert(pair.end(), nullBytes.begin(), nullBytes.end());
    ber::Bytes varbindsSeq = ber::EncodeTlv(ber::TAG_SEQUENCE, ber::EncodeTlv(ber::TAG_SEQUENCE, pair));

    ber::Bytes pduContent;
    ber::Bytes reqId = ber::EncodeInteger(requestId);
    ber::Bytes errStatus = ber::EncodeInteger(0);
    ber::Bytes errIndex = ber::EncodeInteger(0);
    pduContent.insert(pduContent.end(), reqId.begin(), reqId.end());
    pduContent.insert(pduContent.end(), errStatus.begin(), errStatus.end());
    pduContent.insert(pduContent.end(), errIndex.begin(), errIndex.end());
    pduContent.insert(pduContent.end(), varbindsSeq.begin(), varbindsSeq.end());
    ber::Bytes pdu = ber::EncodeTlv(pduTag, pduContent);

    ber::Bytes msgContent;
    ber::Bytes version = ber::EncodeInteger(0); // SNMP v1
    ber::Bytes comm = ber::EncodeOctetString(community);
    msgContent.insert(msgContent.end(), version.begin(), version.end());
    msgContent.insert(msgContent.end(), comm.begin(), comm.end());
    msgContent.insert(msgContent.end(), pdu.begin(), pdu.end());

    return ber::EncodeTlv(ber::TAG_SEQUENCE, msgContent);
}

ber::Bytes BuildGetRequest(const std::string& community, uint32_t requestId, const std::string& oid) {
    return BuildOidRequest(ber::TAG_GET_REQUEST, community, requestId, oid);
}

ber::Bytes BuildGetNextRequest(const std::string& community, uint32_t requestId, const std::string& oid) {
    return BuildOidRequest(ber::TAG_GET_NEXT, community, requestId, oid);
}

bool ParseGetResponse(const uint8_t* data, size_t size, uint32_t& outRequestId,
                       int64_t& outErrorStatus, int64_t& outErrorIndex,
                       std::vector<ParsedVarbind>& outVarbinds, std::string& errorOut) {
    ber::TlvView msg = ber::ParseTlv(data, size, 0);
    if (!msg.ok || msg.tag != ber::TAG_SEQUENCE) { errorOut = "malformed SNMP message envelope"; return false; }

    size_t pos = msg.valueOffset;
    ber::TlvView version = ber::ParseTlv(data, size, pos);
    if (!version.ok) { errorOut = "malformed SNMP version field"; return false; }
    pos = version.valueOffset + version.valueLength;

    ber::TlvView community = ber::ParseTlv(data, size, pos);
    if (!community.ok) { errorOut = "malformed SNMP community field"; return false; }
    pos = community.valueOffset + community.valueLength;

    ber::TlvView pdu = ber::ParseTlv(data, size, pos);
    if (!pdu.ok) { errorOut = "malformed SNMP PDU"; return false; }
    if (pdu.tag != ber::TAG_GET_RESPONSE) { errorOut = "response is not a GetResponse PDU"; return false; }

    size_t pduPos = pdu.valueOffset;
    ber::TlvView reqId = ber::ParseTlv(data, size, pduPos);
    if (!reqId.ok) { errorOut = "malformed request-id"; return false; }
    outRequestId = (uint32_t)ber::DecodeIntegerValue(data + reqId.valueOffset, reqId.valueLength, true);
    pduPos = reqId.valueOffset + reqId.valueLength;

    ber::TlvView errStat = ber::ParseTlv(data, size, pduPos);
    if (!errStat.ok) { errorOut = "malformed error-status"; return false; }
    outErrorStatus = ber::DecodeIntegerValue(data + errStat.valueOffset, errStat.valueLength, true);
    pduPos = errStat.valueOffset + errStat.valueLength;

    ber::TlvView errIdx = ber::ParseTlv(data, size, pduPos);
    if (!errIdx.ok) { errorOut = "malformed error-index"; return false; }
    outErrorIndex = ber::DecodeIntegerValue(data + errIdx.valueOffset, errIdx.valueLength, true);
    pduPos = errIdx.valueOffset + errIdx.valueLength;

    ber::TlvView varbinds = ber::ParseTlv(data, size, pduPos);
    if (!varbinds.ok) { errorOut = "malformed variable-bindings list"; return false; }

    size_t vbPos = varbinds.valueOffset;
    size_t vbEnd = varbinds.valueOffset + varbinds.valueLength;
    while (vbPos < vbEnd) {
        ber::TlvView vb = ber::ParseTlv(data, size, vbPos);
        if (!vb.ok) break;

        size_t inner = vb.valueOffset;
        ber::TlvView oidTlv = ber::ParseTlv(data, size, inner);
        if (!oidTlv.ok) break;
        inner = oidTlv.valueOffset + oidTlv.valueLength;
        ber::TlvView valTlv = ber::ParseTlv(data, size, inner);
        if (!valTlv.ok) break;

        ParsedVarbind pv;
        pv.oid = ber::DecodeOid(data + oidTlv.valueOffset, oidTlv.valueLength);
        pv.valueTag = valTlv.tag;
        pv.valueData = data + valTlv.valueOffset;
        pv.valueLen = valTlv.valueLength;
        outVarbinds.push_back(pv);

        vbPos += vb.totalLength;
    }
    return true;
}

bool ResolveTarget(const SnmpTarget& target, sockaddr_storage& addrOut, int& addrLenOut, std::string& errorOut) {
    ZeroMemory(&addrOut, sizeof(addrOut));

    if (target.isIPv6) {
        sockaddr_in6* a6 = reinterpret_cast<sockaddr_in6*>(&addrOut);
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons(target.port);
        a6->sin6_scope_id = target.scopeId;
        if (InetPtonA(AF_INET6, target.host.c_str(), &a6->sin6_addr) != 1) {
            errorOut = "invalid IPv6 literal: " + target.host;
            return false;
        }
        addrLenOut = sizeof(sockaddr_in6);
        return true;
    }

    sockaddr_in* a4 = reinterpret_cast<sockaddr_in*>(&addrOut);
    a4->sin_family = AF_INET;
    a4->sin_port = htons(target.port);
    if (InetPtonA(AF_INET, target.host.c_str(), &a4->sin_addr) == 1) {
        addrLenOut = sizeof(sockaddr_in);
        return true;
    }

    // Not a numeric literal - try standard DNS. mDNS/Bonjour ".local" names
    // (e.g. "Pantum-4A6A29") are a known, deferred limitation: getaddrinfo
    // will not resolve them, and that failure is reported back as-is.
    ADDRINFOA hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    ADDRINFOA* res = nullptr;
    int rc = getaddrinfo(target.host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || res == nullptr) {
        errorOut = "could not determine IP: DNS resolution failed for '" + target.host + "'";
        return false;
    }
    a4->sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    addrLenOut = sizeof(sockaddr_in);
    return true;
}

// Sleeps for `ms`, or returns early (true) if `stopEvent` is signaled first.
// With stopEvent == nullptr this is a plain Sleep (console/no-shutdown-signal case).
bool InterruptibleWait(HANDLE stopEvent, int ms) {
    if (!stopEvent) {
        Sleep((DWORD)ms);
        return false;
    }
    return WaitForSingleObject(stopEvent, (DWORD)ms) == WAIT_OBJECT_0;
}

struct SingleOidResult {
    bool success = false;    // a well-formed, request-id-matched GetResponse was received
    bool stopping = false;   // aborted early because the service is shutting down
    std::string error;
    int64_t errorStatus = 0;
    bool hasValue = false;
    std::string resultOid;   // the OID the value actually came from (== `oid` for GET; the "next" OID for GETNEXT)
    uint8_t valueTag = 0;
    std::string octetValue;  // valid when valueTag == TAG_OCTET_STRING
    int64_t intValue = 0;    // valid for INTEGER / Counter32 / Gauge32 / TimeTicks
};

// Runs the full send/wait/retry loop for a single OID against an already
// connect()-ed UDP socket (so recv() only ever returns datagrams that
// actually came from the target, not an off-path spoofed reply). When
// `getNext` is set, sends a GETNEXT instead of a GET - the device chooses
// which OID actually answers (the lexicographically next one after `oid`),
// reported back via `resultOid`, so callers must check it's still within
// whatever subtree they care about before trusting the value.
SingleOidResult SnmpGetSingleOid(SOCKET sock, const std::string& oid, const SnmpOptions& options,
                                  Logger& logger, const std::string& printerNameForLogging, bool getNext = false) {
    SingleOidResult result;
    int attempts = options.retries < 1 ? 1 : options.retries;

    for (int attempt = 1; attempt <= attempts; attempt++) {
        // Masked to 31 bits: SNMP's request-id is an SMI INTEGER (-2^31..2^31-1);
        // an unmasked 32-bit pattern with the top bit set would BER-encode to a
        // 5-byte value outside that range, which strict agents can reject.
        uint32_t requestId = ((uint32_t)GetTickCount() ^ ((uint32_t)attempt * 2654435761u)) & 0x7FFFFFFFu;
        ber::Bytes request = getNext ? BuildGetNextRequest(options.community, requestId, oid)
                                      : BuildGetRequest(options.community, requestId, oid);

        int sent = send(sock, reinterpret_cast<const char*>(request.data()), (int)request.size(), 0);
        if (sent == SOCKET_ERROR) {
            logger.Warn("SNMP for '" + printerNameForLogging + "': send failed (attempt " +
                std::to_string(attempt) + "/" + std::to_string(attempts) +
                "), WSAGetLastError=" + std::to_string(WSAGetLastError()));
            if (attempt < attempts && InterruptibleWait(options.stopEvent, options.retryDelayMs)) {
                result.stopping = true;
                return result;
            }
            continue;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval tv;
        tv.tv_sec = options.timeoutMs / 1000;
        tv.tv_usec = (options.timeoutMs % 1000) * 1000;

        int selectRc = select(0, &readSet, nullptr, nullptr, &tv);
        if (selectRc <= 0) {
            // Printer in power-save mode, or transient AV/EDR interference with
            // raw UDP - expected occasionally, hence the retry loop (see brief).
            logger.Debug("SNMP for '" + printerNameForLogging + "' [" + oid + "]: no response (attempt " +
                std::to_string(attempt) + "/" + std::to_string(attempts) + ")");
            if (attempt < attempts && InterruptibleWait(options.stopEvent, options.retryDelayMs)) {
                result.stopping = true;
                return result;
            }
            continue;
        }

        uint8_t buffer[4096];
        int received = recv(sock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
        if (received <= 0) {
            if (attempt < attempts && InterruptibleWait(options.stopEvent, options.retryDelayMs)) {
                result.stopping = true;
                return result;
            }
            continue;
        }

        uint32_t respRequestId = 0;
        int64_t errorStatus = 0, errorIndex = 0;
        std::vector<ParsedVarbind> varbinds;
        std::string parseError;
        if (!ParseGetResponse(buffer, (size_t)received, respRequestId, errorStatus, errorIndex, varbinds, parseError)) {
            logger.Warn("SNMP for '" + printerNameForLogging + "' [" + oid + "]: " + parseError);
            if (attempt < attempts && InterruptibleWait(options.stopEvent, options.retryDelayMs)) {
                result.stopping = true;
                return result;
            }
            continue;
        }

        if (respRequestId != requestId) {
            // Stale reply to an earlier attempt, or noise from something else
            // entirely - not a match for the request we just sent.
            logger.Debug("SNMP for '" + printerNameForLogging + "' [" + oid + "]: ignoring response with mismatched request-id");
            if (attempt < attempts && InterruptibleWait(options.stopEvent, options.retryDelayMs)) {
                result.stopping = true;
                return result;
            }
            continue;
        }

        result.success = true;
        result.errorStatus = errorStatus;
        if (errorStatus != 0) {
            result.error = "SNMP agent returned error-status=" + std::to_string(errorStatus) +
                " for OID " + oid + " (likely unsupported by this device)";
            return result;
        }

        for (const auto& vb : varbinds) {
            // For a plain GET the reply echoes the OID we asked for; for a
            // GETNEXT the device answers with whatever OID actually follows
            // `oid`, so there is nothing to match against - just take it.
            if (!getNext && vb.oid != oid) continue;
            result.hasValue = true;
            result.resultOid = vb.oid;
            result.valueTag = vb.valueTag;
            if (vb.valueTag == ber::TAG_OCTET_STRING) {
                result.octetValue = ber::DecodeOctetStringValue(vb.valueData, vb.valueLen);
            } else {
                result.intValue = ber::DecodeIntegerValue(vb.valueData, vb.valueLen, vb.valueTag == ber::TAG_INTEGER);
            }
            break;
        }
        return result;
    }

    result.error = "no SNMP response for OID " + oid + " after " + std::to_string(attempts) + " attempt(s) (timeout)";
    return result;
}

} // namespace

bool SnmpGlobalInit(Logger& logger) {
    WSADATA wsaData;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (rc != 0) {
        logger.Error("SNMP: WSAStartup failed, rc=" + std::to_string(rc));
        return false;
    }
    return true;
}

void SnmpGlobalCleanup() {
    WSACleanup();
}

SnmpGetResult SnmpGetSysDescrAndPageCount(const SnmpTarget& target, const SnmpOptions& options,
                                           Logger& logger, const std::string& printerNameForLogging) {
    SnmpGetResult result;

    sockaddr_storage addr{};
    int addrLen = 0;
    std::string resolveError;
    if (!ResolveTarget(target, addr, addrLen, resolveError)) {
        result.hostResolved = false;
        result.error = resolveError;
        logger.Warn("SNMP for '" + printerNameForLogging + "': " + resolveError);
        return result;
    }

    int family = target.isIPv6 ? AF_INET6 : AF_INET;
    SOCKET sock = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        result.error = "socket() failed, WSAGetLastError=" + std::to_string(WSAGetLastError());
        logger.Error("SNMP for '" + printerNameForLogging + "': " + result.error);
        return result;
    }

    // Connecting the UDP socket makes the OS filter recv() to datagrams that
    // actually originate from `addr`, closing off spoofed-reply injection
    // from any other host that can reach our ephemeral source port.
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), addrLen) == SOCKET_ERROR) {
        result.error = "connect() failed, WSAGetLastError=" + std::to_string(WSAGetLastError());
        logger.Error("SNMP for '" + printerNameForLogging + "': " + result.error);
        closesocket(sock);
        return result;
    }

    SingleOidResult sysDescrResult = SnmpGetSingleOid(sock, snmp_oids::kSysDescr, options, logger, printerNameForLogging);
    SingleOidResult pageCountResult = sysDescrResult.stopping
        ? SingleOidResult{}
        : SnmpGetSingleOid(sock, snmp_oids::kPrtMarkerLifeCount, options, logger, printerNameForLogging);

    bool stopping = sysDescrResult.stopping || pageCountResult.stopping;

    result.success = sysDescrResult.success || pageCountResult.success;

    if (sysDescrResult.success && sysDescrResult.errorStatus == 0 &&
        sysDescrResult.hasValue && sysDescrResult.valueTag == ber::TAG_OCTET_STRING) {
        result.sysDescr = sysDescrResult.octetValue;
    }

    if (pageCountResult.success && pageCountResult.errorStatus == 0 && pageCountResult.hasValue) {
        uint8_t t = pageCountResult.valueTag;
        if (t == ber::TAG_INTEGER || t == ber::TAG_COUNTER32 || t == ber::TAG_GAUGE32 || t == ber::TAG_TIMETICKS) {
            result.hasPageCount = true;
            result.pageCount = pageCountResult.intValue;
        }
    }

    // Some devices don't have a marker at index "1.1" (e.g. per-color-plane
    // markers indexed differently), so the direct GET above finds nothing
    // even though the device has a real, usable counter elsewhere in the
    // same table. Fall back to walking the column with GETNEXT and take
    // whatever the device says actually comes right after it, as long as
    // that's still inside the prtMarkerLifeCount table (not some unrelated
    // OID further along the MIB tree on a device that has no such table at all).
    if (!result.hasPageCount && !stopping) {
        SingleOidResult walked = SnmpGetSingleOid(sock, snmp_oids::kPrtMarkerLifeCountColumn, options, logger,
                                                    printerNameForLogging, /*getNext=*/true);
        if (walked.stopping) {
            stopping = true;
        } else if (walked.success && walked.errorStatus == 0 && walked.hasValue &&
                   walked.resultOid.rfind(std::string(snmp_oids::kPrtMarkerLifeCountColumn) + ".", 0) == 0) {
            uint8_t t = walked.valueTag;
            if (t == ber::TAG_INTEGER || t == ber::TAG_COUNTER32 || t == ber::TAG_GAUGE32 || t == ber::TAG_TIMETICKS) {
                result.hasPageCount = true;
                result.pageCount = walked.intValue;
                result.success = true;
                logger.Debug("SNMP for '" + printerNameForLogging + "': page count found via GETNEXT walk at " +
                    walked.resultOid + " (not the expected .1.1 index)");
            }
        }
    }

    closesocket(sock);

    if (stopping) {
        result.error = "collection interrupted by service shutdown";
        return result;
    }

    if (!result.hasPageCount) {
        // The page count is the field we actually care about; surface its
        // diagnostic first, falling back to sysDescr's if that's all we have.
        if (!pageCountResult.error.empty()) result.error = pageCountResult.error;
        else if (!sysDescrResult.error.empty()) result.error = sysDescrResult.error;
    }

    if (!result.success) {
        logger.Warn("SNMP for '" + printerNameForLogging + "': no usable response for either OID");
    }

    return result;
}
