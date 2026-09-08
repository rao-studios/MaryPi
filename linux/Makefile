# MaryOS kit. Images are built from source in Docker (builder/); the VM
# runner, the flasher and the app are the Swift package in this directory.
# Virtualization.framework needs codesigned binaries, so `build` signs them
# and `swift run` alone is not enough for the vm commands.
SWIFT ?= swift
TARGET ?= vm
DISK ?=
ARGS ?=
BIN_CLI = .build/debug/maryos
BIN_APP = .build/debug/MaryOSApp

build:
	$(SWIFT) build
	sh scripts/sign.sh $(BIN_CLI) $(BIN_APP)

test:
	$(SWIFT) test
	python3 builder/tests/unzboot_test.py

cli: build
	$(BIN_CLI) $(ARGS)

run-app: build
	$(BIN_APP)

app:
	sh scripts/bundle.sh

rootfs:
	builder/build.sh rootfs

image:
	builder/build.sh all $(TARGET)

vm:
	sh vm.sh $(ARGS)

flash: build
	$(BIN_CLI) flash --disk $(DISK)

clean:
	rm -rf .build dist work

.PHONY: build test cli run-app app rootfs image vm flash clean
