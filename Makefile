# shclaw — bare metal multi-agent AI daemon in C
#
# A single static binary embedding TLS, HTTP, IRC, JSON, a C compiler,
# and a multi-agent agentic loop.
#
#   make              Show help
#   make musl         Build static Linux binary (~530K)
#   make cosmo        Build cross-platform APE binary (~970K)
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

CF_PROT := $(if $(filter x86_64 i686 i386,$(ARCH)),-fcf-protection,)
# TinyCC keeps the i386 stack 4-byte aligned, gcc code expects 16 bytes
# (SSE): the functions that plugins call must realign it on entry
REALIGN := $(if $(filter i686 i386,$(ARCH)),-mstackrealign,)

ifeq ($(filter x86_64 aarch64 i686 i386,$(ARCH)),)
  STATIC := -static
  PIE    :=
else
  STATIC := -static-pie
  PIE    := -fPIE
endif

# -------------------------------------------------------------------
# musl toolchain
# -------------------------------------------------------------------
MUSL_CC      = musl-gcc
# Function/data sections + --gc-sections drop unused code (~25K smaller)
SECTIONS     = -ffunction-sections -fdata-sections
MUSL_CFLAGS  = -std=gnu11 -Os $(SECTIONS) -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
               -I include \
               -I vendor/bearssl/inc \
               -I vendor/tcc \
               -I vendor/cjson \
               -fstack-protector-strong \
               -fstack-clash-protection \
               -D_FORTIFY_SOURCE=2 \
               $(PIE) \
               $(CF_PROT) \
               $(REALIGN) \
               -fno-delete-null-pointer-checks \
               -fno-strict-overflow \
               -fno-strict-aliasing \
               -Wformat=2 -Wformat-security \
               -Wimplicit-fallthrough \
               $(EXTRA_CFLAGS)
MUSL_LDFLAGS = $(STATIC) -Wl,--gc-sections -L vendor/bearssl/build -L vendor/tcc \
               -Wl,-z,relro,-z,now \
               -Wl,-z,noexecstack \
               -Wl,-z,separate-code
MUSL_LIBS    = -lbearssl -ltcc -lpthread -lm
MUSL_BIN     = shclaw

# -------------------------------------------------------------------
# Cosmopolitan toolchain
# -------------------------------------------------------------------
COSMO_DIR    = ./vendor/cosmo
COSMO_CC     = $(COSMO_DIR)/bin/x86_64-unknown-cosmo-cc
COSMO_AR     = $(COSMO_DIR)/bin/x86_64-unknown-cosmo-ar
COSMO_CFLAGS = -std=gnu11 -Os $(SECTIONS) -Wall -Wextra \
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
	@echo "  make musl           Build static Linux binary (~530K)"
	@echo "  make cosmo          Build multi-platform binary (~970K)"
	@echo "                      Runs on Linux/FreeBSD/NetBSD (x86_64); OpenBSD: make native"
	@echo "  make native         Build static binary with cc (FreeBSD/OpenBSD, run with gmake)"
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
vendor/bearssl/build/libbearssl.a.musl: vendor/bearssl/Makefile
	@# Clean cosmo-built libs if switching toolchains
	@rm -f vendor/bearssl/build/libbearssl.a.cosmo
	$(MAKE) -C vendor/bearssl clean 2>/dev/null || true
	$(MAKE) -C vendor/bearssl CC=$(MUSL_CC) CFLAGS="-fPIC -Os $(SECTIONS)" -j$$(getconf _NPROCESSORS_ONLN)
	@touch $@

vendor/tcc/libtcc.a.musl: vendor/tcc/Makefile
	@rm -f vendor/tcc/libtcc.a.cosmo
	$(MAKE) -C vendor/tcc clean 2>/dev/null || true
	cd vendor/tcc && ./configure --cc=$(MUSL_CC)
	$(MAKE) -C vendor/tcc libtcc.a CC=$(MUSL_CC) \
		CFLAGS="-Wall -Os $(SECTIONS) -DCONFIG_RUNMEM_RO=1 -Wdeclaration-after-statement -Wno-unused-result" -j$$(getconf _NPROCESSORS_ONLN)
	@touch $@

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
		CFLAGS="-Os $(SECTIONS)" \
		DLL=no TOOLS=no TESTS=no \
		-j$$(getconf _NPROCESSORS_ONLN)
	@touch $@

vendor/tcc/libtcc.a.cosmo: vendor/tcc/.cosmo-patched $(COSMO_DIR)/bin/cosmocc
	@rm -f vendor/tcc/libtcc.a.musl
	$(MAKE) -C vendor/tcc clean 2>/dev/null || true
	cd vendor/tcc && ./configure --cc="$(abspath $(COSMO_CC))"
	$(MAKE) -C vendor/tcc libtcc.a \
		CC="$(abspath $(COSMO_CC))" \
		AR="$(abspath $(COSMO_AR))" \
		CFLAGS="-Wall -Os $(SECTIONS) -DCONFIG_RUNMEM_RO=1 -Wdeclaration-after-statement -Wno-unused-result" \
		-j$$(getconf _NPROCESSORS_ONLN)
	@touch $@

# -------------------------------------------------------------------
# musl build
# -------------------------------------------------------------------
musl: $(MUSL_BIN)

$(MUSL_BIN): $(MUSL_OBJS) vendor/bearssl/build/libbearssl.a.musl vendor/tcc/libtcc.a.musl
	$(MUSL_CC) $(MUSL_CFLAGS) $(MUSL_LDFLAGS) -o $@ $(MUSL_OBJS) $(MUSL_LIBS)
	strip -s $@
	@echo "==> Built $(MUSL_BIN) ($$(du -h $(MUSL_BIN) | cut -f1))"

build/%.o: src/%.c include/tc.h include/prompt.h vendor/cjson/cJSON.c | build
	$(MUSL_CC) $(MUSL_CFLAGS) -c -o $@ $<

build/cJSON.o: vendor/cjson/cJSON.c vendor/cjson/cJSON.h | build
	$(MUSL_CC) $(MUSL_CFLAGS) -c -o $@ $<

build:
	mkdir -p build

# FreeBSD/OpenBSD: the musl build with the system compiler
native:
	$(MAKE) musl MUSL_CC=cc

check-native:
	$(MAKE) check MUSL_CC=cc

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

check: $(CHECK_MUSL) vendor/bearssl/build/libbearssl.a.musl vendor/tcc/libtcc.a.musl
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
DIST_KIT  := README.md LICENSE NOTICE etc/config.ini.example \
             $(wildcard etc/agents/*.ini.example) include/tc_plugin.h plugins/_template.c

dist:
	@test -f $(MUSL_BIN) -o -f $(COSMO_BIN) || { echo "Build first: make musl or make cosmo"; exit 1; }
	@mkdir -p dist
	@for pair in "$(MUSL_BIN) $(OS)-$(ARCH)" "$(COSMO_BIN) cosmo-x86_64"; do \
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
