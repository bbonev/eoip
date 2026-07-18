/*
 *	Wire-format and rtnl attribute definitions shared by the EoIP
 *	(eoip.c) and EoIPv6 (eoipv6.c) modules and the userland tool.
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#ifndef _EOIP_PROTO_H
#define _EOIP_PROTO_H

/* The EoIP pseudo-GRE header:
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |       GRE FLAGS 0x20 0x01     |      Protocol Type  0x6400    |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |   Encapsulated frame length   |     Tunnel ID (little end.)   |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | Ethernet frame...                                             |
 */
#define EOIP_MAGIC	"\x20\x01\x64\x00"
#define EOIP_HDR_LEN	8

/* The EoIPv6 header (IPv6 protocol 97) is 2 bytes: the low nibble of the
 * first octet is the version (3), the high nibble and the second octet
 * carry the 12-bit tunnel id:
 *
 *  0                   1
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | TID_H |0 0 1 1|     TID_L     |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | Ethernet frame...             |
 */
#define EOIP6_HDR_LEN	2
#define EOIP6_VERSION	0x3
#define EOIP6_TID_MAX	0xFFF

/* eoip/eoipv6 rtnl link attributes.  The link-kind attribute namespace is
 * private per kind; ids up to IFLA_GRE_MAX reuse the identically numbered
 * IFLA_GRE_* values, the range up to 31 is reserved for mirroring future
 * IFLA_GRE_* additions and private attributes start at a fixed 32 so the
 * wire ids do not depend on the if_tunnel.h generation either side was
 * built with.
 */
#define IFLA_EOIP_KA_INTERVAL	32	/* u32, seconds, 0 = keepalive off */
#define IFLA_EOIP_KA_RETRIES	33	/* u8, 0 = send-only (no carrier drop) */
#define IFLA_EOIP_ATTR_MAX	33

#define EOIP_KA_MAX_INTERVAL	3600	/* keeps interval*retries*HZ in 32-bit jiffies math */
#define EOIP_KA_DEF_RETRIES	10	/* RouterOS default is keepalive=10s,10 */

#endif
