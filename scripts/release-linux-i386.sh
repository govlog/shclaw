#!/bin/sh
# Release archive for Linux i386: musl, BearSSL, TinyCC and shclaw all compiled for
# the i686 baseline without SSE or MMX, so the binary runs on any 32-bit x86
# PC from the Pentium Pro on. musl 1.2.6 comes from source with the patches
# Alpine 3.24 applies (CVE-2026-6042, CVE-2026-40200), checked against the
# sha512 sums of Alpine's APKBUILD.
# Needs Docker. Builds the committed tree (HEAD) into dist/.
# Usage: scripts/release-linux-i386.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p dist
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git archive -o "$tmp/src.tar" HEAD
docker run --rm --platform linux/386 -v "$tmp/src.tar:/src.tar:ro" -v "$PWD/dist:/out" alpine:3.24 sh -ec '
  apk add --no-cache build-base git curl util-linux >/dev/null 2>&1
  A=https://gitlab.alpinelinux.org/alpine/aports/-/raw/3.24-stable/main/musl
  cd /tmp
  curl -fsSLO https://musl.libc.org/releases/musl-1.2.6.tar.gz
  for f in CVE-2026-6042.patch CVE-2026-40200.patch __stack_chk_fail_local.c; do curl -fsSLO $A/$f; done
  sha512sum -c <<SUMS
1adad96eddb3a2eb0cacb3e363b0046568925fcdd75cf8b0503f2139df1f693d64730779ca0ce8131b7624ab2d37f4247bb1d3393c523de6e30d2b1d7732555c  musl-1.2.6.tar.gz
f7849abeab0e4eab992a80464afa07b9c8aae5cee76040523b9dbc0931435f28ea8f5792b8a4a0cd6d608ac2f37e3225e89afcb3171a0a6df44450ed57cee83b  CVE-2026-6042.patch
a64ab7688d1a85e560b5687783df482d2467a79a74400da5a1601382847d6b4e6a79b7529a8dc80c46ddda8dc2a812f2261557fc8ee98dc3bdf7322761bd6d9c  CVE-2026-40200.patch
062bb49fa54839010acd4af113e20f7263dde1c8a2ca359b5fb2661ef9ed9d84a0f7c3bc10c25dcfa10bb3c5a4874588dff636ac43d5dbb3d748d75400756d0b  __stack_chk_fail_local.c
SUMS
  tar -xzf musl-1.2.6.tar.gz && cd musl-1.2.6
  patch -p1 -s < ../CVE-2026-6042.patch && patch -p1 -s < ../CVE-2026-40200.patch
  BASE="-march=i686 -mtune=generic -mfpmath=387 -mno-sse -mno-mmx"
  CFLAGS="$BASE -O2" ./configure --prefix=/opt/musl --disable-shared --enable-wrapper=gcc >/dev/null
  make -j$(nproc) >/dev/null && make install >/dev/null
  gcc $BASE -O2 -fPIC -c ../__stack_chk_fail_local.c -o /tmp/sscfl.o
  ar rcs /opt/musl/lib/libssp_nonshared.a /tmp/sscfl.o
  printf "#!/bin/sh\nexec /opt/musl/bin/musl-gcc %s \"\$@\"\n" "$BASE" > /usr/local/bin/i386-cc
  chmod +x /usr/local/bin/i386-cc
  mkdir /build && tar -xf /src.tar -C /build && cd /build && ./vendor.sh >/dev/null
  # musl-gcc cannot link static-PIE (its specs always start with Scrt1.o),
  # and Alpine gcc makes PIE by default: ask for a plain static binary
  mk() { setarch i686 make "$@" MUSL_CC=i386-cc ARCH=i386 CF_PROT= PIE=-fno-pie "STATIC=-static -no-pie"; }
  mk musl 2>&1 | grep -E "warning:|error:|Built" || true
  mk check 2>&1 | grep -E "FAIL|passed|failed|error:" || true
  mk dist
  cp dist/*.tar.gz /out/
  chown -R '"$(id -u):$(id -g)"' /out'
