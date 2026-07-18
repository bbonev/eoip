#!/bin/bash

# exit on error
set -e

# Change dir to the script's location
cd "$(dirname "$0")"

# If called from contrib folder, change dir up
basename="$(basename "$(pwd)")"
if [[ "$basename" == "contrib" ]]; then
	cd ..
fi

# Build and install the eoip userland tool
make clean
make
make install

# Build and install the kernel modules (eoip.ko and, on IPv6 kernels,
# eoipv6.ko).  They are standalone and do not replace the stock gre
# module, so nothing needs to be removed or blacklisted.
cd unified
make
make install
depmod

# Load the modules
modprobe eoip
modprobe eoipv6
