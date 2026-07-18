#!/bin/sh
#
# Integrate the EoIP and EoIPv6 drivers into a Linux kernel source tree
# for an in-tree build, then configure them with CONFIG_NET_EOIP and
# CONFIG_NET_EOIP6.
#
#   contrib/kernel-intree.sh /path/to/linux-source
#
# Why a script and not a patch: the driver sources are new files, which
# drop into any kernel unchanged, but wiring them into the Makefiles and
# Kconfigs by a context diff would break across the wide range of
# supported kernels (3.2 .. current) because the surrounding lines
# differ.  This script instead appends the obj rules and Kconfig entries,
# which is version independent.  It is idempotent - running it twice is
# harmless - and every driver config carries an explicit "depends on" so
# it is correct regardless of where in the menu it lands.

set -e

KSRC="$1"
SELF=$(cd "$(dirname "$0")/.." && pwd)		# repo root
U="$SELF/unified"

if [ -z "$KSRC" ] || [ ! -f "$KSRC/net/ipv4/Kconfig" ]; then
	echo "usage: $0 /path/to/linux-source" >&2
	exit 1
fi

echo "integrating EoIP into $KSRC"

# 1. drop in the sources (add-only, version independent)
cp "$SELF/eoip_version.h"    "$KSRC/net/eoip_version.h"
cp "$U/eoip_proto.h" "$U/eoip_keepalive.h" "$U/eoip.c"    "$KSRC/net/ipv4/"
cp "$U/eoip_proto.h" "$U/eoip_keepalive.h" "$U/eoipv6.c"  "$KSRC/net/ipv6/"

# 2. wire the Makefiles (obj rule order is irrelevant, so append)
grep -q 'CONFIG_NET_EOIP)' "$KSRC/net/ipv4/Makefile" ||
	echo 'obj-$(CONFIG_NET_EOIP) += eoip.o' >> "$KSRC/net/ipv4/Makefile"
grep -q 'CONFIG_NET_EOIP6)' "$KSRC/net/ipv6/Makefile" ||
	echo 'obj-$(CONFIG_NET_EOIP6) += eoipv6.o' >> "$KSRC/net/ipv6/Makefile"

# 3. wire the Kconfigs (append the entries with explicit dependencies)
grep -q 'config NET_EOIP$' "$KSRC/net/ipv4/Kconfig" || cat >> "$KSRC/net/ipv4/Kconfig" <<'EOF'

config NET_EOIP
	tristate "IP: EOIP ethernet tunnels over IP"
	depends on INET && NETFILTER
	help
	  Tunneling means encapsulating data of one protocol type within
	  another protocol and sending it over a channel that understands the
	  encapsulating protocol. This particular tunneling driver implements
	  MikroTik RouterOS compatible encapsulation of ethernet frames over
	  existing IPv4 infrastructure. It is useful if the other endpoint is
	  a MikroTik router. The module is standalone and intercepts its
	  packets with a netfilter hook, coexisting with the standard gre and
	  pptp modules.

	  To compile it as a module, choose M here. If unsure, say N.
EOF

grep -q 'config NET_EOIP6$' "$KSRC/net/ipv6/Kconfig" || cat >> "$KSRC/net/ipv6/Kconfig" <<'EOF'

config NET_EOIP6
	tristate "IPv6: EOIPv6 ethernet tunnels over IPv6"
	depends on IPV6
	help
	  Tunneling means encapsulating data of one protocol type within
	  another protocol and sending it over a channel that understands the
	  encapsulating protocol. This particular tunneling driver implements
	  MikroTik RouterOS compatible encapsulation of ethernet frames over
	  existing IPv6 infrastructure (EoIPv6, IPv6 protocol 97). It is
	  useful if the other endpoint is a MikroTik router.

	  To compile it as a module, choose M here. If unsure, say N.
EOF

echo "done - now enable NET_EOIP / NET_EOIP6 in your kernel config"
echo "  (e.g. scripts/config --module NET_EOIP --module NET_EOIP6)"
