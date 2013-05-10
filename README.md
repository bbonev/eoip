EOIP
====

Kernel mode EOIP (Ethernet over IP) tunnel compatible with MikroTik

This code is developed and tested on 3.2.44 linux kernel. It should not be hard to adapt it to newer kernels.

Install
-------

To patch a kernel tree with this code:

cd path-to-kernel-source/linux-3.2.44

patch -p1 < path-to-eoip/kernel-patch/kernel-3.2.44-eoip-gre-demux.patch

patch -p1 < path-to-eoip/kernel-patch/kernel-3.2.44-eoip-buildconf.patch

patch -p1 < path-to-eoip/kernel-patch/kernel-3.2.44-eoip.patch

Userland control utilities
--------------------------

##### `eoip` - eoip tunnel management utility

- to create new eoip tunnel interface:

`eoip <if-name> <tunnel-id> <src-address> <dst-address>`

- to list existing eoip tunnels:

`eoip list`

Next step is to make a patch for iproute2 to include eoip

