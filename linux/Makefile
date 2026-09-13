# MaryOS kit. Images are built from source in Docker (builder/); the VM
# runner, the flasher and the app are the Swift package in this directory.
# Virtualization.framework needs codesigned binaries, so `build` signs them
# and `swift run` alone is not enough for the vm commands.
#
# The desktop (MaryUI/linux, Liquid Platinum) is compiled by the builder's ui
# stage from the submodule maryui/ or from MARYUI_DIR=/path/to/MaryUI:
#   make ui                  compile it into out/ui (seconds; Docker)
#   make mary                compile Mary's packages (linux/mary: sewnd, threadd, maryd) into out/mary
#   make desktop / ui-dev    ui.sh: compile, then boot the VM to that desktop (out/ui, restarted on every rebuild);
#                            run again while the VM is up to rebuild into it; ARGS=--image for the embedded desktop
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

ui:
	builder/build.sh ui

mary:
	builder/build.sh mary

image:
	builder/build.sh all $(TARGET)

vm:
	sh vm.sh $(ARGS)

desktop:
	sh ui.sh $(ARGS)

ui-dev:
	sh ui.sh $(ARGS)

flash: build
	$(BIN_CLI) flash --disk $(DISK)

clean:
	rm -rf .build dist work out/ui out/mary

.PHONY: build test cli run-app app rootfs ui mary image vm desktop ui-dev flash clean
