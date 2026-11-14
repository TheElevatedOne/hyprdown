PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
CC ?= gcc
PKG_CONFIG ?= pkg-config
VERSION ?= 0.3.1

BUILDDIR := build
DISTDIR := dist
BIN := $(DISTDIR)/hyprdown
TARBALL := $(DISTDIR)/hyprdown-$(VERSION)-linux.tar.gz

CFLAGS ?= -O2 -g
# override: these must survive `make CFLAGS=...` on the command line.
override CFLAGS += -std=c11 -Wall -Wextra
override CPPFLAGS += -D_GNU_SOURCE -DHYPRDOWN_VERSION=\"$(VERSION)\" -I src

JSON_CFLAGS := $(shell $(PKG_CONFIG) --cflags json-c)
JSON_LIBS := $(shell $(PKG_CONFIG) --libs json-c)
SYSTEMD_CFLAGS := $(shell $(PKG_CONFIG) --cflags libsystemd 2>/dev/null)
SYSTEMD_LIBS := $(shell $(PKG_CONFIG) --libs libsystemd 2>/dev/null)

ifneq ($(SYSTEMD_LIBS),)
override CPPFLAGS += -DHAVE_LIBSYSTEMD $(SYSTEMD_CFLAGS)
override LDLIBS += $(SYSTEMD_LIBS)
endif

override CPPFLAGS += $(JSON_CFLAGS)
override LDLIBS += $(JSON_LIBS)

SRCS := src/main.c src/ipc.c src/session.c src/proc.c src/action.c src/config.c
OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SRCS))

.PHONY: all clean install uninstall

all: $(BIN) $(TARBALL)

$(BUILDDIR) $(DISTDIR):
	mkdir -p $@

$(BUILDDIR)/%.o: src/%.c src/hyprdown.h | $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BIN): $(OBJS) | $(DISTDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(TARBALL): $(BIN)
	tar -C $(DISTDIR) -czf $@ hyprdown

clean:
	rm -rf $(BUILDDIR) $(DISTDIR)
	rm -f src/*.o hyprdown

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/hyprdown

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/hyprdown
