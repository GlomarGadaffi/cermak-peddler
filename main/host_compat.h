#ifndef HOST_COMPAT_H
#define HOST_COMPAT_H

// Provide MAC formatting macros for host builds / static analysis when ESP macros are absent
#ifndef MACSTR
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#endif

#ifndef MAC2STR
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#endif

#endif // HOST_COMPAT_H
