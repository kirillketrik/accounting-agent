// winsock2.h must precede any transitive <windows.h> include (pulled in via
// network_info.h -> common/logger.h) to avoid it dragging in the legacy
// winsock.h and colliding with winsock2.h. CMakeLists.txt also defines
// WIN32_LEAN_AND_MEAN globally, which alone would prevent the collision, but
// this ordering keeps the file correct even if built outside that config.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "network_info.h"
#include "common/string_utils.h"
#include <vector>
#include <cstring>

std::vector<HostAddress> GetHostIpAddresses(Logger& logger) {
    std::vector<HostAddress> result;

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG bufLen = 15000;
    std::vector<BYTE> buffer(bufLen);
    IP_ADAPTER_ADDRESSES* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &bufLen);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufLen);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &bufLen);
    }
    if (rc != NO_ERROR) {
        logger.Error("GetAdaptersAddresses failed, rc=" + std::to_string(rc));
        return result;
    }

    for (IP_ADAPTER_ADDRESSES* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        for (IP_ADAPTER_UNICAST_ADDRESS* ua = adapter->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
            SOCKADDR* sa = ua->Address.lpSockaddr;
            if (sa == nullptr) continue;

            char ipStr[INET6_ADDRSTRLEN] = {0};
            HostAddress ha;
            ha.adapterName = strutil::WideToUtf8(adapter->FriendlyName ? adapter->FriendlyName : L"");

            if (sa->sa_family == AF_INET) {
                sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(sa);
                if (InetNtopA(AF_INET, &sin->sin_addr, ipStr, sizeof(ipStr)) == nullptr) continue;
                if (strcmp(ipStr, "127.0.0.1") == 0) continue; // extra guard; loopback adapter is already skipped
                ha.ip = ipStr;
                ha.isIPv6 = false;
            } else if (sa->sa_family == AF_INET6) {
                sockaddr_in6* sin6 = reinterpret_cast<sockaddr_in6*>(sa);
                if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr)) continue;
                if (InetNtopA(AF_INET6, &sin6->sin6_addr, ipStr, sizeof(ipStr)) == nullptr) continue;
                ha.ip = ipStr;
                ha.isIPv6 = true;
            } else {
                continue;
            }

            result.push_back(ha);
        }
    }

    logger.Debug("Host has " + std::to_string(result.size()) + " active non-loopback IP address(es).");
    return result;
}

std::string GetLocalComputerName() {
    wchar_t buf[256];
    DWORD len = 256;
    if (GetComputerNameW(buf, &len)) {
        return strutil::WideToUtf8(std::wstring(buf, len));
    }
    return "unknown-host";
}
