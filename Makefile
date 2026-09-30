# shclaw — bare metal multi-agent AI daemon in C
#
# A single static binary embedding TLS, HTTP, IRC, JSON, a C compiler,
# and a multi-agent agentic loop.
#
#   make              Show help
#   make musl         Build static Linux binary (~430K, LIBC=vendor: ~390K)
#   make cosmo        Build cross-platform APE binary (~710K)
#   make check        Run the behaviour checks (check-cosmo: with cosmocc)
#   make install      Install to PREFIX (creates instance directory structure)
#   make docker-image Build Docker image from pre-compiled binary
#   make smolbsd      Build smolBSD microVM service
#   make clean        Clean everything

PREFIX   = /opt/shclaw

# -------------------------------------------------------------------
# Architecture detection (for musl build)
# -------------------------------------------------------------------
ARCH    := $(shell uname -m)
UNAME_S := $(shell uname -s)
X86     := $(filter x86_64 i%86,$(ARCH))

# TinyCC keeps the i386 stack 4-byte aligned, gcc code expects 16 bytes
# (SSE): the functions that plugins call must realign it on entry
REALIGN := $(if $(filter i%86,$(ARCH)),-mstackrealign,)
# NetBSD (PaX MPROTECT) never lets written pages become executable: TinyCC
# maps its code from a temporary file twice instead, RX and RW
TCC_DEFS := $(if $(filter NetBSD,$(UNAME_S)),-DCONFIG_SELINUX,)
# NetBSD's linker aligns segments on 2M pages: the file grew to 4.3M of padding
comma := ,
PAGE_LD := $(if $(filter NetBSD,$(UNAME_S)),-Wl$(comma)-z$(comma)max-page-size=0x1000,)
# Extra TinyCC configure options, e.g. TCC_CONF=--cpu=armv6l for ARMv6
# plugin code when building on a newer ARM CPU
TCC_CONF ?=
# Plugins never run with tcc -run, -bt or -b: no backtrace or bounds checker
TCC_SLIM := --config-backtrace=no --config-bcheck=no

# -------------------------------------------------------------------
# Size or hardening
# -------------------------------------------------------------------
# By default the binaries are as small as possible, without the exploit
# mitigations that cost code. This agent is already a security hole by
# design: remote code execution is a feature (the builder compiles and runs
# what a model writes, and a plugin can open a remote shell) ;D
# SECURE=1 builds them back: stack protector, stack clash probes, FORTIFY,
# CET, static-PIE, RELRO, separate code pages, Cosmopolitan's full runtime.
# Run make clean when switching: the vendor builds do not track it.
ifeq ($(SECURE),1)
  CF_PROT   := $(if $(X86),-fcf-protection,)
  HARDEN     = -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=2 $(CF_PROT)
  HARDEN_LD := -Wl,-z,relro,-z,now -Wl,-z,separate-code
  STRIP_SH  :=
  STATIC    := -static-pie
  PIE       := -fPIE
  # Packed relative relocations (DT_RELR): 11K less on x86_64, musl 1.2.4+
  RELR      := $(if $(filter x86_64 aarch64 i%86,$(ARCH)),-Wl$(comma)-z$(comma)pack-relative-relocs,)
else
  # Also turns off what distribution compilers enable by default, and
  # OpenBSD's return address protector
  CF_PROT   := $(if $(X86),-fcf-protection=none,)
  HARDEN     = -fno-stack-protector -fno-stack-clash-protection -U_FORTIFY_SOURCE $(CF_PROT) \
               $(if $(filter OpenBSD,$(UNAME_S)),-fno-ret-protector,)
  # The BSD libcs carry unwind tables (160K on FreeBSD): plain C that never
  # unwinds (no pthread_cancel, no backtraces) does not need them
  HARDEN_LD := -Wl,-z,norelro -Wl,-z,noseparate-code -Wl,--build-id=none \
               -Wl,--no-eh-frame-hdr -Wl,-T,build/no-eh-frame.ld
  # GNU strip 2.41+ also drops the section headers (1K): the BSDs cannot
  STRIP_SH  := --strip-section-headers
  STATIC    := -static -no-pie
  PIE       := -fno-pie
endif

# -------------------------------------------------------------------
# musl toolchain
# -------------------------------------------------------------------
MUSL_CC      = musl-gcc
# Size: -Oz (gcc 12+, clang), no debug info, no unwind tables (plain C, no
# backtraces or pthread_cancel), one section per function or object so that
# the linker drops what nothing uses, link-time optimization, symbols stripped
OPT         ?= -Oz
SIZEOPT      = -g0 -fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections -fdata-sections
# The vendor archives hold LTO objects: gcc-ar indexes them
LTO         ?= -flto=auto
LTO_AR      ?= gcc-ar
# LIBC=vendor: musl built here from pinned sources (./vendor.sh musl) with
# the flags above instead of the system's musl-gcc, 35K less on x86_64.
# LIBC_CC is the compiler that builds it: one command, no options
MUSL_ROOT    = $(CURDIR)/vendor/musl/root
LIBC_CC     ?= gcc
ifeq ($(LIBC),vendor)
  MUSL_CC    = $(MUSL_ROOT)/bin/musl-gcc
  LIBC_DEP   = $(MUSL_ROOT)/lib/libc.a
endif
MUSL_CFLAGS  = -std=gnu11 $(OPT) $(SIZEOPT) $(LTO) -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
               -I include \
               -I vendor/bearssl/inc \
               -I vendor/tcc \
               -I vendor/cjson \
               $(HARDEN) \
               $(PIE) \
               $(REALIGN) \
               -fno-delete-null-pointer-checks \
               -fno-strict-overflow \
               -fno-strict-aliasing \
               -Wformat=2 -Wformat-security \
               -Wimplicit-fallthrough \
               $(EXTRA_CFLAGS)
MUSL_LDFLAGS = $(STATIC) $(RELR) -Wl,--gc-sections $(PAGE_LD) -L vendor/bearssl/build -L vendor/tcc \
               $(HARDEN_LD) \
               -Wl,-z,noexecstack
MUSL_LIBS    = -lbearssl -ltcc -lpthread -lm
MUSL_BIN     = shclaw

# -------------------------------------------------------------------
# Cosmopolitan toolchain
# -------------------------------------------------------------------
COSMO_DIR    = ./vendor/cosmo
COSMO_CC     = $(COSMO_DIR)/bin/x86_64-unknown-cosmo-cc
COSMO_AR     = $(COSMO_DIR)/bin/x86_64-unknown-cosmo-ar
# -mtiny: Cosmopolitan's runtime built for size (-217K). It drops --strace
# and --ftrace, malloc's cookies and some checks of API misuse
COSMO_MODE   = $(if $(filter 1,$(SECURE)),,-mtiny)
# -Oz makes it 12K bigger than -Os
COSMO_OPT   ?= -Os
COSMO_CFLAGS = -std=gnu11 $(COSMO_OPT) $(SIZEOPT) $(COSMO_MODE) -Wall -Wextra \
               -Wno-unused-parameter -Wno-format-truncation \
               -Wno-missing-field-initializers \
               -I include \
               -I vendor/bearssl/inc \
               -I vendor/tcc \
               -I vendor/cjson \
               $(EXTRA_CFLAGS)
COSMO_LDFLAGS = -s -Wl,--gc-sections -L vendor/bearssl/build -L vendor/tcc
COSMO_LIBS    = -lbearssl -ltcc -lpthread -lm
COSMO_BIN     = shclaw.com

# -------------------------------------------------------------------
# Sources
# -------------------------------------------------------------------
SRCS     = $(wildcard src/*.c) vendor/cjson/cJSON.c

MUSL_OBJS  = $(patsubst %.c, build/%.o, $(notdir $(SRCS)))
COSMO_OBJS = $(patsubst %.c, build-cosmo/%.o, $(notdir $(SRCS)))

BEARSSL_A = vendor/bearssl/build/libbearssl.a
TCC_A     = vendor/tcc/libtcc.a

# -------------------------------------------------------------------
# Phony targets
# -------------------------------------------------------------------
.PHONY: help musl cosmo native check check-cosmo check-native dist clean install uninstall docker-image smolbsd

# -------------------------------------------------------------------
# Default: help
# -------------------------------------------------------------------
help:
	@echo "shclaw — bare metal multi-agent AI daemon"
	@echo ""
	@echo "  make musl           Build static Linux binary (~430K)"
	@echo "                      LIBC=vendor: with musl built from pinned sources (~390K)"
	@echo "                      SECURE=1: with the exploit mitigations (bigger)"
	@echo "  make cosmo          Build multi-platform binary (~710K)"
	@echo "                      Runs on Linux/FreeBSD/NetBSD (x86_64); OpenBSD: make native"
	@echo "  make native         Build static binary with cc (Free/Net/OpenBSD, run with gmake)"
	@echo "  make check          Run tests/check.c (check-cosmo, check-native: other toolchains)"
	@echo "  make install        Install to PREFIX (default: /opt/shclaw)"
	@echo "  make dist           Release archives of the built binaries, in dist/"
	@echo "  make docker-image   Build Docker image (requires: make musl first)"
	@echo "  make smolbsd        Build smolBSD service (requires: make cosmo first)"
	@echo "                      AGENT_DIR=/path/to/instance"
	@echo "  make clean          Clean everything (build artifacts + vendor libs)"
	@echo ""
	@echo "Vendor sources (BearSSL, TinyCC, cJSON) are fetched automatically on first build."
	@echo "The cosmo target also auto-fetches the cosmocc toolchain."
	@echo ""
	@echo "Powered by: Cosmopolitan Libc (Justine Tunney), TinyCC (Fabrice Bellard),"
	@echo "            BearSSL (Thomas Pornin), cJSON (Dave Gamble)"

# -------------------------------------------------------------------
# Vendor: fetch sources
# -------------------------------------------------------------------
vendor/bearssl/Makefile:
	./vendor.sh

vendor/tcc/Makefile: vendor/bearssl/Makefile
	@true

vendor/cjson/cJSON.c: vendor/bearssl/Makefile
	@true

# Cosmocc toolchain
$(COSMO_DIR)/bin/cosmocc:
	./vendor.sh cosmo

# -------------------------------------------------------------------
# Vendor: build libraries (musl)
# -------------------------------------------------------------------
vendor/bearssl/build/libbearssl.a.musl: vendor/bearssl/Makefile $(LIBC_DEP)
	@# Clean cosmo-built libs if switching toolchains
	@rm -f vendor/bearssl/build/libbearssl.a.cosmo
	$(MAKE) -C vendor/bearssl clean 2>/dev/null || true
	$(MAKE) -C vendor/bearssl CC=$(MUSL_CC) AR=$(LTO_AR) CFLAGS="$(OPT) $(SIZEOPT) $(LTO) $(HARDEN) $(PIE)" \
		DLL=no TOOLS=no TESTS=no -j$$(getconf _NPROCESSORS_ONLN)
	@touch $@

vendor/tcc/libtcc.a.musl: vendor/tcc/Makefile $(LIBC_DEP)
	@rm -f vendor/tcc/libtcc.a.cosmo
	$(MAKE) -C vendor/tcc clean 2>/dev/null || true
	cd vendor/tcc && ./configure --cc=$(MUSL_CC) $(TCC_SLIM) $(TCC_CONF)
	$(MAKE) -C vendor/tcc libtcc.a CC=$(MUSL_CC) AR=$(LTO_AR) \
		CFLAGS="-Wall $(OPT) $(SIZEOPT) $(LTO) $(HARDEN) $(PIE) -DCONFIG_RUNMEM_RO=1 $(TCC_DEFS) -Wdeclaration-after-statement -Wno-unused-result" -j$$(getconf _NPROCESSORS_ONLN)
	@touch $@

# musl for LIBC=vendor
vendor/musl/configure:
	./vendor.sh musl

$(MUSL_ROOT)/lib/libc.a: vendor/musl/configure
	cd vendor/musl && CC="$(LIBC_CC)" CFLAGS="$(OPT) $(HARDEN)" ./configure \
		--prefix=$(MUSL_ROOT) --disable-shared --enable-wrapper=gcc
	$(MAKE) -C vendor/musl -j$$(getconf _NPROCESSORS_ONLN)
	$(MAKE) -C vendor/musl install
	@# The wrapper only knows dynamic and plain static links: add static-PIE.
	@# Links without -static (TinyCC's c2str) also get libc.a, outside the
	@# group that -static adds: libc after libgcc (ARM's __aeabi_ldiv0 calls raise)
	sed -i -e 's|%{!shared: \([^ ]*\)/Scrt1.o}|%{shared:;static-pie:\1/rcrt1.o; :\1/Scrt1.o}|' \
		-e 's|%{static:-static}|& %{static-pie:-static -pie --no-dynamic-linker}|' \
		-e 's|^libgcc.a%s %:if-exists(libgcc_eh.a%s)$$|& -lc|' \
		$(MUSL_ROOT)/lib/musl-gcc.specs
	@# Alpine's gcc always links -lssp_nonshared (libc.a has what it holds)
	@# and, on riscv64, -latomic: take the compiler's own
	printf '!<arch>\n' > $(MUSL_ROOT)/lib/libssp_nonshared.a
	f=$$($(LIBC_CC) -print-file-name=libatomic.a); [ "$$f" = libatomic.a ] || cp "$$f" $(MUSL_ROOT)/lib/

# -------------------------------------------------------------------
# Vendor: build libraries (cosmo)
# -------------------------------------------------------------------
vendor/tcc/.cosmo-patched: vendor/tcc/Makefile patches/tcc-cosmo.patch
	cd vendor/tcc && { git apply -R --check ../../patches/tcc-cosmo.patch 2>/dev/null || \
		git apply ../../patches/tcc-cosmo.patch; }
	@touch $@

vendor/bearssl/build/libbearssl.a.cosmo: vendor/bearssl/Makefile $(COSMO_DIR)/bin/cosmocc
	@# Clean musl-built libs if switching toolchains
	@rm -f vendor/bearssl/build/libbearssl.a.musl
	$(MAKE) -C vendor/bearssl clean 2>/dev/null || true
	$(MAKE) -C vendor/bearssl \
		CC="$(abspath $(COSMO_CC))" \
		AR="$(abspath $(COSMO_AR))" \
		CFLAGS="$(COSMO_OPT) $(SIZEOPT)" \
		DLL=no TOOLS=no TESTS=no \
		-j$$(getconf _NPROCESSORS_ONLN)
	@touch $@

vendor/tcc/libtcc.a.cosmo: vendor/tcc/.cosmo-patched $(COSMO_DIR)/bin/cosmocc
	@rm -f vendor/tcc/libtcc.a.musl
	$(MAKE) -C vendor/tcc clean 2>/dev/null || true
	cd vendor/tcc && ./configure --cc="$(abspath $(COSMO_CC))" $(TCC_SLIM)
	$(MAKE) -C vendor/tcc libtcc.a \
		CC="$(abspath $(COSMO_CC))" \
		AR="$(abspath $(COSMO_AR))" \
		CFLAGS="-Wall $(COSMO_OPT) $(SIZEOPT) -DCONFIG_RUNMEM_RO=1 -Wdeclaration-after-statement -Wno-unused-result" \
		-j$$(getconf _NPROCESSORS_ONLN)
	@touch $@

# -------------------------------------------------------------------
# musl build
# -------------------------------------------------------------------
musl: $(MUSL_BIN)

$(MUSL_BIN): $(MUSL_OBJS) vendor/bearssl/build/libbearssl.a.musl vendor/tcc/libtcc.a.musl build/no-eh-frame.ld
	$(MUSL_CC) $(MUSL_CFLAGS) $(MUSL_LDFLAGS) -o $@ $(MUSL_OBJS) $(MUSL_LIBS)
	strip -s -R .comment $(STRIP_SH) $@ 2>/dev/null || strip -s -R .comment $@
	@echo "==> Built $(MUSL_BIN) ($$(du -h $(MUSL_BIN) | cut -f1))"

build/%.o: src/%.c include/tc.h include/prompt.h vendor/cjson/cJSON.c | build $(LIBC_DEP)
	$(MUSL_CC) $(MUSL_CFLAGS) -c -o $@ $<

build/cJSON.o: vendor/cjson/cJSON.c vendor/cjson/cJSON.h | build $(LIBC_DEP)
	$(MUSL_CC) $(MUSL_CFLAGS) -c -o $@ $<

build:
	mkdir -p build

build/no-eh-frame.ld: | build
	@# NetBSD's crtbeginT.o registers the table: its part and crtend's end stay
	echo 'SECTIONS { /DISCARD/ : { EXCLUDE_FILE(*crtbegin*.o *crtend*.o) *(.eh_frame) } } INSERT AFTER .text;' > $@

# FreeBSD/NetBSD/OpenBSD: the musl build with the system compiler, no
# -fcf-protection either way (clang rejects it on OpenBSD/i386, OpenBSD/amd64
# enforces IBT) and with SECURE=1 plain -static (their compilers ignore
# -static-pie). FreeBSD's and OpenBSD's ar are llvm-ar: LTO works there
NATIVE = MUSL_CC=cc CF_PROT= LTO_AR=ar $(NATIVE_$(UNAME_S)) \
         $(if $(filter 1,$(SECURE)),STATIC=-static PIE= RELR=)
NATIVE_FreeBSD = LTO=-flto
NATIVE_NetBSD  = LTO= OPT=-Os
NATIVE_OpenBSD = LTO=-flto

native:
	$(MAKE) musl $(NATIVE)

check-native:
	$(MAKE) check $(NATIVE)

# -------------------------------------------------------------------
# Cosmopolitan APE build
# -------------------------------------------------------------------
cosmo: $(COSMO_BIN)

$(COSMO_BIN): $(COSMO_OBJS) vendor/bearssl/build/libbearssl.a.cosmo vendor/tcc/libtcc.a.cosmo
	$(COSMO_CC) $(COSMO_CFLAGS) $(COSMO_LDFLAGS) -o $@ $(COSMO_OBJS) $(COSMO_LIBS)
	@rm -f $@.bak
	@# -b keeps the FreeBSD OS/ABI: FreeBSD refuses an unbranded ELF
	$(COSMO_DIR)/bin/assimilate -b $@
	@echo "==> Built $(COSMO_BIN) ($$(du -h $(COSMO_BIN) | cut -f1))"
	@echo "    Runs on: Linux, FreeBSD, NetBSD (x86_64)"

build-cosmo/%.o: src/%.c include/tc.h include/prompt.h vendor/cjson/cJSON.c $(COSMO_DIR)/bin/cosmocc | build-cosmo
	$(COSMO_CC) $(COSMO_CFLAGS) -c -o $@ $<

build-cosmo/cJSON.o: vendor/cjson/cJSON.c vendor/cjson/cJSON.h $(COSMO_DIR)/bin/cosmocc | build-cosmo
	$(COSMO_CC) $(COSMO_CFLAGS) -c -o $@ $<

build-cosmo:
	mkdir -p build-cosmo

# -------------------------------------------------------------------
# Checks: tests/check.c linked with everything but main.o and daemon.o
# -------------------------------------------------------------------
CHECK_MUSL  = $(filter-out build/main.o build/daemon.o,$(MUSL_OBJS))
CHECK_COSMO = $(filter-out build-cosmo/main.o build-cosmo/daemon.o,$(COSMO_OBJS))

check: $(CHECK_MUSL) vendor/bearssl/build/libbearssl.a.musl vendor/tcc/libtcc.a.musl build/no-eh-frame.ld
	$(MUSL_CC) $(MUSL_CFLAGS) $(MUSL_LDFLAGS) -o build/check tests/check.c $(CHECK_MUSL) $(MUSL_LIBS)
	./build/check

check-cosmo: $(CHECK_COSMO) vendor/bearssl/build/libbearssl.a.cosmo vendor/tcc/libtcc.a.cosmo
	$(COSMO_CC) $(COSMO_CFLAGS) $(COSMO_LDFLAGS) -o build-cosmo/check tests/check.c $(CHECK_COSMO) $(COSMO_LIBS)
	./build-cosmo/check

# -------------------------------------------------------------------
# Release archives: each built binary with what an instance needs
# -------------------------------------------------------------------
VERSION   := $(shell sed -n 's/.*TC_VERSION *"\(.*\)"/\1/p' include/tc.h)
OS        := $(shell uname -s | tr '[:upper:]' '[:lower:]')
# Name of the CPU in the archive name; set it instead of ARCH, which the
# vendor Makefiles also read (e.g. DIST_ARCH=armv6 on a newer ARM CPU)
DIST_ARCH ?= $(ARCH)
DIST_KIT  := README.md LICENSE NOTICE etc/config.ini.example \
             $(wildcard etc/agents/*.ini.example) include/tc_plugin.h plugins/_template.c

dist:
	@test -f $(MUSL_BIN) -o -f $(COSMO_BIN) || { echo "Build first: make musl or make cosmo"; exit 1; }
	@mkdir -p dist
	@for pair in "$(MUSL_BIN) $(OS)-$(DIST_ARCH)" "$(COSMO_BIN) cosmo-x86_64"; do \
		set -- $$pair; [ -f "$$1" ] || continue; \
		name=shclaw-$(VERSION)-$$2; rm -rf "dist/$$name"; mkdir -p "dist/$$name"; \
		tar -cf - $(DIST_KIT) | tar -xf - -C "dist/$$name"; \
		cp "$$1" "dist/$$name/shclaw"; \
		tar -C dist -czf "dist/$$name.tar.gz" "$$name" && rm -rf "dist/$$name"; \
		echo "==> dist/$$name.tar.gz"; \
	done

# -------------------------------------------------------------------
# Install (creates instance directory structure at PREFIX)
# -------------------------------------------------------------------
install:
	@rm -f build/main.o
	$(MAKE) musl EXTRA_CFLAGS='-DINSTALL_PREFIX=\"$(PREFIX)\"'
	@echo "Installing to $(PREFIX)/"
	install -d $(PREFIX)/bin
	install -d $(PREFIX)/etc/agents
	install -d $(PREFIX)/plugins
	install -d $(PREFIX)/include
	install -d $(PREFIX)/data
	install -d $(PREFIX)/logs
	install -m 755 $(MUSL_BIN) $(PREFIX)/bin/$(MUSL_BIN)
	install -m 644 include/tc_plugin.h $(PREFIX)/include/tc_plugin.h
	install -m 644 plugins/_template.c $(PREFIX)/plugins/_template.c
	@for f in etc/config.ini.example etc/agents/*.ini.example; do \
		[ -f "$$f" ] && install -m 644 "$$f" "$(PREFIX)/$$f" || true; \
	done
	@echo ""
	@echo "Installed. Directory structure:"
	@echo "  $(PREFIX)/"
	@echo "    bin/shclaw"
	@echo "    etc/config.ini.example"
	@echo "    etc/agents/*.ini.example"
	@echo "    include/tc_plugin.h"
	@echo "    plugins/_template.c"
	@echo "    data/"
	@echo "    logs/"
	@echo ""
	@echo "Configure:"
	@echo "  cp $(PREFIX)/etc/config.ini.example $(PREFIX)/etc/config.ini"
	@echo "  cp $(PREFIX)/etc/agents/jarvis.ini.example $(PREFIX)/etc/agents/jarvis.ini"
	@echo ""
	@echo "Run:"
	@echo "  $(PREFIX)/bin/$(MUSL_BIN)"

uninstall:
	rm -rf $(PREFIX)

# -------------------------------------------------------------------
# Docker image (copies pre-compiled binary, no build inside Docker)
# -------------------------------------------------------------------
docker-image: $(MUSL_BIN)
	cp $(MUSL_BIN) docker/$(MUSL_BIN)
	docker build -t shclaw docker/
	rm -f docker/$(MUSL_BIN)
	@echo ""
	@echo "Docker image built: shclaw"
	@echo ""
	@echo "Run:"
	@echo "  docker run --user \"\$$(id -u):\$$(id -g)\" -v /path/to/instance:/app/instance shclaw"
	@echo ""
	@echo "Instance directory must contain etc/config.ini"

# -------------------------------------------------------------------
# smolBSD microVM service
# -------------------------------------------------------------------
SMOLBSD_DIR = vendor/smolbsd
SMOLBSD_IMG = shclaw-amd64:latest
AGENT_DIR ?=

$(SMOLBSD_DIR)/Makefile:
	./vendor.sh smolbsd

smolbsd: $(COSMO_BIN) $(SMOLBSD_DIR)/Makefile
	@if [ -z "$(AGENT_DIR)" ]; then \
		echo "Usage: make smolbsd AGENT_DIR=/path/to/instance"; \
		echo ""; \
		echo "AGENT_DIR must point to a shclaw instance directory"; \
		echo "containing etc/config.ini"; \
		exit 1; \
	fi
	@if [ ! -f "$(AGENT_DIR)/etc/config.ini" ]; then \
		echo "Error: $(AGENT_DIR)/etc/config.ini not found"; \
		echo "Create an instance directory first (make install or manually)"; \
		exit 1; \
	fi
	cp $(COSMO_BIN) $(SMOLBSD_DIR)/shclaw.com
	cd $(SMOLBSD_DIR) && ./smoler.sh build -y $(CURDIR)/smolbsd/SMOLerfile
	@echo ""
	@echo "smolBSD image built: $(SMOLBSD_DIR)/images/$(SMOLBSD_IMG).img"
	@echo ""
	@echo "Run (Ctrl-A X stops the VM):"
	@echo "  cd $(SMOLBSD_DIR) && ./smoler.sh run $(SMOLBSD_IMG) -w $(abspath $(AGENT_DIR))"

# -------------------------------------------------------------------
# Clean everything
# -------------------------------------------------------------------
clean:
	rm -rf build build-cosmo $(MUSL_BIN) $(COSMO_BIN) $(COSMO_BIN).dbg $(COSMO_BIN).bak
	rm -f docker/$(MUSL_BIN)
	rm -f $(SMOLBSD_DIR)/shclaw.com
	rm -rf $(SMOLBSD_DIR)/service/shclaw $(SMOLBSD_DIR)/etc/shclaw.conf
	rm -f $(SMOLBSD_DIR)/images/$(SMOLBSD_IMG).img $(SMOLBSD_DIR)/images/$(SMOLBSD_IMG).sig
	$(MAKE) -C vendor/bearssl clean 2>/dev/null || true
	$(MAKE) -C vendor/tcc clean 2>/dev/null || true
	rm -f vendor/tcc/.cosmo-patched
	rm -f vendor/bearssl/build/libbearssl.a.musl vendor/bearssl/build/libbearssl.a.cosmo
	rm -f vendor/tcc/libtcc.a.musl vendor/tcc/libtcc.a.cosmo
	rm -rf $(MUSL_ROOT)
	$(MAKE) -C vendor/musl clean 2>/dev/null || true
