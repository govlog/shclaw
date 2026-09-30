#!/bin/sh
# Release archive for Linux i386: musl, BearSSL, TinyCC and shclaw all compiled for
# the i586 baseline (no cmov, SSE or MMX), so the binary runs on any 32-bit x86
# PC from the Pentium and the AMD K6 on. musl is the pinned one (LIBC=vendor).
# Needs Docker. Builds a source tarball (default: the committed tree, HEAD;
# vendor/ may be included) into an output directory (default: dist/).
# Usage: scripts/release-linux-i386.sh [source.tar [output dir]]
# SECURE=1 in the environment builds the hardened variant (see the Makefile).
set -e
cd "$(dirname "$0")/.."
src=${1:-}
out=${2:-dist}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -z "$src" ]; then
    git archive -o "$tmp/src.tar" HEAD
    src=$tmp/src.tar
fi
mkdir -p "$out"
src=$(readlink -f "$src")
out=$(readlink -f "$out")
docker run --rm --platform linux/386 -e SECURE="${SECURE:-}" -v "$src:/src.tar:ro" -v "$out:/out" alpine:3.24 sh -ec '
  apk add --no-cache build-base git curl util-linux >/dev/null 2>&1
  mkdir /build && tar -xf /src.tar -C /build && cd /build
  [ -f vendor/bearssl/Makefile ] || ./vendor.sh >/dev/null
  [ -f vendor/musl/configure ] || ./vendor.sh musl >/dev/null
  BASE="-march=i586 -mtune=generic -mfpmath=387 -mno-sse -mno-mmx"
  printf "#!/bin/sh\nexec gcc %s \"\$@\"\n" "$BASE" > /usr/local/bin/i586-gcc
  chmod +x /usr/local/bin/i586-gcc
  # A plain static binary, not the static-PIE that Alpine gcc makes by
  # default: PIE code on i386 gives up a register to the GOT pointer
  mk() { setarch i686 make "$@" LIBC=vendor LIBC_CC=i586-gcc ARCH=i386 CF_PROT= PIE=-fno-pie \
         "STATIC=-static -no-pie" ${SECURE:+SECURE=$SECURE}; }
  mk musl > /tmp/musl.log 2>&1 || { tail -30 /tmp/musl.log; exit 1; }
  grep -E "warning:|Built" /tmp/musl.log || true
  mk check > /tmp/check.log 2>&1 || { grep -E "FAIL|error" /tmp/check.log; tail -5 /tmp/check.log; exit 1; }
  grep -E "passed" /tmp/check.log
  mk dist
  cp dist/*.tar.gz /out/
  chown -R '"$(id -u):$(id -g)"' /out'
