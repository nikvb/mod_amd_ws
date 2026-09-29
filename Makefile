# mod_amd_ws -- build against the FreeSWITCH that is installed on this host.
#
#   make                      build mod_amd_ws.so (headers via pkg-config freeswitch, or FS_INCLUDES=)
#   make install              install into the running FreeSWITCH's mod_dir (FS_MODDIR= to override)
#   make test-unit            engine + WebSocket client against the mock server (no FreeSWITCH needed)
#   make show-config          print what was detected
#   make installer            regenerate install.sh (embeds the sources)
#   make test                 unit tests + FreeSWITCH integration tests (FS_BIN=...)
#
# Overrides: FS_INCLUDES=-I/path/include/freeswitch  FS_LIBS=-L/path/lib -lfreeswitch
#            FS_MODDIR=/usr/lib/freeswitch/mod  PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig

MODNAME   = mod_amd_ws.so
SRCS      = src/mod_amd_ws.c src/amd_engine.c src/ws_client.c src/local_amd.c
OBJS      = $(SRCS:.c=.o)
HDRS      = src/amd_engine.h src/ws_client.h src/local_amd.h

PKG_CONFIG ?= pkg-config
FS_PC_DIRS := /usr/local/freeswitch/lib/pkgconfig:/usr/lib/pkgconfig:/usr/lib64/pkgconfig:/usr/share/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig
FS_INCLUDES ?= $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH):$(FS_PC_DIRS)" $(PKG_CONFIG) --cflags freeswitch 2>/dev/null)
FS_LIBS     ?= $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH):$(FS_PC_DIRS)" $(PKG_CONFIG) --libs freeswitch 2>/dev/null)
SSL_CFLAGS  ?= $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
SSL_LIBS    ?= $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null || echo -lssl -lcrypto)

WARN    = -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wformat=2 -Wshadow \
          -Wno-discarded-qualifiers -Wno-format-nonliteral
CFLAGS ?= -O2 -g
override CFLAGS += -fPIC -std=gnu99 -pthread $(WARN) $(FS_INCLUDES) $(SSL_CFLAGS)
LDFLAGS ?=
override LDFLAGS += -shared -pthread -Wl,--version-script=src/exports.map -Wl,-Bsymbolic

FS_MODDIR ?= $(shell ./fs-detect.sh --moddir 2>/dev/null)

.PHONY: all clean install show-config test-unit test check-headers installer check-installer

all: check-headers $(MODNAME)

check-headers:
	@if [ -z "$(strip $(FS_INCLUDES))" ]; then \
	  echo "ERROR: FreeSWITCH headers not found (pkg-config freeswitch)." >&2; \
	  echo "       Install the dev package (Debian: libfreeswitch-dev, RPM: freeswitch-devel, Alpine: freeswitch-dev)" >&2; \
	  echo "       or pass FS_INCLUDES=-I/usr/local/freeswitch/include/freeswitch, or run ./install.sh" >&2; \
	  exit 1; fi

src/%.o: src/%.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(MODNAME): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(FS_LIBS) $(SSL_LIBS)
	@if command -v nm >/dev/null; then \
	  nm -D --defined-only $@ | grep -q ' mod_amd_ws_module_interface$$' || { echo "ERROR: module interface symbol missing" >&2; exit 1; }; fi
	@echo "built $@"

install: $(MODNAME)
	@if [ -z "$(FS_MODDIR)" ]; then echo "ERROR: cannot find the FreeSWITCH module directory; pass FS_MODDIR=" >&2; exit 1; fi
	@if [ -f "$(FS_MODDIR)/$(MODNAME)" ]; then cp -p "$(FS_MODDIR)/$(MODNAME)" "$(FS_MODDIR)/$(MODNAME).bak.$$(date +%Y%m%d%H%M%S)"; fi
	install -m 0755 $(MODNAME) "$(FS_MODDIR)/$(MODNAME)"
	@echo "installed $(FS_MODDIR)/$(MODNAME)"

show-config:
	@echo "FS_INCLUDES = $(FS_INCLUDES)"
	@echo "FS_LIBS     = $(FS_LIBS)"
	@echo "SSL_LIBS    = $(SSL_LIBS)"
	@echo "FS_MODDIR   = $(FS_MODDIR)"
	@./fs-detect.sh --show 2>/dev/null || true

test/engine_cli: test/engine_cli.c src/amd_engine.c src/ws_client.c src/local_amd.c $(HDRS)
	$(CC) -O2 -g -std=gnu99 -pthread $(WARN) -o $@ test/engine_cli.c src/amd_engine.c src/ws_client.c src/local_amd.c $(SSL_CFLAGS) $(SSL_LIBS)

test-unit: test/engine_cli
	./test/unit.sh

test: $(MODNAME) test-unit
	python3 test/fs_test.py

installer:
	./tools/gen-installer.sh

# CI: install.sh must embed the current sources
check-installer:
	./tools/check-embedded.sh

clean:
	rm -f $(OBJS) $(MODNAME) test/engine_cli
