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

# install also removes leftovers from the old lowercase naming (oshi,
# oshi.desktop, oshi.svg), and refreshes the desktop + icon caches when
# the tools are around.

Oshi: oshi.c
	$(CC) oshi.c -o Oshi -Wall -Wextra -pedantic $(if $(VERSION),-DOSHI_VERSION=\"$(VERSION)\")

install: Oshi Oshi.desktop Oshi.svg
	mkdir -p $(DESTDIR)$(BINDIR) \
	  $(DESTDIR)$(SHAREDIR)/applications \
	  $(DESTDIR)$(SHAREDIR)/icons/hicolor/scalable/apps
	install -m755 Oshi $(DESTDIR)$(BINDIR)/Oshi
	install -m644 Oshi.desktop $(DESTDIR)$(SHAREDIR)/applications/Oshi.desktop
	install -m644 Oshi.svg $(DESTDIR)$(SHAREDIR)/icons/hicolor/scalable/apps/Oshi.svg
	rm -f $(DESTDIR)$(BINDIR)/oshi \
	  $(DESTDIR)$(SHAREDIR)/applications/oshi.desktop \
	  $(DESTDIR)$(SHAREDIR)/icons/hicolor/scalable/apps/oshi.svg
	@command -v update-desktop-database >/dev/null 2>&1 && \
	  update-desktop-database $(DESTDIR)$(SHAREDIR)/applications 2>/dev/null; true
	@command -v gtk-update-icon-cache >/dev/null 2>&1 && \
	  gtk-update-icon-cache -f -t $(DESTDIR)$(SHAREDIR)/icons/hicolor 2>/dev/null; true

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/Oshi \
	  $(DESTDIR)$(SHAREDIR)/applications/Oshi.desktop \
	  $(DESTDIR)$(SHAREDIR)/icons/hicolor/scalable/apps/Oshi.svg \
	  $(DESTDIR)$(BINDIR)/oshi \
	  $(DESTDIR)$(SHAREDIR)/applications/oshi.desktop \
	  $(DESTDIR)$(SHAREDIR)/icons/hicolor/scalable/apps/oshi.svg
	@command -v update-desktop-database >/dev/null 2>&1 && \
	  update-desktop-database $(DESTDIR)$(SHAREDIR)/applications 2>/dev/null; true
	@command -v gtk-update-icon-cache >/dev/null 2>&1 && \
	  gtk-update-icon-cache -f -t $(DESTDIR)$(SHAREDIR)/icons/hicolor 2>/dev/null; true

clean:
	rm -f Oshi oshi

.PHONY: clean install uninstall
