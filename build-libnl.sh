#!/usr/bin/env bash
curl -fsSL https://github.com/thom311/libnl/releases/download/libnl3_10_0/libnl-3.10.0.tar.gz | tar -xvzf -
cd ./libnl-3.10.0/ || exit
sed -i 's|-lpthread||g' ./configure
export TOOLCHAIN=~/Android/Sdk/ndk/26.3.11579264/toolchains/llvm/prebuilt/linux-x86_64/
export TARGET=aarch64-linux-android
export API=24
export AR="${TOOLCHAIN}/bin/llvm-ar"
export CC="${TOOLCHAIN}/bin/${TARGET}${API}-clang"
export AS="${CC}"
export CXX="${TOOLCHAIN}/bin/${TARGET}${API}-clang++"
export LD="${TOOLCHAIN}/bin/ld"
export RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"
export STRIP="${TOOLCHAIN}/bin/llvm-strip"
./configure --enable-cli=no --host ${TARGET} --prefix="${PWD}/../distribution" CFLAGS='-fPIC -Din_addr_t=uint32_t' CXXFLAGS='-fPIC -Din_addr_t=uint32_t'
make
make install
