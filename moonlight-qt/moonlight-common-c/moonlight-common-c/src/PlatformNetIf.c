#include "Limelight-internal.h"

#ifdef VIPLE_MPQUIC

#include "PlatformNetIf.h"

#include <string.h>

const char* lcNetIfTypeName(int type) {
    switch (type) {
        case LC_NETIF_TYPE_ETHERNET: return "Ethernet";
        case LC_NETIF_TYPE_WIFI:     return "Wi-Fi";
        case LC_NETIF_TYPE_CELLULAR: return "Cellular";
        case LC_NETIF_TYPE_LOOPBACK: return "Loopback";
        case LC_NETIF_TYPE_VPN:      return "VPN";
        default:                     return "Unknown";
    }
}

#ifdef _WIN32

#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>

#pragma comment(lib, "iphlpapi.lib")

static int classifyWindowsAdapter(DWORD ifType) {
    switch (ifType) {
        case IF_TYPE_ETHERNET_CSMACD:
        case IF_TYPE_GIGABITETHERNET:
        case IF_TYPE_FASTETHER:
            return LC_NETIF_TYPE_ETHERNET;
        case IF_TYPE_IEEE80211:
            return LC_NETIF_TYPE_WIFI;
        case IF_TYPE_SOFTWARE_LOOPBACK:
            return LC_NETIF_TYPE_LOOPBACK;
        case IF_TYPE_TUNNEL:
        case IF_TYPE_PPP:
            return LC_NETIF_TYPE_VPN;
        case IF_TYPE_WWANPP:
        case IF_TYPE_WWANPP2:
            return LC_NETIF_TYPE_CELLULAR;
        default:
            return LC_NETIF_TYPE_UNKNOWN;
    }
}

int lcEnumNetInterfaces(PLC_NET_INTERFACE out, int maxCount) {
    PIP_ADAPTER_ADDRESSES adapterList = NULL;
    PIP_ADAPTER_ADDRESSES adapter;
    ULONG bufLen = 15000;
    ULONG ret;
    int count = 0;

    adapterList = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
    if (!adapterList) return -1;

    ret = GetAdaptersAddresses(
        AF_UNSPEC,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        NULL, adapterList, &bufLen);

    if (ret == ERROR_BUFFER_OVERFLOW) {
        free(adapterList);
        adapterList = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
        if (!adapterList) return -1;
        ret = GetAdaptersAddresses(
            AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            NULL, adapterList, &bufLen);
    }

    if (ret != NO_ERROR) {
        free(adapterList);
        return -1;
    }

    for (adapter = adapterList; adapter && count < maxCount; adapter = adapter->Next) {
        PIP_ADAPTER_UNICAST_ADDRESS unicast;
        int ifType;

        if (adapter->OperStatus != IfOperStatusUp)
            continue;

        ifType = classifyWindowsAdapter(adapter->IfType);
        if (ifType == LC_NETIF_TYPE_LOOPBACK)
            continue;

        for (unicast = adapter->FirstUnicastAddress;
             unicast && count < maxCount;
             unicast = unicast->Next) {
            struct sockaddr* sa = unicast->Address.lpSockaddr;

            if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)
                continue;
            // Skip link-local IPv6
            if (sa->sa_family == AF_INET6) {
                struct sockaddr_in6* sin6 = (struct sockaddr_in6*)sa;
                if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr))
                    continue;
            }

            memset(&out[count], 0, sizeof(out[count]));
            WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1,
                                out[count].name, LC_NETIF_MAX_NAME, NULL, NULL);
            out[count].index = (int)adapter->IfIndex;
            out[count].type = ifType;
            out[count].family = sa->sa_family;
            memcpy(&out[count].addr, sa, unicast->Address.iSockaddrLength);
            out[count].addrLen = (SOCKADDR_LEN)unicast->Address.iSockaddrLength;
            out[count].metric = (int)(adapter->Ipv4Metric);
            out[count].up = true;
            out[count].preferred = (unicast->DadState == IpDadStatePreferred);
            count++;
        }
    }

    free(adapterList);
    return count;
}

#elif defined(__ANDROID__)

// Android: interface enumeration done via JNI in MultiNetworkAdapter.java.
// This stub returns 0 interfaces; the real list is provided by the Java
// layer and injected via lcSetNetInterfacesFromJni().

static LC_NET_INTERFACE s_jniInterfaces[LC_NETIF_MAX_COUNT];
static int s_jniInterfaceCount = 0;

void lcSetNetInterfacesFromJni(PLC_NET_INTERFACE interfaces, int count) {
    if (count > LC_NETIF_MAX_COUNT)
        count = LC_NETIF_MAX_COUNT;
    memcpy(s_jniInterfaces, interfaces, count * sizeof(LC_NET_INTERFACE));
    s_jniInterfaceCount = count;
}

int lcEnumNetInterfaces(PLC_NET_INTERFACE out, int maxCount) {
    int count = s_jniInterfaceCount;
    if (count > maxCount)
        count = maxCount;
    memcpy(out, s_jniInterfaces, count * sizeof(LC_NET_INTERFACE));
    return count;
}

#else
// POSIX (Linux, macOS, etc.)

#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>

#ifdef __linux__
#include <linux/wireless.h>
#endif

static int classifyPosixInterface(const char* name, unsigned int flags) {
    if (flags & IFF_LOOPBACK)
        return LC_NETIF_TYPE_LOOPBACK;

#ifdef __linux__
    // Try wireless ioctl to distinguish WiFi from Ethernet
    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct iwreq wrq;
            memset(&wrq, 0, sizeof(wrq));
            strncpy(wrq.ifr_name, name, IFNAMSIZ - 1);
            if (ioctl(sock, SIOCGIWNAME, &wrq) == 0) {
                close(sock);
                return LC_NETIF_TYPE_WIFI;
            }
            close(sock);
        }
    }

    // Check sysfs for cellular (wwan) interfaces
    {
        char path[256];
        snprintf(path, sizeof(path), "/sys/class/net/%s/type", name);
        FILE* f = fopen(path, "r");
        if (f) {
            int type = 0;
            if (fscanf(f, "%d", &type) == 1 && type == 512) {
                fclose(f);
                return LC_NETIF_TYPE_CELLULAR;
            }
            fclose(f);
        }
    }
#endif

    if (strncmp(name, "tun", 3) == 0 || strncmp(name, "tap", 3) == 0 ||
        strncmp(name, "wg", 2) == 0 || strncmp(name, "tailscale", 9) == 0)
        return LC_NETIF_TYPE_VPN;

    return LC_NETIF_TYPE_ETHERNET;
}

int lcEnumNetInterfaces(PLC_NET_INTERFACE out, int maxCount) {
    struct ifaddrs* ifList = NULL;
    struct ifaddrs* ifa;
    int count = 0;

    if (getifaddrs(&ifList) != 0)
        return -1;

    for (ifa = ifList; ifa && count < maxCount; ifa = ifa->ifa_next) {
        int ifType;
        SOCKADDR_LEN saLen;

        if (!ifa->ifa_addr)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_RUNNING))
            continue;
        if (ifa->ifa_addr->sa_family != AF_INET && ifa->ifa_addr->sa_family != AF_INET6)
            continue;

        // Skip link-local IPv6
        if (ifa->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6* sin6 = (struct sockaddr_in6*)ifa->ifa_addr;
            if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr))
                continue;
        }

        ifType = classifyPosixInterface(ifa->ifa_name, ifa->ifa_flags);
        if (ifType == LC_NETIF_TYPE_LOOPBACK)
            continue;

        saLen = (ifa->ifa_addr->sa_family == AF_INET)
            ? (SOCKADDR_LEN)sizeof(struct sockaddr_in)
            : (SOCKADDR_LEN)sizeof(struct sockaddr_in6);

        memset(&out[count], 0, sizeof(out[count]));
        PltSafeStrcpy(out[count].name, sizeof(out[count].name), ifa->ifa_name);
        out[count].index = (int)if_nametoindex(ifa->ifa_name);
        out[count].type = ifType;
        out[count].family = ifa->ifa_addr->sa_family;
        memcpy(&out[count].addr, ifa->ifa_addr, saLen);
        out[count].addrLen = saLen;
        out[count].metric = 0;
        out[count].up = true;
        out[count].preferred = true;
        count++;
    }

    freeifaddrs(ifList);
    return count;
}

#endif // platform

#endif // VIPLE_MPQUIC
