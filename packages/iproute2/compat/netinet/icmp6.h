/*
 * The one constant ip/ipprefix.c needs from <netinet/icmp6.h>.
 *
 * uClibc-ng installs no netinet/icmp6.h at all, because it is configured here
 * without IPv6 (__UCLIBC_HAS_IPV6__ undefined). That is not an oversight: this
 * device KERNEL has no IPv6 either -- measured on the stock kernel, no inet6,
 * no icmpv6, no ndisc, no tcp_v6 -- and busybox is configured the same way for
 * the same reason.
 *
 * ipprefix.c handles RTM_NEWPREFIX, the router-advertisement prefix messages a
 * kernel with IPv6 sends. It cannot fire here. It still has to compile,
 * because ip/ip.c names its handler in a table built unconditionally.
 *
 * The value is the IANA neighbour-discovery option type, which is the same
 * everywhere and is what the message carries.
 */
#ifndef ODI_NETINET_ICMP6_H
#define ODI_NETINET_ICMP6_H

/* Neighbour discovery option types (RFC 4861). */
#define ND_OPT_SOURCE_LINKADDR		1
#define ND_OPT_TARGET_LINKADDR		2
#define ND_OPT_PREFIX_INFORMATION	3
#define ND_OPT_REDIRECTED_HEADER	4
#define ND_OPT_MTU			5

#endif
