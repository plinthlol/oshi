# The version oshi reports when built straight from git with no tag: only
# override it when there really is a tag to name the build after.
VERSION := $(shell git describe --tags --abbrev=0 2>/dev/null)

# Where 'make install' puts oshi: your own ~/.local/bin, except when run as
# root ('sudo make install'), which puts it in /usr/local/bin for every user.
# Not /bin - that directory is for the system's own programs.
ifeq ($(shell id -u),0)
BINDIR ?= /usr/local/bin
else
BINDIR ?= $(HOME)/.local/bin
endif

oshi: oshi.c
	$(CC) oshi.c -o oshi -Wall -Wextra -pedantic $(if $(VERSION),-DOSHI_VERSION=\"$(VERSION)\")

install: oshi
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m755 oshi $(DESTDIR)$(BINDIR)/oshi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/oshi

clean:
	rm -f oshi

.PHONY: clean install uninstall
