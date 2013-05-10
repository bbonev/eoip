EOIP
====

Kernel mode EOIP (Ethernet over IP) tunnel compatible with MikroTik RouterOS

There are several projects doing the same job with userland utilities via tap interfaces and raw sockets. While a userland application is easier to install and maintain it lacks the perfomance and stability of an in-kernel module. Especially for the simple job of adding and stripping the EOIP tunneling headers. The userland tunneling application may be good for testing, research or concept proof projects but not suitable for production environments with high bandwidth requirements.

This project's goals are:

- to solve the perfomance issue with EOIP on Linux
- to make EOIP tunneling support a standard part of the Linux world


Install
-------

This code is tested on a 3.2.44 linux kernel. It should not be hard to adapt it to newer or older kernels.

- To patch a kernel tree with this code:

```
cd path-to-kernel-source/linux-3.2.44
patch -p1 < path-to-eoip/kernel-patch/kernel-3.2.44-eoip-gre-demux.patch
patch -p1 < path-to-eoip/kernel-patch/kernel-3.2.44-eoip-buildconf.patch
patch -p1 < path-to-eoip/kernel-patch/kernel-3.2.44-eoip.patch
```

afterwards `make (menu/x/...)config` and select `IP: EOIP tunnels over IP` located under `Networking support` / `Networking options`

EOIP tunnel depends on `IP: GRE demultiplexer`

- To build the userland management utility `eoip`:

```
cd path-to-eoip
make
```


Userland management utility
---------------------------

##### `eoip` - eoip tunnel management utility

- to create new eoip tunnel interface:

`eoip <if-name> <tunnel-id> <src-address> <dst-address>`

- to list existing eoip tunnels:

`eoip list`


Roadmap
-------

- make a patch for iproute2 to include eoip support

- work towards making this code good enough for inclusion in official kernel/iproute2 releases


Development process
-------------------

This code was developed based on information gathered from sniffed datagrams and information from similar projects without involving any reverse engineering of code from the closed source commercial product

The protocol is not documented and although it looks like there are no deviations in the header format this cannot be guaranteed in all environments or for future releases of the commercial product

Protocol spec
-------------

After IP header (which can be fragmented, MTU 1500 is usually used for tunnels)
GRE-like datagram follows. Note that it's nothing like RFC 1701 MikroTik mentioned in their docs:

* Header format (taken from https://github.com/katuma/eoip)


     0                   1                   2                   3
     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |       GRE FLAGS 0x20 0x01     |      Protocol Type  0x6400    | = MAGIC "\x20\x01\x64\x00"
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |   Encapsulated frame length   |           Tunnel ID           |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    | Ethernet frame...                                             |

Strangely enough the frame length is kept into network byte order and tunnel id in little endian byte order

License
-------

All code and code modifications in this project are released under the GPL licence. Look at the COPYING file.

