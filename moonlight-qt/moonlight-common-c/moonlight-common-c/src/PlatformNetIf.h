#pragma once

#ifdef VIPLE_MPQUIC

#include "Platform.h"
#include "PlatformSockets.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LC_NETIF_MAX_NAME 64
#define LC_NETIF_MAX_COUNT 16

#define LC_NETIF_TYPE_UNKNOWN   0
#define LC_NETIF_TYPE_ETHERNET  1
#define LC_NETIF_TYPE_WIFI      2
#define LC_NETIF_TYPE_CELLULAR  3
#define LC_NETIF_TYPE_LOOPBACK  4
#define LC_NETIF_TYPE_VPN       5

typedef struct _LC_NET_INTERFACE {
    char name[LC_NETIF_MAX_NAME];
    int index;
    int type;
    int family;
    struct sockaddr_storage addr;
    SOCKADDR_LEN addrLen;
    int metric;
    bool up;
    bool preferred;
} LC_NET_INTERFACE, *PLC_NET_INTERFACE;

// Enumerate all usable network interfaces on this host.
// Returns the number of interfaces written to `out` (up to `maxCount`),
// or -1 on error. Only returns interfaces that are up and have an
// address assigned.
int lcEnumNetInterfaces(PLC_NET_INTERFACE out, int maxCount);

// Get a human-readable description for a network interface type.
const char* lcNetIfTypeName(int type);

#ifdef __cplusplus
}
#endif

#endif // VIPLE_MPQUIC
