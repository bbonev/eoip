#!/bin/bash -e

# Change dir to the script's location
cd "$(dirname "$0")"

# Check for a loaded gre module
loaded=$(lsmod | grep '^gre ')
if [[ "$loaded" != "" ]]; then
	# Show a hint of loaded modules
	lsmod | grep '^gre ' | cat

	echo "WARNING: You have a gre module loaded. Removing the existing module may break anything that requires it. Please proceed with caution."
	read -r -p "Proceed anyway? [y/N] " response
	if [[ "$response" != "y" && "$response" != "Y" ]]; then
		echo "Aborted by user request"
		exit 1
	fi
fi

# Delete the existing gre module because on some
# systems depmod is not enough to override the
# old gre module.
find /lib/modules/ -name "gre.ko*" -delete

# If called from contrib folder, change dir up
basename="$(basename "$(pwd)")"
if [[ "$basename" == "contrib" ]]; then
	cd ..
fi

# Build eoip tool
make clean
make
make install

# Detect Kernel Major & Minor
unamer=$(uname -r)
kernel=(${unamer//./ })
cd "out-of-tree-${kernel[0]}.${kernel[1]}.x"

# Build eoip module
make
make install
depmod

# Show a hint of loaded modules
lsmod | grep '^gre '

# Remove the old module
echo "Attempting to remove the gre module... if this fails, you may have a dependant module that needs to be unloaded first"
rmmod gre

# Load the new module
modprobe eoip
