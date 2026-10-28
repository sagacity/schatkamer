#!/usr/bin/env bash
# Cross-compiles schatkamer-app.elf in WSL: builds the pinned PS5 payload SDK,
# fetches the pinned PacBrew libcurl, then runs make.
set -euo pipefail
cd "$(dirname "$0")"

sdk_commit=a7685ce8e97ce1f8a4f38253a41d54241c9124f1
work=.deps
export PS5_PAYLOAD_SDK="$PWD/$work/ps5-payload-sdk"

if [[ ! -d $PS5_PAYLOAD_SDK ]]; then
    mkdir -p "$work"
    [[ -d $work/ps5-sdk ]] || git clone https://github.com/ps5-payload-dev/sdk "$work/ps5-sdk"
    git -C "$work/ps5-sdk" checkout "$sdk_commit"
    make -C "$work/ps5-sdk" DESTDIR="$PS5_PAYLOAD_SDK" clean install
fi

root=$(bash tools/setup-pacbrew-dependencies.sh --all)
export PKG_CONFIG_SYSROOT_DIR="$root"
export PKG_CONFIG_LIBDIR="$root/user/homebrew/lib/pkgconfig"
export PKG_CONFIG_PATH="$root/user/homebrew/libdata/pkgconfig"
export CURL_CFLAGS="$(pkg-config --static --cflags libcurl)"
export CURL_LIBS="$(pkg-config --static --libs libcurl)"

make clean all
sha256sum schatkamer-app.elf
