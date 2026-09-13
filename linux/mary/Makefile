# Mary in C for MaryOS: one package per Swift repository or Mary target, each
# beside the others so they can be read against their Swift twins (PORTING.md
# maps every file). Plain GNU make + pkg-config, the same shape as MaryUI/linux.
#
#   make               every package's static library, and its tools and daemons
#   make test          build and run <pkg>/tests/test_*.c
#   make install       DESTDIR=… PREFIX=/usr
#   make check-deps    which optional libraries were found, and which packages they gate
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
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration -fPIC
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
DEPS_runtime := brain voice computer-use skills mary-thread conduit ambient foundation common
DEPS_fleet := common
DEPS_frigate := common

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
# The daemons need Linux itself: SO_PEERCRED, systemd. (No trailing comment on
# the assignment: make would keep the spaces before it, and HAVE_LINUX would
# never be empty.)
HAVE_LINUX := $(if $(filter Linux,$(UNAME)),1)

OPTIONAL_NAMES := CURL LWS SSL NGHTTP2 PROTOBUFC PIPEWIRE LINUX
enabled = $(if $(strip $(foreach n,$(NEEDS_$(1)),$(if $(HAVE_$(n)),,x))),,1)
BUILT_PKGS := $(foreach p,$(PKGS),$(if $(call enabled,$(p)),$(p)))
SKIPPED_PKGS := $(filter-out $(BUILT_PKGS),$(PKGS))

INCLUDES := $(foreach p,$(PKGS),-I$(p)/include) -Itesting
COMPILE = $(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(BASE_CFLAGS) $(OPT_CFLAGS)

srcs_of = $(wildcard $(1)/src/*.c)
lib_of = $(if $(call srcs_of,$(1)),$(O)/lib/lib$(1).a)
libs_of = $(foreach d,$(1) $(DEPS_$(1)),$(call lib_of,$(d)))

# strip: foreach joins empty results with spaces, and `ifneq ($(ALL_BINS),)` must see "".
ALL_LIBS := $(strip $(foreach p,$(BUILT_PKGS),$(call lib_of,$(p))))
ALL_BINS := $(strip $(foreach p,$(BUILT_PKGS),$(patsubst $(p)/bin/%.c,$(O)/bin/%,$(wildcard $(p)/bin/*.c))))
ALL_TESTS := $(strip $(foreach p,$(BUILT_PKGS),$(patsubst $(p)/tests/%.c,$(O)/tests/$(p)/%,$(wildcard $(p)/tests/test_*.c))))

.PHONY: all test install check-deps clean
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
	$$(COMPILE) -o $$@ $$< $$(call libs_of,$(1)) $$(BASE_LIBS) $$(OPT_LIBS)

$$(O)/bin/%: $(1)/bin/%.c $$(call libs_of,$(1))
	@mkdir -p $$(@D)
	$$(COMPILE) -o $$@ $$< $$(call libs_of,$(1)) $$(BASE_LIBS) $$(OPT_LIBS)
endef
$(foreach p,$(PKGS),$(eval $(call package,$(p))))

test: $(ALL_TESTS)
	@fail=0; for t in $(ALL_TESTS); do echo "== $$t"; MARY_SRC=$(CURDIR) $$t || fail=1; done; [ $$fail = 0 ]

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/doc/mary
ifneq ($(ALL_BINS),)
	install -m 755 $(ALL_BINS) $(DESTDIR)$(PREFIX)/bin/
endif
	install -m 644 README.md PORTING.md $(DESTDIR)$(PREFIX)/share/doc/mary/

check-deps:
	@$(PKG_CONFIG) --print-errors --exists $(BASE_PKGS) && echo "json-c ok"
	@$(foreach n,$(OPTIONAL_NAMES),echo "$(n): $(if $(HAVE_$(n)),found,missing)";)
	@echo "built: $(BUILT_PKGS)"
	@echo "skipped: $(if $(SKIPPED_PKGS),$(foreach p,$(SKIPPED_PKGS),$(p) (needs $(NEEDS_$(p)))),none)"

clean:
	rm -rf $(O)

-include $(shell find $(O)/obj -name '*.d' 2>/dev/null)
