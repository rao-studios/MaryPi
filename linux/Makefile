# MaryOS kit, the Mac side: the Swift package in this directory (MaryOSKit, the maryos CLI
# and MaryOS.app: the Virtualization.framework runner, the build driver and the flasher).
# The OS itself is the MaryOS submodule in maryos/ (git submodule update --init): distro/,
# builder/ (images are built from source in Docker), the desktop in maryui/ and Mary in mary/.
# Virtualization.framework needs codesigned binaries, so `build` signs them and `swift run`
# alone is not enough for the vm commands.
#
#   make ui                  compile the desktop (maryos/maryui) into maryos/out/ui (seconds; Docker)
#   make mary                compile Mary's packages (maryos/mary: sewnd, threadd, maryd) into maryos/out/mary
#   make vm                  maryos/vm.sh: compile, then boot the VM to that desktop (out/ui, restarted on every rebuild);
#                            run again while the VM is up to rebuild into it; ARGS=--image for the embedded desktop
#   make terminal            maryos/terminal.sh: boot the VM to a login prompt; ARGS=--console for the serial console
#   make flash DISK=disk4    erase disk4 and write the pi5 image (asks first)
#   make lp-tokens           regenerate Sources/LiquidPlatinum/LPTokens.swift from maryos/maryui's lp_tokens.h
#   make lp-tokens-check     fail when LPTokens.swift is out of date (make test runs it)
SWIFT ?= swift
TARGET ?= vm
DISK ?=
ARGS ?=
KIT = maryos
BIN_CLI = .build/debug/maryos
BIN_APP = .build/debug/MaryOSApp

build:
	$(SWIFT) build
	sh scripts/sign.sh $(BIN_CLI) $(BIN_APP)

test: lp-tokens-check
	$(SWIFT) test
	$(MAKE) -C $(KIT) test

cli: build
	$(BIN_CLI) $(ARGS)

run-app: build
	$(BIN_APP)

app:
	sh scripts/bundle.sh

lp-tokens:
	python3 scripts/gen-lp-tokens.py

lp-tokens-check:
	python3 scripts/gen-lp-tokens.py --check

rootfs:
	$(KIT)/builder/build.sh rootfs

ui:
	$(KIT)/builder/build.sh ui

mary:
	$(KIT)/builder/build.sh mary

image:
	$(KIT)/builder/build.sh all $(TARGET)

vm:
	sh $(KIT)/vm.sh $(ARGS)

terminal:
	sh $(KIT)/terminal.sh $(ARGS)

flash: build
	$(BIN_CLI) flash --disk $(DISK)

clean:
	rm -rf .build dist

.PHONY: build test cli run-app app lp-tokens lp-tokens-check rootfs ui mary image vm terminal flash clean
