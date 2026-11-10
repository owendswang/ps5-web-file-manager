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

VERSION_TAG := v1.14
TITLE_ID    := FMGR88888
PYTHON      ?= python3
CMAKE       ?= cmake
STRIP       ?= $(PS5_PAYLOAD_SDK)/bin/prospero-strip
PKG_CONFIG  ?= $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
HOST_CC     ?= cc
HOST_STRIP  ?= strip
HOST_PKG_CONFIG ?= pkg-config

BIN        := web-file-mgr.elf
LINUX_BIN  := web-file-mgr-linux
PS5_DEBUG  := $(if $(filter debug,$(MAKECMDGOALS)),1,0)
PS5_BUILD_DIR := .build-ps5/$(if $(filter debug,$(MAKECMDGOALS)),debug,release)
PS5_BUILD_BIN := $(PS5_BUILD_DIR)/$(BIN)
COMMON_SRCS := src/main.c src/websrv.c src/filemgr.c src/file_response.c src/task.c src/upload.c src/download.c src/text.c src/list.c src/space.c src/fs_util.c src/json_util.c src/path_util.c src/asset.c src/mime.c src/notify.c src/pkg_installer.c src/pkg_info.c src/archive_extract.c src/archive_helper.c src/smb.c src/remote.c src/vfs.c src/transfer.c vendor/yxml/yxml.c
PS5_SRCS    := $(COMMON_SRCS) src/app_installer.c src/browser_patch.c .build-fullscreen/hbc_patch.c
HEADERS     := $(wildcard src/*.h)
FULLSCREEN_HEAD := $(shell if test -f vendor/browser-fullscreen/.git; then git -C vendor/browser-fullscreen rev-parse --git-path HEAD; fi)
FULLSCREEN_INPUTS := $(FULLSCREEN_HEAD) $(wildcard vendor/browser-fullscreen/src/*.[ch])
SMB2_HEAD   := $(shell if test -f vendor/libsmb2/.git; then git -C vendor/libsmb2 rev-parse --git-path HEAD; fi)
SMB2_INPUTS := $(SMB2_HEAD) $(wildcard vendor/libsmb2/CMakeLists.txt vendor/libsmb2/cmake/* vendor/libsmb2/lib/* vendor/libsmb2/include/*.h vendor/libsmb2/include/smb2/*)
SMB2_CMAKE_FLAGS := -DBUILD_SHARED_LIBS=OFF -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF -DENABLE_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release
SMB2_LINUX_LIB := .build-smb2/linux/build/lib/libsmb2.a
SMB2_PS5_LIB := .build-smb2/ps5/build/lib/libsmb2.a
CURL_HEAD := $(shell git -C vendor/curl rev-parse --git-path HEAD 2>/dev/null)
CURL_INPUTS := $(CURL_HEAD) $(wildcard vendor/curl/lib/*.[ch] vendor/curl/CMake/*.cmake) vendor/curl/CMakeLists.txt
CURL_LIB := .build-curl/ps5/lib/libcurl.a
CURL_LINUX_LIB := .build-curl/linux/lib/libcurl.a
CURL_FLAGS := -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DCURL_ENABLE_EXPORT_TARGET=OFF -DCURL_USE_PKGCONFIG=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF -DCURL_USE_GSSAPI=OFF -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_TELNET=ON -DCURL_DISABLE_TFTP=ON -DCURL_DISABLE_POP3=ON -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_SMTP=ON -DCURL_DISABLE_GOPHER=ON -DCURL_DISABLE_MQTT=ON -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_FILE=ON -DENABLE_THREADED_RESOLVER=OFF -DCMAKE_BUILD_TYPE=MinSizeRel
CURL_FLAGS += -DCURL_DISABLE_IPFS=ON -DCURL_DISABLE_WEBSOCKETS=ON -DCURL_DISABLE_ALTSVC=ON -DCURL_DISABLE_HSTS=ON -DCURL_DISABLE_COOKIES=ON -DCURL_DISABLE_PROXY=ON -DCURL_ENABLE_NTLM=OFF
CURL_FLAGS += -DCURL_DISABLE_MIME=ON -DCURL_DISABLE_FORM_API=ON -DCURL_DISABLE_NETRC=ON -DCURL_DISABLE_AWS=ON -DCURL_DISABLE_PROGRESS_METER=ON
CURL_FLAGS += -DCURL_ENABLE_SSL=OFF -DCURL_USE_OPENSSL=OFF -DCURL_USE_MBEDTLS=OFF
# Keep FTP, HTTP/WebDAV and Basic/Digest; omit unused APIs and connection features.
CURL_FLAGS += -DCURL_DISABLE_BEARER_AUTH=ON -DCURL_DISABLE_KERBEROS_AUTH=ON -DCURL_DISABLE_NEGOTIATE_AUTH=ON -DCURL_DISABLE_DOH=ON -DCURL_DISABLE_GETOPTIONS=ON -DCURL_DISABLE_HEADERS_API=ON
CURL_FLAGS += -DCURL_DISABLE_BINDLOCAL=ON -DCURL_DISABLE_SOCKETPAIR=ON -DCURL_DISABLE_SHUFFLE_DNS=ON -DENABLE_UNIX_SOCKETS=OFF
# Verbose tracing is unused; curl's error buffer and our debug logs remain available.
CURL_FLAGS += -DCURL_DISABLE_VERBOSE_STRINGS=ON -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DBUILD_EXAMPLES=OFF -DENABLE_CURL_MANUAL=OFF
LINUX_SRCS  := $(COMMON_SRCS)
BASE_ASSETS := $(filter-out %.dds,$(wildcard assets/*))
ifneq ($(filter linux,$(MAKECMDGOALS)),)
ASSETS      := $(BASE_ASSETS)
else
ASSETS      := $(filter-out assets/icon0.png,$(BASE_ASSETS))
endif
GEN_SRCS    := $(patsubst assets/%,gen/%, $(ASSETS:=.c))

CFLAGS := -Oz -flto=thin -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Werror -ffunction-sections -fdata-sections -Isrc -DVERSION_TAG=\"$(VERSION_TAG)\" -DTITLE_ID=\"$(TITLE_ID)\"
CFLAGS += `$(PKG_CONFIG) libmicrohttpd --cflags`
CFLAGS += -DWFM_DEBUG=$(PS5_DEBUG)
# Fullscreen release flags: fast native anchors only; live plan & safe scan omitted.
CFLAGS += -DFULLSCREEN_LIVE_PLAN=0
CFLAGS += -Ivendor/libsmb2/include
CFLAGS += -Ivendor/curl/include -Ivendor/yxml -DCURL_STATICLIB
LDFLAGS := -Wl,--gc-sections
LDADD  := `$(PKG_CONFIG) libmicrohttpd --libs`
LDADD  += -lSceIpmi -lSceAppInstUtil -lSceUserService -lSceSystemService
LINUX_CFLAGS := -O2 -flto=auto -Wall -Werror -Isrc -DVERSION_TAG=\"$(VERSION_TAG)\" -DTITLE_ID=\"$(TITLE_ID)\"
LINUX_CFLAGS += `$(HOST_PKG_CONFIG) libmicrohttpd --cflags`
LINUX_CFLAGS += -DWFM_DEBUG=1
LINUX_CFLAGS += -Ivendor/libsmb2/include
LINUX_CFLAGS += -Ivendor/curl/include -Ivendor/yxml -DCURL_STATICLIB
LINUX_LDADD := `$(HOST_PKG_CONFIG) libmicrohttpd --libs` -pthread

.PHONY: all debug linux deps linux-deps clean force-ps5-copy
.SECONDARY: .build-smb2/linux/source.stamp .build-smb2/ps5/source.stamp

all: deps $(BIN)

debug: deps $(BIN)

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
	rm -rf .build-ps5/release .build-ps5/debug
	rm -rf .build-smb2
	rm -rf .build-fullscreen
	rm -rf .build-curl

gen/%.c: assets/% gen-asset-module.py | gen
	$(PYTHON) gen-asset-module.py --path $* $< > $@

$(BIN): $(PS5_BUILD_BIN) force-ps5-copy
	cp $< $@

$(PS5_BUILD_BIN): $(PS5_SRCS) $(HEADERS) $(GEN_SRCS) assets/icon0.png assets/param.json Makefile $(SMB2_PS5_LIB) $(CURL_LIB) .build-fullscreen/source.stamp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(SMB2_PS5_LIB) $(CURL_LIB) $(LDADD)
	$(STRIP) $@

$(LINUX_BIN): $(LINUX_SRCS) $(HEADERS) $(GEN_SRCS) Makefile $(SMB2_LINUX_LIB) $(CURL_LINUX_LIB)
	$(HOST_CC) $(LINUX_CFLAGS) -o $@ $(filter %.c,$^) $(SMB2_LINUX_LIB) $(CURL_LINUX_LIB) $(LINUX_LDADD)
	$(HOST_STRIP) $@

.build-fullscreen/source.stamp: Makefile patches/browser-fullscreen-embed.patch .gitmodules $(FULLSCREEN_INPUTS)
	@test -f vendor/browser-fullscreen/src/main.c || \
	  (echo "Initialize fullscreen with: git submodule update --init --recursive" >&2; exit 1)
	mkdir -p .build-fullscreen
	cp vendor/browser-fullscreen/src/*.[ch] .build-fullscreen/
	git apply --directory=.build-fullscreen patches/browser-fullscreen-embed.patch
	touch $@

.build-fullscreen/hbc_patch.c: .build-fullscreen/source.stamp
	@test -f $@

.build-smb2/%/source.stamp: Makefile patches/libsmb2-rename-replace.patch .gitmodules $(SMB2_INPUTS)
	@test -f vendor/libsmb2/include/smb2/libsmb2.h || \
	  (echo "Initialize libsmb2 with: git submodule update --init --recursive" >&2; exit 1)
	rm -rf .build-smb2/$*/source
	mkdir -p .build-smb2/$*/source
	cp -R vendor/libsmb2/. .build-smb2/$*/source/
	rm -f .build-smb2/$*/source/.git
	git apply --directory=.build-smb2/$*/source patches/libsmb2-rename-replace.patch
	touch $@

$(SMB2_LINUX_LIB): .build-smb2/linux/source.stamp
	$(CMAKE) -S .build-smb2/linux/source -B .build-smb2/linux/build $(SMB2_CMAKE_FLAGS) -DCMAKE_C_COMPILER="$(HOST_CC)"
	$(CMAKE) --build .build-smb2/linux/build --target smb2 --parallel 4

$(SMB2_PS5_LIB): .build-smb2/ps5/source.stamp
	$(CMAKE) -S .build-smb2/ps5/source -B .build-smb2/ps5/build $(SMB2_CMAKE_FLAGS) \
	  -DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
	  -DCMAKE_C_COMPILER="$(PS5_PAYLOAD_SDK)/bin/prospero-clang" \
	  -DCMAKE_AR="$(PS5_PAYLOAD_SDK)/bin/prospero-ar" \
	  -DCMAKE_RANLIB="$(PS5_PAYLOAD_SDK)/bin/prospero-ranlib" \
	  '-DCMAKE_C_FLAGS=-flto=thin -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables' \
	  '-DCMAKE_C_FLAGS_RELEASE=-Oz -DNDEBUG'
	$(CMAKE) --build .build-smb2/ps5/build --target smb2 --parallel 4

$(CURL_LINUX_LIB): $(CURL_INPUTS) Makefile
	$(CMAKE) -S vendor/curl -B .build-curl/linux $(CURL_FLAGS) -DCMAKE_C_FLAGS='-ffunction-sections -fdata-sections'
	$(CMAKE) --build .build-curl/linux --target libcurl_static --parallel 4

$(CURL_LIB): $(CURL_INPUTS) Makefile
	$(CMAKE) -S vendor/curl -B .build-curl/ps5 $(CURL_FLAGS) \
	  -DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
	  -DCMAKE_C_COMPILER=$(PS5_PAYLOAD_SDK)/bin/prospero-clang \
	  -DCMAKE_FIND_ROOT_PATH=$(PS5_PAYLOAD_SDK)/target -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
	  '-DCMAKE_C_FLAGS_MINSIZEREL=-Oz -DNDEBUG' \
	  -DCMAKE_C_FLAGS='-flto=thin -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables' \
	  -DCMAKE_AR=$(PS5_PAYLOAD_SDK)/bin/prospero-ar -DCMAKE_RANLIB=$(PS5_PAYLOAD_SDK)/bin/prospero-ranlib
	$(CMAKE) --build .build-curl/ps5 --target libcurl_static --parallel 4
