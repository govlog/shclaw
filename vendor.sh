#!/bin/sh
# vendor.sh — fetch vendor sources for shclaw
set -e

VENDOR="$(dirname "$0")/vendor"
mkdir -p "$VENDOR"

# BearSSL
if [ ! -f "$VENDOR/bearssl/Makefile" ]; then
    echo "Fetching BearSSL..."
    git clone --depth 1 https://www.bearssl.org/git/BearSSL "$VENDOR/bearssl"
else
    echo "BearSSL: already present"
fi

# TinyCC
if [ ! -f "$VENDOR/tcc/Makefile" ]; then
    echo "Fetching TinyCC..."
    git clone --depth 1 https://repo.or.cz/tinycc.git "$VENDOR/tcc"
else
    echo "TinyCC: already present"
fi

# cJSON (just two files)
if [ ! -f "$VENDOR/cjson/cJSON.c" ]; then
    echo "Fetching cJSON..."
    mkdir -p "$VENDOR/cjson"
    curl -sL https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c -o "$VENDOR/cjson/cJSON.c"
    curl -sL https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h -o "$VENDOR/cjson/cJSON.h"
else
    echo "cJSON: already present"
fi

# Cosmopolitan toolchain (only if building APE)
if [ "$1" = "cosmo" ]; then
    if [ -x "$VENDOR/cosmo/bin/cosmocc" ]; then
        echo "cosmocc: already present"
    else
        echo "Fetching cosmocc toolchain..."
        TMPFILE=$(mktemp /tmp/cosmocc-XXXXXX.zip)
        trap 'rm -f "$TMPFILE"' EXIT
        curl -fSL "https://cosmo.zip/pub/cosmocc/cosmocc.zip" -o "$TMPFILE"
        mkdir -p "$VENDOR/cosmo"
        unzip -qo "$TMPFILE" -d "$VENDOR/cosmo"
        rm -f "$TMPFILE"
        trap - EXIT
    fi
fi

# smolBSD (only if building microVM)
if [ "$1" = "smolbsd" ]; then
    if [ -f "$VENDOR/smolbsd/Makefile" ]; then
        echo "smolBSD: already present"
    else
        echo "Fetching smolBSD..."
        git clone --depth 1 https://github.com/NetBSDfr/smolBSD.git "$VENDOR/smolbsd"
    fi
fi

echo "Done. Run 'make' to build."
