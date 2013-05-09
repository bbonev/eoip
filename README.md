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

A simple control utility to create tunnels is included with following format:

eoip &lt;if-name&gt; &lt;tunnel-id&gt; &lt;src-address&gt; &lt;dst-address&gt;

A patch to iproute2 to make this built-in is coming soon

