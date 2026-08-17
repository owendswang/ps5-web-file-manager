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

VERSION_TAG := v1.6
TITLE_ID    := FMGR88888
PYTHON      ?= python3
STRIP       ?= $(PS5_PAYLOAD_SDK)/bin/prospero-strip
PKG_CONFIG  ?= $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
HOST_CC     ?= cc
HOST_STRIP  ?= strip
HOST_PKG_CONFIG ?= pkg-config

BIN        := web-file-mgr.elf
LINUX_BIN  := web-file-mgr-linux
COMMON_SRCS := src/main.c src/websrv.c src/filemgr.c src/file_response.c src/task.c src/upload.c src/download.c src/text.c src/list.c src/space.c src/fs_util.c src/json_util.c src/path_util.c src/asset.c src/mime.c src/notify.c src/pkg_installer.c
PS5_SRCS    := $(COMMON_SRCS) src/app_installer.c
LINUX_SRCS  := $(COMMON_SRCS)
BASE_ASSETS := $(filter-out %.dds,$(wildcard assets/*))
ifneq ($(filter linux,$(MAKECMDGOALS)),)
ASSETS      := $(BASE_ASSETS)
else
ASSETS      := $(filter-out assets/icon0.png,$(BASE_ASSETS))
endif
GEN_SRCS    := $(patsubst assets/%,gen/%, $(ASSETS:=.c))

CFLAGS := -Oz -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Werror -ffunction-sections -fdata-sections -Isrc -DVERSION_TAG=\"$(VERSION_TAG)\" -DTITLE_ID=\"$(TITLE_ID)\"
CFLAGS += `$(PKG_CONFIG) libmicrohttpd --cflags`
LDFLAGS := -Wl,--gc-sections
LDADD  := `$(PKG_CONFIG) libmicrohttpd --libs`
LDADD  += -lSceIpmi -lSceAppInstUtil
LINUX_CFLAGS := -O2 -flto -Wall -Werror -Isrc -DVERSION_TAG=\"$(VERSION_TAG)\" -DTITLE_ID=\"$(TITLE_ID)\"
LINUX_CFLAGS += `$(HOST_PKG_CONFIG) libmicrohttpd --cflags`
LINUX_LDADD := `$(HOST_PKG_CONFIG) libmicrohttpd --libs` -pthread

.PHONY: all linux deps linux-deps clean

all: deps $(BIN)

linux: linux-deps $(LINUX_BIN)

deps:
	@$(PKG_CONFIG) --exists libmicrohttpd || ./install-libmicrohttpd.sh

linux-deps:
	@$(HOST_PKG_CONFIG) --exists libmicrohttpd || \
	  (echo "libmicrohttpd development package is required for make linux" >&2; exit 1)

gen:
	mkdir gen

clean:
	rm -rf $(BIN) $(LINUX_BIN) gen

gen/%.c: assets/% gen-asset-module.py | gen
	$(PYTHON) gen-asset-module.py --path $* $< > $@

$(BIN): $(PS5_SRCS) $(GEN_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDADD)
	$(STRIP) $@

$(LINUX_BIN): $(LINUX_SRCS) $(GEN_SRCS)
	$(HOST_CC) $(LINUX_CFLAGS) -o $@ $^ $(LINUX_LDADD)
	$(HOST_STRIP) $@
