ifneq ($(filter-out linux linux-deps clean,$(MAKECMDGOALS)),)
  ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
  else
    $(error PS5_PAYLOAD_SDK is undefined)
  endif
endif
ifeq ($(MAKECMDGOALS),)
  ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
  else
    $(error PS5_PAYLOAD_SDK is undefined)
  endif
endif

VERSION_TAG := v0.2
TITLE_ID    := FMGR88888
PYTHON      ?= python3
STRIP       ?= $(PS5_PAYLOAD_SDK)/bin/prospero-strip
PKG_CONFIG  ?= $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
HOST_CC     ?= cc
HOST_STRIP  ?= strip
HOST_PKG_CONFIG ?= pkg-config

BIN        := web-file-mgr.elf
LEGACY_BIN := web-file-mgr-legacy.elf
LINUX_BIN  := web-file-mgr-linux
COMMON_SRCS := src/main.c src/websrv.c src/filemgr.c src/asset.c src/mime.c src/notify.c
PS5_SRCS    := $(COMMON_SRCS) src/app_installer.c
LINUX_SRCS  := $(COMMON_SRCS)
ASSETS      := $(wildcard assets/*)
GEN_SRCS    := $(patsubst assets/%,gen/%, $(ASSETS:=.c))

CFLAGS := -Oz -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Werror -ffunction-sections -fdata-sections -Isrc -DVERSION_TAG=\"$(VERSION_TAG)\" -DTITLE_ID=\"$(TITLE_ID)\"
CFLAGS += `$(PKG_CONFIG) libmicrohttpd --cflags`
LDFLAGS := -Wl,--gc-sections
LEGACY_CFLAGS := $(filter-out -ffunction-sections -fdata-sections,$(CFLAGS))
LEGACY_LDFLAGS :=
LDADD  := `$(PKG_CONFIG) libmicrohttpd --libs`
LDADD  += -lSceIpmi -lSceAppInstUtil
LINUX_CFLAGS := -O2 -Wall -Werror -Isrc -DVERSION_TAG=\"$(VERSION_TAG)\" -DTITLE_ID=\"$(TITLE_ID)\"
LINUX_CFLAGS += `$(HOST_PKG_CONFIG) libmicrohttpd --cflags`
LINUX_LDADD := `$(HOST_PKG_CONFIG) libmicrohttpd --libs` -pthread

.PHONY: all legacy linux deps linux-deps clean

all: deps $(BIN)

legacy: deps $(LEGACY_BIN)

linux: linux-deps $(LINUX_BIN)

deps:
	@$(PKG_CONFIG) --exists libmicrohttpd || ./install-libmicrohttpd.sh

linux-deps:
	@$(HOST_PKG_CONFIG) --exists libmicrohttpd || \
	  (echo "libmicrohttpd development package is required for make linux" >&2; exit 1)

gen:
	mkdir gen

clean:
	rm -rf $(BIN) $(LEGACY_BIN) $(LINUX_BIN) gen

gen/%.c: assets/% gen-asset-module.py gen
	$(PYTHON) gen-asset-module.py --path $* $< > $@

$(BIN): $(PS5_SRCS) $(GEN_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDADD)
	$(STRIP) $@

$(LEGACY_BIN): $(PS5_SRCS) $(GEN_SRCS)
	$(CC) $(LEGACY_CFLAGS) $(LEGACY_LDFLAGS) -o $@ $^ $(LDADD)
	$(STRIP) $@

$(LINUX_BIN): $(LINUX_SRCS) $(GEN_SRCS)
	$(HOST_CC) $(LINUX_CFLAGS) -o $@ $^ $(LINUX_LDADD)
	$(HOST_STRIP) $@
