EOIP
====

Kernel mode EOIP (Ethernet Over IP) tunnel compatible with MikroTik RouterOS

There are several projects doing the same job with userland utilities via tap interfaces and raw sockets. While a userland application is easier to install and maintain it lacks the performance and stability of an in-kernel module. Especially for the simple job of adding and stripping the EOIP tunneling headers. The userland tunneling application may be good for testing, research or concept proof projects but not suitable for production environments with high bandwidth requirements.

This project's goals are:

-   to solve the performance issue with EOIP on Linux
-   to make EOIP tunneling support a standard part of the Linux world

Install
-------

This code was developed on a 3.2.44 linux kernel and tested on the next 3.2.x releases. Patches and out-of-tree builds are provided for 3.2/3.16/4.19 kernels. It should not be hard to adapt it to different Linux kernels.

### Unified out-of-tree build (recommended)

The `unified/` directory contains a single set of sources (`eoip.c`, `gre.c`, `eoipv6.c`) that builds unmodified on all supported kernels (3.2 up to 7.0+) using `LINUX_VERSION_CODE` conditionals. No patching happens at build time and every kernel version gets the same features at the same time. `eoipv6.ko` is built automatically when the kernel has IPv6 support and is fully standalone (it does not need the replacement `gre.ko`):

````shell
cd path-to-eoip/unified
make               # build against the running kernel
make KDIR=/lib/modules/X.Y.Z/build   # or against specific headers
make install
````

The per-version `out-of-tree-X.Y.x` directories and `kernel-patch/` patches are retained for reference under `obsolete/`, but the unified build is preferred.

-   To patch a kernel tree:

````shell
cd path-to-kernel-source/linux-X.Y.Z
patch -p1 < path-to-eoip/obsolete/kernel-patch/kernel-X.Y.Z-eoip-gre-demux.patch
patch -p1 < path-to-eoip/obsolete/kernel-patch/kernel-X.Y.Z-eoip-buildconf.patch
patch -p1 < path-to-eoip/obsolete/kernel-patch/kernel-X.Y.Z-eoip.patch
````

afterwards configure the kernel in the usual ways `make (menu/x/...)config` and do not forget to select `IP: EOIP tunnels over IP` located under `Networking options` from `Networking support`

EOIP tunnel depends on `IP: GRE demultiplexer` - if it not selected then EOIP tunnel is not shown at all

Unless the target is a limited embedded system, it is recommended to build EOIP and GRE demux as modules.

-   To build the modules out of the kernel tree (legacy per-version way):

````shell
cd path-to-eoip/obsolete/out-of-tree-X.Y.Z
make
make install
````

For this to work at least the running kernel's headers should be available.

On Debian/Ubuntu systems this build process will place the newly built modules in `/lib/modules/x.x.x.x/misc`. Note that there will be two versions of `gre.ko` (the GRE demux).
At least on 3.2/3.16/4.19 it is safe to replace the original version with the modified one because it is backwards compatible.

The `eoip.ko` module cannot operate properly without the newly built version of GRE demux (`gre.ko`). If the original `gre.ko` is loaded then it should be removed and the newly built `gre.ko` loaded before loading `eoip.ko`.

-   To build the userland management utility `eoip`:

````shell
cd path-to-eoip
make
````

### Example how to build out of tree

This example may be used as a checklist for an out of tree build:

````shell
root@ubuntu-18_04:~/eoip# make
cc -Wall -Os -c libnetlink.c
cc -Wall -Os -o eoip eoipcr.c libnetlink.o
strip eoip
root@ubuntu-18_04:~/eoip# cd out-of-tree-4.15.x/
root@ubuntu-18_04:~/eoip/out-of-tree-4.15.x# make
patching file eoip.c
using 4.15.0 version of gre_demux.c
found running kernel version of gre.h
patching file gre.h
Hunk #1 succeeded at 24 (offset 15 lines).
patching file gre_demux.c
Hunk #3 succeeded at 137 (offset -9 lines).
Hunk #4 succeeded at 173 (offset -9 lines).
make[1]: Entering directory '/usr/src/linux-headers-4.15.0-106-generic'
  CC [M]  /root/eoip/out-of-tree-4.15.x/eoip.o
  CC [M]  /root/eoip/out-of-tree-4.15.x/gre.o
  Building modules, stage 2.
  MODPOST 2 modules
  CC      /root/eoip/out-of-tree-4.15.x/eoip.mod.o
  LD [M]  /root/eoip/out-of-tree-4.15.x/eoip.ko
  CC      /root/eoip/out-of-tree-4.15.x/gre.mod.o
  LD [M]  /root/eoip/out-of-tree-4.15.x/gre.ko
make[1]: Leaving directory '/usr/src/linux-headers-4.15.0-106-generic'
root@ubuntu-18_04:~/eoip/out-of-tree-4.15.x# insmod ./gre.ko
root@ubuntu-18_04:~/eoip/out-of-tree-4.15.x# insmod ./eoip.ko
root@ubuntu-18_04:~/eoip/out-of-tree-4.15.x# ../eoip add name eoip0 local 203.0.113.113 remote 198.51.100.100 tunnel-id 8421
root@ubuntu-18_04:~/eoip/out-of-tree-4.15.x# ip li set eoip0 up
````

### DKMS

````shell
cd path-to-eoip
ln -s $(pwd) /usr/src/eoip-1.1
dkms add eoip/1.1
dkms build eoip/1.1
dkms install eoip/1.1
````

Userland management utility
---------------------------

### `eoip` - tunnel management utility

### Important notes

-   Normally EoIP tunnels in MikroTiks work well by only specifying the remote IP address. This code requires to configure both ends in a symmetrical way - each end of the tunnel should have the local IP address configured and equal to the remote IP address configured on the other end.
-   Keepalive is supported but off by default; either enable it (`keepalive 10,10` matches the RouterOS default) or configure the tunnel on MikroTik's end with `!keepalive`.
-   It is a good idea to use IP fragmentation and to set MTU on both ends to 1500; using `clamp-tcp-mss` is pointless in this case. For performance it would be best if the transport network's MTU is 42+tunnel MTU (42 bytes is the EoIP protocol overhead) but obviously that is not the case over the Internet.
-   The EoIP protocol is connection-less and requires both ends to be able to reach the other end. In case only one end has a public IP, the other end may establish a private network by using another protocol that works over NAT (e.g. `PPTP`, `L2TP`, etc.) and run EoIP on top of the newly established private network.
-   Security warning: EoIP is a simple encapsulation and does implement any transport security.

### Usage

-   to create new eoip tunnel interface:

````none
    eoip add tunnel-id <tunnel-id> [name <if-name>]
             [local <src-address>] [remote <dst-address>]
             [link <ifindex>] [ttl <ttl>] [tos <tos>]
             [keepalive none|<secs>[,<retries>]]
````

IPv6 local/remote addresses select the `eoipv6` tunnel kind (tunnel-id 0..4095, `ttl` sets the hop limit, `tos` the traffic class). Keepalive is off by default; `keepalive <secs>` implies the RouterOS default of 10 retries and `keepalive <secs>,0` selects send-only mode (transmit keepalives, never drop the carrier).

-   to change existing eoip tunnel interface:

````none
    eoip change name <if-name> tunnel-id <tunnel-id>
                [local <src-address>] [remote <dst-address>]
                [link <ifindex>] [ttl <ttl>] [tos <tos>]
                [keepalive none|<secs>[,<retries>]]
````

When the keepalive option is not given, `change` leaves the current keepalive configuration untouched.

-   to list existing eoip tunnels:

````none
    eoip list
````

### Example how to configure

To configure an Ethernet tunnel between a MikroTik with IP 198.51.100.11 and a Linux box with IP 203.0.113.50 with tunnel id 1234:

On MikroTik:

`/interface eoip add clamp-tcp-mss=no !keepalive local-address=198.51.100.11 remote-address=203.0.113.50 tunnel-id=1234 mtu=1500 name=eoip1234`

On Linux:

`eoip add name eoip1234 local 203.0.113.50 remote 198.51.100.11 tunnel-id 1234`

Installation Script
-------

Example provided in contrib/install.sh


Roadmap
-------

-   ~~make a patch for iproute2 to include eoip support~~
-   ~~work towards making this code good enough for inclusion in official kernel/iproute2 releases~~

    The inclusion of a reverse-engineered proprietary protocol which violates GRE standards is not going to happen in the official Linux kernel. Creating a patch for iproute2 is pointless as it would require patching and replacing one more component that in turn would make supporting a Linux system with kernel mode EoIP even harder. Thus using the included simple tool would be easier and preferred.

    The problem in making a stand-alone EoIP kernel module is that it requires replacing gre_demux (`gre.ko`). Linux kernel does not support overloading IP protocol handlers and EoIP does not fit anywhere in the standard gre_demux logic. A relatively sane solution might be to include the GRE demultiplexing logic in the EoIP kernel module itself, to provide a `gre` alias and to blacklist the original `gre.ko`. In this way GRE and PPTP would still be able to coexist with EoIP.

-   ~~make a DKMS package~~
-   ~~implement the `keepalive` option (off by default for backward compatibility)~~
-   ~~implement EoIPv6 (a different protocol, see the protocol spec) as a separate `eoipv6` kernel module~~
-   IPsec integration compatible with RouterOS `ipsec-secret` (userland tooling around strongSwan)

Development process
-------------------

This code was developed based on information gathered from sniffed datagrams and information from similar projects without involving any reverse engineering of code from the closed source commercial product.

The protocol is not documented and although it looks like there are no deviations in the header format this cannot be guaranteed in all environments or for future releases of the commercial product.

Protocol spec
-------------

After the IP header (which can be fragmented, MTU 1500 is usually used for tunnels) a GRE-like datagram follows.
Note that RFC 1701 is mentioned in MikroTik's docs but there is nothing in common between the standard and the actual protocol used.

Header format (taken from https://github.com/katuma/eoip):

````none
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       GRE FLAGS 0x20 0x01     |      Protocol Type  0x6400    | = MAGIC "\x20\x01\x64\x00"
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Encapsulated frame length   |           Tunnel ID           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Ethernet frame...                                             |
````

Strangely enough the frame length is kept into network byte order while tunnel ID is in little endian byte order.

### Keepalive packet format (EoIP)

A keepalive is a normal EoIP datagram with the encapsulated frame length field set to zero and no payload - i.e. exactly the 8 header bytes:

````none
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       GRE FLAGS 0x20 0x01     |      Protocol Type  0x6400    | = MAGIC "\x20\x01\x64\x00"
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Frame length 0x00 0x00      |           Tunnel ID           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
````

For example a keepalive for tunnel ID 7 is `20 01 64 00 00 00 07 00` (the tunnel ID is little endian, as in data packets).

### EoIPv6 protocol spec

MikroTik's EoIPv6 (`/interface eoipv6`) is a completely different protocol from EoIP: it does not use GRE at all. After the IPv6 header (Next Header 97, the EtherIP protocol number from RFC 3378) a 2 byte header follows, then the raw Ethernet frame:

````none
 0                   1
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| TID_H |0 0 1 1|     TID_L     |  TID_H = tunnel ID bits 11..8, 0x3 = version
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Ethernet frame...             |
````

-   The tunnel ID is only 12 bits (0..4095), packed as `byte0 = (TID>>8)<<4 | 0x3`, `byte1 = TID & 0xff`; e.g. tunnel ID 1234 (0x4d2) is sent as `43 d2` and tunnel ID 0 as `03 00`.
-   Although the header size and protocol number come from EtherIP (RFC 3378), the format violates the RFC twice: the version nibble sits in the low nibble of byte 0 instead of the high one, and the "reserved, must be zero" bits carry the tunnel ID.
-   There is no frame length field; the encapsulated frame length is simply the IPv6 payload length minus 2.
-   The receiver demultiplexes tunnels by the (source address, tunnel ID) pair.
-   The default interface MTU on a 1500 byte path is 1444 (1500 - 40 IPv6 - 2 header - 14 Ethernet), compared to 1458 for EoIP.
-   RouterOS sends tunnel packets with hop limit 255, traffic class 0 and flow label 0 (observed on live captures for both keepalives and data packets).
-   Encapsulated frames are not padded to the 60 byte Ethernet minimum - e.g. a 42 byte ARP frame is carried as-is (observed on a live capture).

EoIPv6 is implemented by the standalone `eoipv6.ko` module (built from `unified/eoipv6.c` when the kernel has IPv6 support); unlike the EoIP pair it does not need the replacement GRE demultiplexer.

### Keepalive packet format (EoIPv6)

A bare 2 byte header with no payload - the only way to express an empty frame in a format without a length field. Verified against a live RouterOS capture: a keepalive for tunnel ID 1234 is an IPv6 datagram with next header 97, payload length 2, hop limit 255, traffic class 0, flow label 0 and payload bytes `43 d2`.

A peer that does not implement EoIPv6 answers each keepalive with ICMPv6 parameter problem (code 1, unrecognized next header, pointer 6); RouterOS ignores these and keeps transmitting.

### Keepalive semantics

The semantics below are verified for EoIP (RouterOS behavior and the interoperating in-kernel OpenBSD `eoip(4)` implementation) and are presumed identical for EoIPv6. RouterOS configures them as `keepalive=interval,retries` with a default of `10s,10`:

-   Each side independently transmits one keepalive every `interval`; keepalives are not echoed and there is no reply - each side just listens for the other's keepalives.
-   Only received keepalives count as a sign of peer liveness; received data frames do not. This is why a RouterOS tunnel with keepalive enabled shows `not running` against a peer that passes traffic but sends no keepalives.
-   When nothing is heard for `interval * retries` (100 seconds with the defaults), RouterOS clears the interface running flag. Keepalive transmission continues while the peer is considered down, which is what makes both ends recover automatically; the running flag returns when keepalives are heard again.

Both modules implement these semantics with `keepalive <interval>[,<retries>]`; keepalive is off by default for backward compatibility (a tunnel without keepalive silently consumes received keepalives), `retries` defaults to the RouterOS value of 10 and `retries` 0 selects send-only mode. When monitoring is enabled, the interface carrier is dropped after `interval * retries` of silence and restored by the first received keepalive, so bridges, bonding and routing react to peer loss the Linux-native way.

Bugs
----

This code was tested and works without problems on quite a few different 32/64bit x86 systems.

No testing was done on non-x86 and big endian hardware.

There is no guarantee that there are no bugs left. Patches are welcome.

License
-------

All code and code modifications in this project are released under the GPL licence. Look at the COPYING file.
