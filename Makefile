# Convenience wrapper around SwiftPM. Works with GNU make and bmake.
SWIFT ?= swift

build:
	$(SWIFT) build

test:
	$(SWIFT) test

release:
	$(SWIFT) build -c release

app: release
	sh scripts/bundle.sh

clean:
	rm -rf .build dist

.PHONY: build test release app clean
