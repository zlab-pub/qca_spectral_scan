#!/usr/bin/env bash
mkdir -p ./distribution/include
curl -fsSL https://w1.fi/cgit/hostap/plain/src/common/qca-vendor.h -o ./distribution/include/qca-vendor.h
./build-libnl.sh
