# Mary in C for MaryOS: one package per Swift repository or Mary target, each
# beside the others so they can be read against their Swift twins (PORTING.md
# maps every file). Plain GNU make + pkg-config, the same shape as MaryUI/linux.
#
#   make               every package's static library, and its tools and daemons
#   make test          build and run <pkg>/tests/test_*.c
#   make install       DESTDIR=… PREFIX=/usr (SHERPA_DIR=… adds sherpa-onnx and the "Hey Mary" model)
#   make check-deps    which optional libraries were found, and which packages they gate
#   make gen           regenerate conduit's protobuf-c code (needs protoc-c)
#   make gen-check     fail when that code or the proto copies are stale
#
# Every output goes under $(O) (default build/), so the source tree may be a
# read-only mount (the MaryPi builder mounts it that way).
O ?= build
PREFIX ?= /usr
DESTDIR ?=
CC ?= cc
AR ?= ar
PKG_CONFIG ?= pkg-config
VERSION := 0.1.0
UNAME := $(shell uname -s)

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration -fPIC -pthread
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DMARY_VERSION=\"$(VERSION)\"

# Packages. A package's include/ is on every path; a package may link only the
# ones named in its DEPS (dependencies first-to-last is the link order).
PKGS := common foundation ambient conduit sewn thread mary-thread skills computer-use brain voice runtime fleet frigate

DEPS_common :=
DEPS_foundation := common
DEPS_ambient := foundation common
DEPS_conduit := common
DEPS_sewn := common
DEPS_thread := conduit common
DEPS_mary-thread := conduit common
DEPS_skills := common
DEPS_computer-use := skills common
DEPS_brain := skills foundation common
DEPS_voice := common
DEPS_runtime := brain voice computer-use skills mary-thread conduit sewn ambient foundation common
DEPS_fleet := common
DEPS_frigate := common

# What a package needs beyond json-c (names from OPTIONAL_NAMES); without them it is skipped.
NEEDS_conduit := NGHTTP2 PROTOBUFC
NEEDS_thread := NGHTTP2 PROTOBUFC
NEEDS_mary-thread := NGHTTP2 PROTOBUFC
NEEDS_runtime := NGHTTP2 PROTOBUFC

# json-c is the one library everything uses.
BASE_PKGS := json-c
HAVE_BASE := $(shell $(PKG_CONFIG) --exists $(BASE_PKGS) 2>/dev/null && echo 1)
BASE_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(BASE_PKGS) 2>/dev/null)
BASE_LIBS := $(shell $(PKG_CONFIG) --libs $(BASE_PKGS) 2>/dev/null) -lm

# The rest are optional per package, the way MaryUI's system apps are: a package
# whose NEEDS are not all found is skipped (make check-deps says which), so the
# pure packages still build and test on a Mac. The builder image has them all.
define optional_pkgs
HAVE_$(1) := $$(shell $$(PKG_CONFIG) --exists $(2) 2>/dev/null && echo 1)
ifeq ($$(HAVE_$(1)),1)
OPT_CFLAGS += $$(shell $$(PKG_CONFIG) --cflags $(2)) -DHAVE_$(1)
OPT_LIBS += $$(shell $$(PKG_CONFIG) --libs $(2))
endif
endef
$(eval $(call optional_pkgs,CURL,libcurl))                  # sewn: Mistral over HTTPS
$(eval $(call optional_pkgs,LWS,libwebsockets))             # sewn: Voxtral Realtime
$(eval $(call optional_pkgs,SSL,openssl))                   # sewn
$(eval $(call optional_pkgs,NGHTTP2,libnghttp2))            # conduit: gRPC over HTTP/2
$(eval $(call optional_pkgs,PROTOBUFC,libprotobuf-c))       # conduit: thread.v1 messages
$(eval $(call optional_pkgs,PIPEWIRE,libpipewire-0.3))      # voice: microphone and speaker
# sherpa-onnx ("Hey Mary") has no pkg-config file. The builder unpacks the release
# pinned in third_party.lock into SHERPA_DIR — include/sherpa-onnx/c-api/c-api.h, lib/,
# model/ and licenses/ (builder/lib/mary.sh) — and passes it here. voice links it; its
# tests find the libraries where they were unpacked, the installed daemons in SHERPA_RPATH.
SHERPA_DIR ?=
SHERPA_RPATH ?= /usr/lib/maryos/sherpa-onnx
HAVE_SHERPA := $(if $(SHERPA_DIR),$(if $(wildcard $(SHERPA_DIR)/include/sherpa-onnx/c-api/c-api.h),1))
ifeq ($(HAVE_SHERPA),1)
OPT_CFLAGS += -I$(abspath $(SHERPA_DIR))/include -DHAVE_SHERPA
LIBS_voice := -L$(abspath $(SHERPA_DIR))/lib -lsherpa-onnx-c-api
TEST_LIBS_voice := -Wl,-rpath,$(abspath $(SHERPA_DIR))/lib
BIN_LIBS_voice := -Wl,-rpath,$(SHERPA_RPATH)
endif
# The daemons need Linux itself: SO_PEERCRED, systemd. (No trailing comment on
# the assignment: make would keep the spaces before it, and HAVE_LINUX would
# never be empty.)
HAVE_LINUX := $(if $(filter Linux,$(UNAME)),1)

OPTIONAL_NAMES := CURL LWS SSL NGHTTP2 PROTOBUFC PIPEWIRE SHERPA LINUX
enabled = $(if $(strip $(foreach n,$(NEEDS_$(1)),$(if $(HAVE_$(n)),,x))),,1)
BUILT_PKGS := $(foreach p,$(PKGS),$(if $(call enabled,$(p)),$(p)))
SKIPPED_PKGS := $(filter-out $(BUILT_PKGS),$(PKGS))

INCLUDES := $(foreach p,$(PKGS),-I$(p)/include) -Itesting
COMPILE = $(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(BASE_CFLAGS) $(OPT_CFLAGS)

srcs_of = $(wildcard $(1)/src/*.c)
lib_of = $(if $(call srcs_of,$(1)),$(O)/lib/lib$(1).a)
libs_of = $(foreach d,$(1) $(DEPS_$(1)),$(call lib_of,$(d)))
# A package's own extra link flags and its dependencies', e.g. $(call dep_flags,runtime,LIBS) picks up LIBS_voice.
dep_flags = $(foreach d,$(1) $(DEPS_$(1)),$($(2)_$(d)))

# strip: foreach joins empty results with spaces, and `ifneq ($(ALL_BINS),)` must see "".
ALL_LIBS := $(strip $(foreach p,$(BUILT_PKGS),$(call lib_of,$(p))))
ALL_BINS := $(strip $(foreach p,$(BUILT_PKGS),$(patsubst $(p)/bin/%.c,$(O)/bin/%,$(wildcard $(p)/bin/*.c))))
ALL_TESTS := $(strip $(foreach p,$(BUILT_PKGS),$(patsubst $(p)/tests/%.c,$(O)/tests/$(p)/%,$(wildcard $(p)/tests/test_*.c))))

.PHONY: all test install check-deps gen gen-check clean
all: $(ALL_LIBS) $(ALL_BINS)
ifneq ($(SKIPPED_PKGS),)
	@echo "make: skipped $(SKIPPED_PKGS) (make check-deps says what they need)"
endif

$(O)/obj/%.o: %.c
ifneq ($(HAVE_BASE),1)
	$(error json-c not found: install libjson-c-dev (Linux) or json-c (Homebrew))
endif
	@mkdir -p $(@D)
	$(COMPILE) -MMD -MP -c $< -o $@

define package
$$(O)/lib/lib$(1).a: $$(patsubst %.c,$$(O)/obj/%.o,$$(call srcs_of,$(1)))
	@mkdir -p $$(@D)
	rm -f $$@
	$$(AR) rcs $$@ $$^

$$(O)/tests/$(1)/%: $(1)/tests/%.c $$(call libs_of,$(1)) testing/mary_test.h
	@mkdir -p $$(@D)
	$$(COMPILE) -o $$@ $$< $$(call libs_of,$(1)) $$(call dep_flags,$(1),LIBS) $$(call dep_flags,$(1),TEST_LIBS) $$(BASE_LIBS) $$(OPT_LIBS)

$$(O)/bin/%: $(1)/bin/%.c $$(call libs_of,$(1))
	@mkdir -p $$(@D)
	$$(COMPILE) -o $$@ $$< $$(call libs_of,$(1)) $$(call dep_flags,$(1),LIBS) $$(call dep_flags,$(1),BIN_LIBS) $$(BASE_LIBS) $$(OPT_LIBS)
endef
$(foreach p,$(PKGS),$(eval $(call package,$(p))))

test: $(ALL_TESTS)
	@fail=0; for t in $(ALL_TESTS); do echo "== $$t"; MARY_SRC=$(CURDIR) $$t || fail=1; done; [ $$fail = 0 ]

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/doc/mary $(DESTDIR)$(PREFIX)/share/mary
ifneq ($(ALL_BINS),)
	install -m 755 $(ALL_BINS) $(DESTDIR)$(PREFIX)/bin/
endif
	install -m 644 README.md PORTING.md $(DESTDIR)$(PREFIX)/share/doc/mary/
	install -m 644 voice/data/keywords.txt $(DESTDIR)$(PREFIX)/share/mary/keywords.txt
ifeq ($(HAVE_SHERPA),1)
	install -d $(DESTDIR)$(SHERPA_RPATH) $(DESTDIR)$(PREFIX)/share/mary/models/kws $(DESTDIR)$(PREFIX)/share/doc/mary/third_party
	install -m 644 $(SHERPA_DIR)/lib/*.so $(DESTDIR)$(SHERPA_RPATH)/
	install -m 644 $(SHERPA_DIR)/model/*.onnx $(SHERPA_DIR)/model/tokens.txt $(DESTDIR)$(PREFIX)/share/mary/models/kws/
	install -m 644 $(SHERPA_DIR)/licenses/* $(DESTDIR)$(PREFIX)/share/doc/mary/third_party/
endif

check-deps:
	@$(PKG_CONFIG) --print-errors --exists $(BASE_PKGS) && echo "json-c ok"
	@$(foreach n,$(OPTIONAL_NAMES),echo "$(n): $(if $(HAVE_$(n)),found,missing)";)
	@echo "built: $(BUILT_PKGS)"
	@echo "skipped: $(if $(SKIPPED_PKGS),$(foreach p,$(SKIPPED_PKGS),$(p) (needs $(NEEDS_$(p)))),none)"

# conduit: Conduit's protos compiled with protobuf-c. The generated files are committed —
# the builder image has protoc-c, a Mac may not — and moved under include/conduit/, so
# the .c files include "conduit/<name>.pb-c.h". gen-check keeps both the generated code
# and the proto copies (against a sibling Conduit checkout) honest.
PROTOC_C ?= protoc-c
CONDUIT_DIR ?= ../../../Conduit
PROTO_NAMES := thread fleet
# protoc-c names a service member after each RPC, and thread.proto's Register becomes a
# member called `register` — a C keyword. conduit routes calls itself and never uses the
# service stubs, so the headers get `register_` instead.
PB_FIXUP := sed -e 's/(\*register)(/(*register_)(/' -e 's/function_prefix__ \#\# register,/function_prefix__ \#\# register_,/'

gen:
	@command -v $(PROTOC_C) > /dev/null || { echo "gen: $(PROTOC_C) not found (the builder image has it)"; exit 1; }
	@rm -rf $(O)/gen && mkdir -p $(O)/gen
	cd conduit/protos && $(PROTOC_C) --c_out=$(abspath $(O))/gen $(PROTO_NAMES:%=%.proto)
	@for p in $(PROTO_NAMES); do \
	  sed 's|#include "'$$p'.pb-c.h"|#include "conduit/'$$p'.pb-c.h"|' $(O)/gen/$$p.pb-c.c > conduit/src/$$p.pb-c.c; \
	  $(PB_FIXUP) $(O)/gen/$$p.pb-c.h > conduit/include/conduit/$$p.pb-c.h; \
	done; echo "gen: conduit/src/*.pb-c.c and conduit/include/conduit/*.pb-c.h written"

gen-check:
	@if [ -d "$(CONDUIT_DIR)/Protos" ]; then \
	  for p in $(PROTO_NAMES); do cmp -s conduit/protos/$$p.proto "$(CONDUIT_DIR)/Protos/$$p.proto" \
	    || { echo "gen-check: conduit/protos/$$p.proto differs from $(CONDUIT_DIR)/Protos"; exit 1; }; done; \
	else echo "gen-check: no Conduit checkout at $(CONDUIT_DIR); the proto copies were not compared"; fi
	@command -v $(PROTOC_C) > /dev/null || { echo "gen-check: $(PROTOC_C) not found; the generated code was not compared"; exit 0; }; \
	tmp=$$(mktemp -d); (cd conduit/protos && $(PROTOC_C) --c_out=$$tmp $(PROTO_NAMES:%=%.proto)) || exit 1; \
	for p in $(PROTO_NAMES); do \
	  sed 's|#include "'$$p'.pb-c.h"|#include "conduit/'$$p'.pb-c.h"|' $$tmp/$$p.pb-c.c | cmp -s - conduit/src/$$p.pb-c.c \
	    || { echo "gen-check: conduit/src/$$p.pb-c.c is stale (make gen)"; rm -rf $$tmp; exit 1; }; \
	  $(PB_FIXUP) $$tmp/$$p.pb-c.h | cmp -s - conduit/include/conduit/$$p.pb-c.h \
	    || { echo "gen-check: conduit/include/conduit/$$p.pb-c.h is stale (make gen)"; rm -rf $$tmp; exit 1; }; \
	done; rm -rf $$tmp; echo "gen-check: the protos and generated code are current"

clean:
	rm -rf $(O)

-include $(shell find $(O)/obj -name '*.d' 2>/dev/null)
