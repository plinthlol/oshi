# The version oshi reports when built straight from git with no tag: only
# override it when there really is a tag to name the build after.
VERSION := $(shell git describe --tags --abbrev=0 2>/dev/null)

oshi: oshi.c
	$(CC) oshi.c -o oshi -Wall -Wextra -pedantic $(if $(VERSION),-DOSHI_VERSION=\"$(VERSION)\")

clean:
	rm -f oshi

.PHONY: clean
