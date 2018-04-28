CXX=g++
PROTOC=protoc
INSTALL=install
EMBEDDED_BMUSB=no
PKG_MODULES := Qt5Core Qt5Gui Qt5Widgets Qt5OpenGLExtensions Qt5OpenGL Qt5PrintSupport libusb-1.0 movit luajit libmicrohttpd epoxy x264 protobuf
CXXFLAGS ?= -O2 -g -Wall  # Will be overridden by environment.
CXXFLAGS += -std=gnu++11 -fPIC $(shell pkg-config --cflags $(PKG_MODULES)) -pthread -DMOVIT_SHADER_DIR=\"$(shell pkg-config --variable=shaderdir movit)\" -Idecklink/

# Override CEF_DIR on the command line to build with CEF.
# E.g.: make CEF_DIR=/home/sesse/cef_binary_3.3282.1734.g8f26fe0_linux64
CEF_DIR=

# Release or Debug, depending on what CEF version you want to link to.
# Optionally, the special value CEF_BUILD_TYPE=system assumes you can build and link
# to CEF without any special flags, get the remaining libraries (libEGL.so,
# natives_blob.bin, etc.) from $(CEF_LIB), get resources from $(CEF_RESOURCE_DIR),
# and symlink resources instead of copying them in.
CEF_BUILD_TYPE=Release
ifneq ($(CEF_DIR),)
  CPPFLAGS += -DHAVE_CEF=1
  ifeq ($(CEF_BUILD_TYPE),system)
    EXTRA_CEF_LDFLAGS =
    CEF_LIB_DIR = $(CEF_DIR)
    CEF_RESOURCE_DIR = /usr/share/cef/Resources
    CEF_CP = ln -s
  else
    CEF_LIBS = $(CEF_DIR)/libcef_dll_wrapper/libcef_dll_wrapper.a
    CPPFLAGS += -I$(CEF_DIR) -I$(CEF_DIR)/include
    EXTRA_CEF_LDFLAGS = -L$(CEF_DIR)/$(CEF_BUILD_TYPE) -Wl,-rpath,\$$ORIGIN
    CEF_LIB_DIR = $(CEF_DIR)/$(CEF_BUILD_TYPE)
    CEF_RESOURCE_DIR = $(CEF_DIR)/Resources
    CEF_CP = cp -a
  endif
endif

ifeq ($(EMBEDDED_BMUSB),yes)
  CPPFLAGS += -Ibmusb/
else
  PKG_MODULES += bmusb
endif
LDLIBS=$(shell pkg-config --libs $(PKG_MODULES)) -pthread -lva -lva-drm -lva-x11 -lX11 -lavformat -lavcodec -lavutil -lswscale -lavresample -lzita-resampler -lasound -ldl -lqcustomplot
ifneq ($(CEF_DIR),)
  EXTRA_CEF_LDLIBS = -lcef
  ifeq ($(CEF_BUILD_TYPE),system)
    # Don't build this ourselves; just link to the system version.
    EXTRA_CEF_LDLIBS += -lcef_dll_wrapper
  endif
endif

# Qt objects
OBJS_WITH_MOC = glwidget.o mainwindow.o vumeter.o lrameter.o compression_reduction_meter.o correlation_meter.o aboutdialog.o analyzer.o input_mapping_dialog.o midi_mapping_dialog.o nonlinear_fader.o
OBJS += $(OBJS_WITH_MOC)
OBJS += $(OBJS_WITH_MOC:.o=.moc.o) ellipsis_label.moc.o clickable_label.moc.o
OBJS += context_menus.o vu_common.o piecewise_interpolator.o main.o
OBJS += midi_mapper.o midi_mapping.pb.o

# Mixer objects
AUDIO_MIXER_OBJS = audio_mixer.o alsa_input.o alsa_pool.o ebu_r128_proc.o stereocompressor.o resampling_queue.o flags.o correlation_measurer.o filter.o input_mapping.o state.pb.o
OBJS += chroma_subsampler.o v210_converter.o mixer.o basic_stats.o metrics.o pbo_frame_allocator.o context.o ref_counted_frame.o theme.o httpd.o flags.o image_input.o alsa_output.o disk_space_estimator.o print_latency.o timecode_renderer.o tweaked_inputs.o $(AUDIO_MIXER_OBJS)

# Streaming and encoding objects
OBJS += quicksync_encoder.o x264_encoder.o x264_dynamic.o x264_speed_control.o video_encoder.o metacube2.o mux.o audio_encoder.o ffmpeg_raii.o ffmpeg_util.o json.pb.o

# DeckLink
OBJS += decklink_capture.o decklink_util.o decklink_output.o decklink/DeckLinkAPIDispatch.o

KAERU_OBJS = kaeru.o x264_encoder.o mux.o basic_stats.o metrics.o flags.o audio_encoder.o x264_speed_control.o print_latency.o x264_dynamic.o ffmpeg_raii.o ref_counted_frame.o ffmpeg_capture.o ffmpeg_util.o httpd.o json.pb.o metacube2.o

# bmusb
ifeq ($(EMBEDDED_BMUSB),yes)
  OBJS += bmusb/bmusb.o bmusb/fake_capture.o
  KAERU_OBJS += bmusb/bmusb.o
endif

# FFmpeg input
OBJS += ffmpeg_capture.o

ifneq ($(CEF_DIR),)
  # CEF input
  OBJS += nageru_cef_app.o cef_capture.o
endif

# Benchmark program.
BM_OBJS = benchmark_audio_mixer.o $(AUDIO_MIXER_OBJS) flags.o metrics.o

%.o: %.cpp
	$(CXX) -MMD -MP $(CPPFLAGS) $(CXXFLAGS) -o $@ -c $<
%.o: %.cc
	$(CXX) -MMD -MP $(CPPFLAGS) $(CXXFLAGS) -o $@ -c $<
%.pb.cc %.pb.h : %.proto
	$(PROTOC) --cpp_out=. $<

%.h: %.ui
	uic $< -o $@

%.moc.cpp: %.h
	moc $< -o $@

ifneq ($(CEF_DIR),)
CEF_RESOURCES = natives_blob.bin snapshot_blob.bin v8_context_snapshot.bin
CEF_RESOURCES += cef.pak cef_100_percent.pak cef_200_percent.pak cef_extensions.pak devtools_resources.pak
CEF_RESOURCES += libEGL.so libGLESv2.so swiftshader/libEGL.so swiftshader/libGLESv2.so
CEF_RESOURCES += locales/en-US.pak locales/en-US.pak.info
ifneq ($(CEF_NO_ICUDTL),yes)
CEF_RESOURCES += icudtl.dat
endif
ifneq ($(CEF_BUILD_TYPE),system)
CEF_RESOURCES += libcef.so
endif
endif

all: nageru kaeru benchmark_audio_mixer $(CEF_RESOURCES)

nageru: $(OBJS) $(CEF_LIBS)
	$(CXX) -o $@ $^ $(LDFLAGS) $(EXTRA_CEF_LDFLAGS) $(LDLIBS) $(EXTRA_CEF_LDLIBS) $(CEF_LIBS)
kaeru: $(KAERU_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS) $(LDLIBS)
benchmark_audio_mixer: $(BM_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS) $(LDLIBS)

ifneq ($(CEF_DIR),)
# A lot of these unfortunately have to be in the same directory as the binary;
# some can be given paths, but not all.
libcef.so: $(CEF_LIB_DIR)/libcef.so
	$(CEF_CP) $< $@
libEGL.so: $(CEF_LIB_DIR)/libEGL.so
	$(CEF_CP) $< $@
libGLESv2.so: $(CEF_LIB_DIR)/libGLESv2.so
	$(CEF_CP) $< $@
swiftshader/:
	mkdir swiftshader/
swiftshader/libEGL.so: | swiftshader/ $(CEF_LIB_DIR)/swiftshader/libEGL.so
	$(CEF_CP) $(CEF_LIB_DIR)/swiftshader/libEGL.so $@
swiftshader/libGLESv2.so: | swiftshader/ $(CEF_LIB_DIR)/swiftshader/libGLESv2.so
	$(CEF_CP) $(CEF_LIB_DIR)/swiftshader/libGLESv2.so $@
locales/:
	mkdir locales/
locales/en-US.pak: | locales/ $(CEF_RESOURCE_DIR)/locales/en-US.pak
	$(CEF_CP) $(CEF_RESOURCE_DIR)/locales/en-US.pak $@
locales/en-US.pak.info: | locales/ $(CEF_RESOURCE_DIR)/locales/en-US.pak.info
	$(CEF_CP) $(CEF_RESOURCE_DIR)/locales/en-US.pak.info $@
icudtl.dat: $(CEF_RESOURCE_DIR)/icudtl.dat
	$(CEF_CP) $< $@
%.bin: $(CEF_LIB_DIR)/%.bin
	$(CEF_CP) $< $@
%.pak: $(CEF_RESOURCE_DIR)/%.pak
	$(CEF_CP) $< $@
endif

# Extra dependencies that need to be generated.
aboutdialog.o: ui_aboutdialog.h
analyzer.o: ui_analyzer.h
alsa_pool.o: state.pb.h
audio_mixer.o: state.pb.h
input_mapping.o: state.pb.h
input_mapping_dialog.o: ui_input_mapping.h
mainwindow.o: ui_mainwindow.h ui_display.h ui_audio_miniview.h ui_audio_expanded_view.h ui_midi_mapping.h
mainwindow.o: midi_mapping.pb.h
midi_mapper.o: midi_mapping.pb.h
midi_mapping_dialog.o: ui_midi_mapping.h midi_mapping.pb.h
mixer.o: json.pb.h

# CEF wrapper library; typically not built as part of the binary distribution.
$(CEF_DIR)/libcef_dll_wrapper/libcef_dll_wrapper.a: $(CEF_DIR)/Makefile
	cd $(CEF_DIR) && $(MAKE) libcef_dll_wrapper

$(CEF_DIR)/Makefile:
	cd $(CEF_DIR) && cmake .

DEPS=$(OBJS:.o=.d) $(BM_OBJS:.o=.d) $(KAERU_OBJS:.o=.d)
-include $(DEPS)

clean:
	$(RM) $(OBJS) $(BM_OBJS) $(KAERU_OBJS) $(DEPS) nageru kaeru benchmark_audio_mixer ui_aboutdialog.h ui_analyzer.h ui_mainwindow.h ui_display.h ui_about.h ui_audio_miniview.h ui_audio_expanded_view.h ui_input_mapping.h ui_midi_mapping.h chain-*.frag *.dot *.pb.cc *.pb.h $(OBJS_WITH_MOC:.o=.moc.cpp) ellipsis_label.moc.cpp clickable_label.moc.cpp $(CEF_LIBS) $(CEF_RESOURCES)

PREFIX=/usr/local
install: install-cef
	$(INSTALL) -m 755 -o root -g root -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/nageru $(DESTDIR)$(PREFIX)/lib/nageru
	$(INSTALL) -m 755 -o root -g root nageru $(DESTDIR)$(PREFIX)/lib/nageru/nageru
	ln -s $(PREFIX)/lib/nageru/nageru $(DESTDIR)$(PREFIX)/bin/nageru
	$(INSTALL) -m 755 -o root -g root kaeru $(DESTDIR)$(PREFIX)/bin/kaeru
	$(INSTALL) -m 644 -o root -g root theme.lua $(DESTDIR)$(PREFIX)/share/nageru/theme.lua
	$(INSTALL) -m 644 -o root -g root simple.lua $(DESTDIR)$(PREFIX)/share/nageru/simple.lua
	$(INSTALL) -m 644 -o root -g root bg.jpeg $(DESTDIR)$(PREFIX)/share/nageru/bg.jpeg
	$(INSTALL) -m 644 -o root -g root akai_midimix.midimapping $(DESTDIR)$(PREFIX)/share/nageru/akai_midimix.midimapping

ifneq ($(CEF_DIR),)
  ifeq ($(CEF_BUILD_TYPE),system)
install-cef:
	for FILE in $(CEF_RESOURCES); do \
		mkdir -p `dirname $(DESTDIR)$(PREFIX)/lib/nageru/$$FILE`; \
		cp -a $$FILE $(DESTDIR)$(PREFIX)/lib/nageru/$$FILE; \
	done

  else
install-cef:
	for FILE in $(CEF_LIBS) $(CEF_RESOURCES); do \
		$(INSTALL) -D -m 644 -o root -g root $$FILE $(DESTDIR)$(PREFIX)/lib/nageru/$$FILE; \
	done

  endif
else
install-cef:
endif
