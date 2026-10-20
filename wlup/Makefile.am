# Backends are in subdirectories, built into the same module
AUTOMAKE_OPTIONS = subdir-objects

AM_CPPFLAGS = \
  -DXRDP_CFG_PATH=\"${sysconfdir}/${sysconfsubdir}\" \
  -DXRDP_SBIN_PATH=\"${sbindir}\" \
  -DXRDP_SHARE_PATH=\"${datadir}/xrdp\" \
  -DXRDP_PID_PATH=\"${localstatedir}/run\" \
  -I$(top_srcdir)/common \
  -I$(srcdir) \
  -I$(builddir)

AM_CFLAGS = $(WAYLAND_CLIENT_CFLAGS) $(XKBCOMMON_CFLAGS)

if XRDP_WLUP_MUTTER
AM_CPPFLAGS += -DXRDP_WLUP_MUTTER
AM_CFLAGS += $(WLUP_MUTTER_CFLAGS)
MUTTER_SOURCES = backends/mutter/mutter.c backends/mutter/mutter.h
MUTTER_LIBS = $(WLUP_MUTTER_LIBS)
else
MUTTER_SOURCES =
MUTTER_LIBS =
endif

# The PipeWire frames, for both backends that cast to PipeWire
if XRDP_WLUP_MUTTER
PIPEWIRE_SOURCES = backends/pipewire/capture.c backends/pipewire/capture.h
else
if XRDP_WLUP_KWIN
PIPEWIRE_SOURCES = backends/pipewire/capture.c backends/pipewire/capture.h
else
PIPEWIRE_SOURCES =
endif
endif

# KWin's protocols are not copied here: they come from the
# plasma-wayland-protocols package (see configure)
KWIN_PROTOCOLS = \
  fake-input \
  kde-output-device-v2 \
  kde-output-management-v2 \
  zkde-screencast-unstable-v1

if XRDP_WLUP_KWIN
AM_CPPFLAGS += -DXRDP_WLUP_KWIN
AM_CFLAGS += $(WLUP_KWIN_CFLAGS)
KWIN_SOURCES = backends/kwin/kwin.c backends/kwin/kwin.h
KWIN_LIBS = $(WLUP_KWIN_LIBS)
KWIN_PROTOCOLS_BUILT = $(KWIN_PROTOCOLS)
else
KWIN_SOURCES =
KWIN_LIBS =
KWIN_PROTOCOLS_BUILT =
endif

# ext-foreign-toplevel-list-v1 is only needed because
# ext-image-capture-source-v1 refers to one of its interfaces
PROTOCOLS = \
  ext-data-control-v1 \
  ext-foreign-toplevel-list-v1 \
  ext-image-capture-source-v1 \
  ext-image-copy-capture-v1 \
  virtual-keyboard-unstable-v1 \
  wlr-output-management-unstable-v1 \
  wlr-virtual-pointer-unstable-v1

PROTOCOL_H_FILES = $(PROTOCOLS:=-client-protocol.h) \
  $(KWIN_PROTOCOLS_BUILT:=-client-protocol.h)
PROTOCOL_C_FILES = $(PROTOCOLS:=-protocol.c) $(KWIN_PROTOCOLS_BUILT:=-protocol.c)

# Generated only when wlup is built: "make dist" also visits this
# directory, where WAYLAND_SCANNER may not be set
if XRDP_WLUP
BUILT_SOURCES = $(PROTOCOL_H_FILES) $(PROTOCOL_C_FILES)
endif
CLEANFILES = $(PROTOCOL_H_FILES) $(PROTOCOL_C_FILES) \
  $(KWIN_PROTOCOLS:=-client-protocol.h) $(KWIN_PROTOCOLS:=-protocol.c)

EXTRA_DIST = $(PROTOCOLS:%=protocols/%.xml)

%-client-protocol.h: $(srcdir)/protocols/%.xml
	$(WAYLAND_SCANNER) client-header $< $@

%-protocol.c: $(srcdir)/protocols/%.xml
	$(WAYLAND_SCANNER) private-code $< $@

# make takes the first of these rules whose XML file exists
%-client-protocol.h: $(PLASMA_WAYLAND_PROTOCOLS)/%.xml
	$(WAYLAND_SCANNER) client-header $< $@

%-protocol.c: $(PLASMA_WAYLAND_PROTOCOLS)/%.xml
	$(WAYLAND_SCANNER) private-code $< $@

# wayland-scanner's code is C, and must stay C: in C++ (CC=g++, as xrdp's
# CI builds), a const object at file scope has internal linkage, so the
# interfaces would neither be exported nor link. -x c compiles it as C
# whatever CC is; wlup's own code reaches it through the client headers,
# which declare it extern "C"
noinst_LTLIBRARIES = libwlup_protocols.la
nodist_libwlup_protocols_la_SOURCES = $(PROTOCOL_C_FILES)
libwlup_protocols_la_CFLAGS = -x c $(AM_CFLAGS)

module_LTLIBRARIES = \
  libwlup.la

libwlup_la_SOURCES = \
  wlup.c \
  wlup.h \
  wlup_backend.h \
  wlup_clip.c \
  wlup_clip.h \
  wlup_gfx.c \
  wlup_gfx.h \
  backends/wlroots/wlroots.c \
  backends/wlroots/clip.c \
  backends/wlroots/clip.h \
  $(PIPEWIRE_SOURCES) \
  $(MUTTER_SOURCES) \
  $(KWIN_SOURCES)

EXTRA_libwlup_la_SOURCES = backends/mutter/mutter.c backends/mutter/mutter.h \
  backends/pipewire/capture.c backends/pipewire/capture.h \
  backends/kwin/kwin.c backends/kwin/kwin.h

nodist_libwlup_la_SOURCES = $(PROTOCOL_H_FILES)

libwlup_la_LIBADD = \
  libwlup_protocols.la \
  $(top_builddir)/common/libcommon.la \
  $(WAYLAND_CLIENT_LIBS) \
  $(XKBCOMMON_LIBS) \
  $(MUTTER_LIBS) \
  $(KWIN_LIBS)

libwlup_la_LDFLAGS = -avoid-version -module
