# The version oshi reports when built straight from git with no tag: only
# override it when there really is a tag to name the build after.
VERSION := $(shell git describe --tags --abbrev=0 2>/dev/null)

# Where 'make install' puts oshi: your own ~/.local (bin + share, the XDG
# user dirs), except when run as root ('sudo make install'), which puts it
# in /usr/local for every user. Not /bin - that directory is for the
# system's own programs.
ifeq ($(shell id -u),0)
BINDIR ?= /usr/local/bin
SHAREDIR ?= /usr/local/share
else
BINDIR ?= $(HOME)/.local/bin
SHAREDIR ?= $(HOME)/.local/share
endif

oshi: oshi.c
	$(CC) oshi.c -o oshi -Wall -Wextra -pedantic $(if $(VERSION),-DOSHI_VERSION=\"$(VERSION)\")

install: oshi oshi.desktop
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(SHAREDIR)/applications
	install -m755 oshi $(DESTDIR)$(BINDIR)/oshi
	install -m644 oshi.desktop $(DESTDIR)$(SHAREDIR)/applications/oshi.desktop
	@command -v update-desktop-database >/dev/null 2>&1 && \
	  update-desktop-database $(DESTDIR)$(SHAREDIR)/applications 2>/dev/null; true

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/oshi $(DESTDIR)$(SHAREDIR)/applications/oshi.desktop
	@command -v update-desktop-database >/dev/null 2>&1 && \
	  update-desktop-database $(DESTDIR)$(SHAREDIR)/applications 2>/dev/null; true

clean:
	rm -f oshi

.PHONY: clean install uninstall
