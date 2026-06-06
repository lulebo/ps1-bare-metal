#!/bin/bash
#
# Install mipsel-none-elf GCC toolchain for PS1 bare-metal development.
# Builds binutils + GCC from source into ~/mipsel-none-elf. The toolchain
# itself installs under your home directory; only the initial apt dependency
# step needs sudo.
#
# Usage (run from a WSL terminal):
#   bash tools/install-toolchain.sh
#
# After completion, add the toolchain to your PATH by appending this line
# to ~/.bashrc or ~/.profile:
#   export PATH="$HOME/mipsel-none-elf/bin:$PATH"
#
# Then re-open your terminal (or: source ~/.bashrc) and verify with:
#   mipsel-none-elf-gcc --version
#
# Based on:
#   https://github.com/grumpycoders/pcsx-redux/blob/main/tools/linux-mips/spawn-compiler.sh

set -e

PREFIX="$HOME/mipsel-none-elf"
BUILD_DIR="$(mktemp -d)"
BINUTILS_VER="2.45"
GCC_VER="15.2.0"

echo "==> Installing build dependencies (requires sudo)..."
sudo apt-get update
sudo apt-get install -y \
    build-essential wget flex bison \
    libgmp-dev libmpfr-dev libmpc-dev

echo "==> Build directory: $BUILD_DIR"
echo "==> Install prefix:  $PREFIX"
mkdir -p "$PREFIX"

# ---- binutils ----
echo "==> Downloading binutils-${BINUTILS_VER}..."
cd "$BUILD_DIR"
for url in \
    "https://ftpmirror.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.gz" \
    "https://mirrors.kernel.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.gz"
do
    wget --max-redirect=2 --timeout=60 --continue --trust-server-names "$url" && break
done

echo "==> Building binutils..."
tar xfz "binutils-${BINUTILS_VER}.tar.gz"
cd "binutils-${BINUTILS_VER}"
./configure \
    --target=mipsel-none-elf \
    --disable-multilib \
    --disable-nls \
    --disable-werror \
    --disable-docs \
    --prefix="$PREFIX"
make -j"$(nproc)"
make install-strip
cd "$BUILD_DIR"

# ---- GCC ----
echo "==> Downloading gcc-${GCC_VER} (this is large, ~100 MB)..."
for url in \
    "https://ftpmirror.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.gz" \
    "https://mirrors.kernel.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.gz"
do
    wget --max-redirect=2 --timeout=120 --continue --trust-server-names "$url" && break
done

echo "==> Building GCC (this takes 20-40 minutes)..."
tar xfz "gcc-${GCC_VER}.tar.gz"
cd "gcc-${GCC_VER}"
./contrib/download_prerequisites
mkdir build && cd build
../configure \
    --target=mipsel-none-elf \
    --without-isl \
    --disable-nls \
    --disable-threads \
    --disable-shared \
    --disable-libssp \
    --disable-libstdcxx-pch \
    --disable-libgomp \
    --disable-werror \
    --without-headers \
    --disable-hosted-libstdcxx \
    --with-as="$PREFIX/bin/mipsel-none-elf-as" \
    --with-ld="$PREFIX/bin/mipsel-none-elf-ld" \
    --enable-languages=c,c++ \
    --prefix="$PREFIX"
make -j"$(nproc)" all-gcc
make install-strip-gcc
make -j"$(nproc)" all-target-libgcc
make install-strip-target-libgcc
make -j"$(nproc)" all-target-libstdc++-v3
make install-strip-target-libstdc++-v3

echo ""
echo "==> Done! Toolchain installed to: $PREFIX"
echo ""
echo "Add this to ~/.bashrc:"
echo "  export PATH=\"\$HOME/mipsel-none-elf/bin:\$PATH\""
echo ""
echo "Then verify with:"
echo "  mipsel-none-elf-gcc --version"
