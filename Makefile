# shclaw — bare metal multi-agent AI daemon in C
#
# A single static binary embedding TLS, HTTP, IRC, JSON, a C compiler,
# and a multi-agent agentic loop.
#
#   make              Show help
#   make musl         Build static Linux binary (~515K)
#   make cosmo        Build cross-platform APE binary (~955K)
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
MUSL_CFLAGS  = -std=gnu11 -Os -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
               -I include \
               -I vendor/bearssl/inc \
               -I vendor/tcc \
               -I vendor/cjson \
               -fstack-protector-strong \
               -fstack-clash-protection \
               -D_FORTIFY_SOURCE=2 \
               $(PIE) \
               $(CF_PROT) \
               -fno-delete-null-pointer-checks \
               -fno-strict-overflow \
               -fno-strict-aliasing \
               -Wformat=2 -Wformat-security \
               -Wimplicit-fallthrough \
               $(EXTRA_CFLAGS)
MUSL_LDFLAGS = $(STATIC) -L vendor/bearssl/build -L vendor/tcc \
               -Wl,-z,relro,-z,now \
               -Wl,-z,noexecstack \
               -Wl,-z,separate-code
MUSL_LIBS    = -lbearssl -ltcc -lpthread -ldl -lm
MUSL_BIN     = shclaw

# -------------------------------------------------------------------
# Cosmopolitan toolchain
# -------------------------------------------------------------------
COSMO_DIR    = ./vendor/cosmo
COSMO_CC     = $(COSMO_DIR)/bin/x86_64-unknown-cosmo-cc
COSMO_AR     = $(COSMO_DIR)/bin/x86_64-unknown-cosmo-ar
COSMO_CFLAGS = -std=gnu11 -Os -Wall -Wextra \
               -Wno-unused-parameter -Wno-format-truncation \
               -Wno-missing-field-initializers \
               -I include \
               -I vendor/bearssl/inc \
               -I vendor/tcc \
               -I vendor/cjson \
               $(EXTRA_CFLAGS)
COSMO_LDFLAGS = -s -L vendor/bearssl/build -L vendor/tcc
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
.PHONY: help musl cosmo clean install uninstall docker-image smolbsd

# -------------------------------------------------------------------
# Default: help
# -------------------------------------------------------------------
help:
	@echo "shclaw — bare metal multi-agent AI daemon"
	@echo ""
	@echo "  make musl           Build static Linux binary (~515K)"
	@echo "  make cosmo          Build multi-platform binary (~955K)"
	@echo "                      Runs on Linux/NetBSD/FreeBSD/OpenBSD (x86_64)"
	@echo "  make install        Install to PREFIX (default: /opt/shclaw)"
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
	$(MAKE) -C vendor/bearssl CC=$(MUSL_CC) CFLAGS="-fPIC -Os" -j$$(nproc)
	@touch $@

vendor/tcc/libtcc.a.musl: vendor/tcc/Makefile
	@rm -f vendor/tcc/libtcc.a.cosmo
	$(MAKE) -C vendor/tcc clean 2>/dev/null || true
	cd vendor/tcc && ./configure --cc=$(MUSL_CC)
	$(MAKE) -C vendor/tcc libtcc.a CC=$(MUSL_CC) \
		CFLAGS="-Wall -Os -Wdeclaration-after-statement -Wno-unused-result" -j$$(nproc)
	@touch $@

# -------------------------------------------------------------------
# Vendor: build libraries (cosmo)
# -------------------------------------------------------------------
vendor/tcc/.cosmo-patched: vendor/tcc/Makefile patches/tcc-cosmo.patch
	cd vendor/tcc && patch -p1 -N < ../../patches/tcc-cosmo.patch || true
	@touch $@

vendor/bearssl/build/libbearssl.a.cosmo: vendor/bearssl/Makefile $(COSMO_DIR)/bin/cosmocc
	@# Clean musl-built libs if switching toolchains
	@rm -f vendor/bearssl/build/libbearssl.a.musl
	$(MAKE) -C vendor/bearssl clean 2>/dev/null || true
	$(MAKE) -C vendor/bearssl \
		CC="$(abspath $(COSMO_CC))" \
		AR="$(abspath $(COSMO_AR))" \
		CFLAGS="-Os" \
		DLL=no TOOLS=no TESTS=no \
		-j$$(nproc)
	@touch $@

vendor/tcc/libtcc.a.cosmo: vendor/tcc/.cosmo-patched $(COSMO_DIR)/bin/cosmocc
	@rm -f vendor/tcc/libtcc.a.musl
	$(MAKE) -C vendor/tcc clean 2>/dev/null || true
	cd vendor/tcc && ./configure --cc="$(abspath $(COSMO_CC))"
	$(MAKE) -C vendor/tcc libtcc.a \
		CC="$(abspath $(COSMO_CC))" \
		AR="$(abspath $(COSMO_AR))" \
		CFLAGS="-Wall -Os -Wdeclaration-after-statement -Wno-unused-result" \
		-j$$(nproc)
	@touch $@

# -------------------------------------------------------------------
# musl build
# -------------------------------------------------------------------
musl: $(MUSL_BIN)

$(MUSL_BIN): $(MUSL_OBJS) vendor/bearssl/build/libbearssl.a.musl vendor/tcc/libtcc.a.musl
	$(MUSL_CC) $(MUSL_CFLAGS) $(MUSL_LDFLAGS) -o $@ $(MUSL_OBJS) $(MUSL_LIBS)
	strip -s $@
	@echo "==> Built $(MUSL_BIN) ($$(du -h $(MUSL_BIN) | cut -f1))"

build/%.o: src/%.c include/tc.h vendor/cjson/cJSON.c | build
	$(MUSL_CC) $(MUSL_CFLAGS) -c -o $@ $<

build/cJSON.o: vendor/cjson/cJSON.c vendor/cjson/cJSON.h | build
	$(MUSL_CC) $(MUSL_CFLAGS) -c -o $@ $<

build:
	mkdir -p build

# -------------------------------------------------------------------
# Cosmopolitan APE build
# -------------------------------------------------------------------
cosmo: $(COSMO_BIN)

$(COSMO_BIN): $(COSMO_OBJS) vendor/bearssl/build/libbearssl.a.cosmo vendor/tcc/libtcc.a.cosmo
	$(COSMO_CC) $(COSMO_CFLAGS) $(COSMO_LDFLAGS) -o $@ $(COSMO_OBJS) $(COSMO_LIBS)
	@rm -f $@.bak
	$(COSMO_DIR)/bin/assimilate $@
	@echo "==> Built $(COSMO_BIN) ($$(du -h $(COSMO_BIN) | cut -f1))"
	@echo "    Runs on: Linux, NetBSD, FreeBSD, OpenBSD (x86_64)"

build-cosmo/%.o: src/%.c include/tc.h vendor/cjson/cJSON.c $(COSMO_DIR)/bin/cosmocc | build-cosmo
	$(COSMO_CC) $(COSMO_CFLAGS) -c -o $@ $<

build-cosmo/cJSON.o: vendor/cjson/cJSON.c vendor/cjson/cJSON.h $(COSMO_DIR)/bin/cosmocc | build-cosmo
	$(COSMO_CC) $(COSMO_CFLAGS) -c -o $@ $<

build-cosmo:
	mkdir -p build-cosmo

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
	@echo "    plugins/"
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
	@echo "  docker run -v /path/to/instance:/app/instance shclaw"
	@echo ""
	@echo "Instance directory must contain etc/config.ini"

# -------------------------------------------------------------------
# smolBSD microVM service
# -------------------------------------------------------------------
SMOLBSD_DIR = vendor/smolbsd
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
	rm -f $(SMOLBSD_DIR)/shclaw.com $(SMOLBSD_DIR)/images/shclaw-amd64.img
	rm -rf $(SMOLBSD_DIR)/service/shclaw
	cp $(COSMO_BIN) $(SMOLBSD_DIR)/shclaw.com
	cp smolbsd/Dockerfile $(SMOLBSD_DIR)/dockerfiles/Dockerfile.shclaw
	cd $(SMOLBSD_DIR) && ./smoler.sh build dockerfiles/Dockerfile.shclaw
	@echo ""
	@echo "smolBSD image built: $(SMOLBSD_DIR)/images/shclaw-amd64.img"
	@echo ""
	@echo "Run:"
	@echo "  cd $(SMOLBSD_DIR) && ./startnb.sh -k kernels/netbsd-SMOL \\"
	@echo "    -i images/shclaw-amd64.img -w $(abspath $(AGENT_DIR))"

# -------------------------------------------------------------------
# Clean everything
# -------------------------------------------------------------------
clean:
	rm -rf build build-cosmo $(MUSL_BIN) $(COSMO_BIN) $(COSMO_BIN).dbg $(COSMO_BIN).bak
	rm -f docker/$(MUSL_BIN)
	rm -f $(SMOLBSD_DIR)/shclaw.com $(SMOLBSD_DIR)/dockerfiles/Dockerfile.shclaw
	rm -rf $(SMOLBSD_DIR)/service/shclaw $(SMOLBSD_DIR)/etc/shclaw.conf
	rm -f $(SMOLBSD_DIR)/images/shclaw-amd64.img
	$(MAKE) -C vendor/bearssl clean 2>/dev/null || true
	$(MAKE) -C vendor/tcc clean 2>/dev/null || true
	rm -f vendor/tcc/.cosmo-patched
	rm -f vendor/bearssl/build/libbearssl.a.musl vendor/bearssl/build/libbearssl.a.cosmo
	rm -f vendor/tcc/libtcc.a.musl vendor/tcc/libtcc.a.cosmo
