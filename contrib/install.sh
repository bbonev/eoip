#!/bin/bash -e

# Change dir to the script's location
cd $(pwd)

# Delete the existing gre module because on some
# systems depmod is not enough to override the
# old gre module.
find /usr/lib/modules/ -name "gre.ko*" -delete

# If called from contrib folder, change dir up
dirname=$(basename $(pwd))
if [[ "$dirname" == "contrib" ]]; then
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

# Remove the old module, continue on error
rmmod gre || true

# Load the new module
modprobe eoip
