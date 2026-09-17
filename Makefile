# wayrdp — an RDP server for wlroots compositors.
#
# Every input comes from an official Arch repository: wayland-protocols carries
# the capture protocols, wlr-protocols the input ones, freerdp the protocol
# itself. Nothing here needs the AUR.

STAGING := /usr/share/wayland-protocols/staging
WLR     := /usr/share/wlr-protocols/unstable

# virtual-keyboard is the one protocol no Arch package installs: it lives in the
# wlroots source tree and is not shipped. protocols/ holds that copy, taken
# unchanged from wlroots, MIT, and it is the only vendored file here.

BUILD   := build
GEN     := $(BUILD)/gen

# name : path-to-xml
PROTOCOLS := \
	ext-foreign-toplevel-list-v1:$(STAGING)/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml \
	ext-image-capture-source-v1:$(STAGING)/ext-image-capture-source/ext-image-capture-source-v1.xml \
	ext-image-copy-capture-v1:$(STAGING)/ext-image-copy-capture/ext-image-copy-capture-v1.xml \
	wlr-screencopy-unstable-v1:$(WLR)/wlr-screencopy-unstable-v1.xml \
	wlr-virtual-pointer-unstable-v1:$(WLR)/wlr-virtual-pointer-unstable-v1.xml \
	virtual-keyboard-unstable-v1:protocols/virtual-keyboard-unstable-v1.xml

PROTO_NAMES := $(foreach p,$(PROTOCOLS),$(firstword $(subst :, ,$(p))))
PROTO_HDRS  := $(patsubst %,$(GEN)/%-client-protocol.h,$(PROTO_NAMES))
PROTO_SRCS  := $(patsubst %,$(GEN)/%-protocol.c,$(PROTO_NAMES))
PROTO_OBJS  := $(PROTO_SRCS:.c=.o)

DEPS    := wayland-client xkbcommon freerdp3 freerdp-server3 winpr3 libpipewire-0.3 libcrypto
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra
# Dependency headers go in as system headers: FreeRDP/WinPR mark parts of their
# own API deprecated inside their own headers, and a -Werror build should not
# fail on declarations the project does not even call.
CPPFLAGS += -I$(GEN) -Isrc $(patsubst -I%,-isystem %,$(shell pkg-config --cflags $(DEPS)))
LDLIBS  += $(shell pkg-config --libs $(DEPS))

.PHONY: all clean protocols
all: $(BUILD)/wayrdp $(BUILD)/wayrdp-probe

protocols: $(PROTO_HDRS) $(PROTO_SRCS)

# The header has to exist before anything that includes it compiles, and make
# has no way to know that from the .c alone.
$(BUILD)/%.o: src/%.c | $(PROTO_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/wayrdp-probe: $(BUILD)/probe.o $(BUILD)/wayland.o $(PROTO_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/wayrdp: $(BUILD)/main.o $(BUILD)/rdp.o $(BUILD)/config.o $(BUILD)/password.o $(BUILD)/audio.o $(BUILD)/wayland.o $(PROTO_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(GEN)/%-protocol.o: $(GEN)/%-protocol.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

define proto_rule
$(GEN)/$(1)-client-protocol.h: $(2)
	@mkdir -p $$(@D)
	wayland-scanner client-header $$< $$@
$(GEN)/$(1)-protocol.c: $(2)
	@mkdir -p $$(@D)
	wayland-scanner private-code $$< $$@
endef
$(foreach p,$(PROTOCOLS),$(eval $(call proto_rule,$(firstword $(subst :, ,$(p))),$(lastword $(subst :, ,$(p))))))

clean:
	rm -rf $(BUILD)
