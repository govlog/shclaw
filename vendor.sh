#!/bin/sh
# vendor.sh — fetch pinned vendor sources for shclaw
#
#   ./vendor.sh            BearSSL, TinyCC, cJSON
#   ./vendor.sh cosmo      + cosmocc toolchain
#   ./vendor.sh smolbsd    + smolBSD
#   ./vendor.sh musl       + musl sources (make musl LIBC=vendor)
#
# Every source is pinned (commit or checksum). To bump one, change its pin
# below, delete vendor/<name> and rebuild.
set -e

BEARSSL_REV=7bea48e5e850ab4cafbe68d3765cdaba13a86d6f   # 2026-04-06
# TinyCC "mob" is writable by anyone: never build an unpinned HEAD.
TCC_REV=9db1105c32afd3dcf0c28b8186f08e63c761b2b5       # mob, 2026-09-26
CJSON_REV=6d9f2443ab071f86e5d9b43025a40929ec41c46c     # master, 2026-09-16 (1.7.19+)
CJSON_C_SHA256=607e756460fa0de37d20a7a9181f2de29c97bfb7ce5a0e6c2f548243836cd852
CJSON_H_SHA256=25b0145150d500498e4d209cec69c18c42cf818bffcc54690be3b895a2a16dee
COSMOCC_VER=4.0.2
COSMOCC_SHA256=85b8c37a406d862e656ad4ec14be9f6ce474c1b436b9615e91a55208aced3f44
SMOLBSD_REV=6fbba69c4bb823aba5667227a1935cd5f66c0818   # 2026-09-24
# musl release plus the upstream fixes Alpine 3.24 applies to it
MUSL_VER=1.2.6
MUSL_SHA256=d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a
MUSL_PATCHES="CVE-2026-6042.patch:1d0be2e72b9d5bd16546b923aa8af861d271322f01644716a81823bec4065c99
CVE-2026-40200.patch:1ee29f64f9ca8e8ad7c349779d661ff6b52126a27575d3586981357a52c406fb"

VENDOR="$(dirname "$0")/vendor"
mkdir -p "$VENDOR"

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
    else command sha256 -q "$1"; fi   # BSD (command: not this function)
}

check_sha256() {
    if [ "$(sha256 "$1")" != "$2" ]; then
        echo "ERROR: checksum mismatch for $1" >&2
        rm -f "$1"
        exit 1
    fi
}

# git_pinned <dir> <url> <rev> — the tree only appears once at the pin
git_pinned() {
    if [ -d "$1/.git" ]; then
        head=$(git -C "$1" rev-parse HEAD)
        if [ "$head" != "$3" ]; then
            echo "ERROR: $1 is at $head, pinned $3: delete it to refetch" >&2
            exit 1
        fi
        echo "$(basename "$1"): already present"
        return
    fi
    echo "Fetching $(basename "$1") @ $3..."
    rm -rf "$1.tmp"
    git clone -q "$2" "$1.tmp"
    git -C "$1.tmp" -c advice.detachedHead=false checkout -q "$3"
    mv "$1.tmp" "$1"
}

git_pinned "$VENDOR/bearssl" https://www.bearssl.org/git/BearSSL "$BEARSSL_REV"
git_pinned "$VENDOR/tcc" https://repo.or.cz/tinycc.git "$TCC_REV"

if [ ! -f "$VENDOR/cjson/cJSON.c" ]; then
    echo "Fetching cJSON @ $CJSON_REV..."
    mkdir -p "$VENDOR/cjson"
    for f in cJSON.c cJSON.h; do
        curl -fsSL "https://raw.githubusercontent.com/DaveGamble/cJSON/$CJSON_REV/$f" \
            -o "$VENDOR/cjson/$f"
    done
    check_sha256 "$VENDOR/cjson/cJSON.c" "$CJSON_C_SHA256"
    check_sha256 "$VENDOR/cjson/cJSON.h" "$CJSON_H_SHA256"
else
    echo "cJSON: already present"
fi

if [ "$1" = "cosmo" ]; then
    if [ -x "$VENDOR/cosmo/bin/cosmocc" ]; then
        echo "cosmocc: already present"
    else
        echo "Fetching cosmocc $COSMOCC_VER..."
        TMPFILE=$(mktemp "${TMPDIR:-/tmp}/cosmocc-XXXXXX")
        trap 'rm -f "$TMPFILE"' EXIT
        curl -fSL "https://cosmo.zip/pub/cosmocc/cosmocc-$COSMOCC_VER.zip" -o "$TMPFILE"
        check_sha256 "$TMPFILE" "$COSMOCC_SHA256"
        mkdir -p "$VENDOR/cosmo"
        unzip -qo "$TMPFILE" -d "$VENDOR/cosmo"
    fi
fi

if [ "$1" = "smolbsd" ]; then
    git_pinned "$VENDOR/smolbsd" https://github.com/NetBSDfr/smolBSD.git "$SMOLBSD_REV"
fi

if [ "$1" = "musl" ]; then
    if [ -f "$VENDOR/musl/configure" ]; then
        echo "musl: already present"
    else
        echo "Fetching musl $MUSL_VER..."
        rm -rf "$VENDOR/musl.tmp" && mkdir -p "$VENDOR/musl.tmp"
        cd "$VENDOR/musl.tmp"
        curl -fsSLO "https://musl.libc.org/releases/musl-$MUSL_VER.tar.gz"
        check_sha256 "musl-$MUSL_VER.tar.gz" "$MUSL_SHA256"
        tar -xzf "musl-$MUSL_VER.tar.gz"
        for p in $MUSL_PATCHES; do
            curl -fsSLO "https://gitlab.alpinelinux.org/alpine/aports/-/raw/3.24-stable/main/musl/${p%%:*}"
            check_sha256 "${p%%:*}" "${p#*:}"
            patch -d "musl-$MUSL_VER" -p1 -s < "${p%%:*}"
        done
        cd - >/dev/null
        mv "$VENDOR/musl.tmp/musl-$MUSL_VER" "$VENDOR/musl"
        rm -rf "$VENDOR/musl.tmp"
    fi
fi

echo "Done. Run 'make' to build."
