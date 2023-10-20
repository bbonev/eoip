#!/bin/sh

BASEDIR=$(dirname "$0")

sed -n 's/^#define.*EOIP_VERSION.*"\(.*\)".*/\1/p' "$BASEDIR"/version.h
