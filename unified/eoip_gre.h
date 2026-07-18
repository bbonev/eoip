/*
 *	Shared definitions between the EoIP tunnel module (eoip.c) and the
 *	extended GRE demultiplexer (gre.c).
 *
 *	Protocols with a non-standard GRE header (EoIP) are dispatched by
 *	ids that do not overlap with possible standard GRE protocol
 *	versions (0x00 - 0x7f).
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#ifndef _EOIP_GRE_H
#define _EOIP_GRE_H

#ifndef GREPROTO_NONSTD_BASE
#define GREPROTO_NONSTD_BASE	0x80
#define GREPROTO_NONSTD_EOIP	(0 + GREPROTO_NONSTD_BASE)
#define GREPROTO_NONSTD_MAX	(1 + GREPROTO_NONSTD_BASE)
#endif

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

#endif
