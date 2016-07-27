CXX=g++
INSTALL=install
EMBEDDED_BMUSB=no
PKG_MODULES := Qt5Core Qt5Gui Qt5Widgets Qt5OpenGLExtensions Qt5OpenGL libusb-1.0 movit lua52 libmicrohttpd epoxy x264
CXXFLAGS := -O2 -g -std=gnu++11 -Wall -Wno-deprecated-declarations -fPIC $(shell pkg-config --cflags $(PKG_MODULES)) -pthread -DMOVIT_SHADER_DIR=\"$(shell pkg-config --variable=shaderdir movit)\" -Idecklink/
LDFLAGS=$(shell pkg-config --libs $(PKG_MODULES)) -lEGL -lGL -pthread -lva -lva-drm -lva-x11 -lX11 -lavformat -lavcodec -lavutil -lswscale -lavresample -lzita-resampler -lasound -ldl

ifeq ($(EMBEDDED_BMUSB),yes)
  CXXFLAGS += -Ibmusb/
else
  PKG_MODULES += bmusb
endif

# Qt objects
OBJS=glwidget.o main.o mainwindow.o vumeter.o lrameter.o vu_common.o correlation_meter.o aboutdialog.o
OBJS += glwidget.moc.o mainwindow.moc.o vumeter.moc.o lrameter.moc.o correlation_meter.moc.o aboutdialog.moc.o

# Mixer objects
OBJS += mixer.o pbo_frame_allocator.o context.o ref_counted_frame.o theme.o resampling_queue.o httpd.o ebu_r128_proc.o flags.o image_input.o stereocompressor.o filter.o alsa_output.o correlation_measurer.o

# Streaming and encoding objects
OBJS += quicksync_encoder.o x264_encoder.o x264_speed_control.o video_encoder.o metacube2.o mux.o audio_encoder.o

# DeckLink
OBJS += decklink_capture.o decklink/DeckLinkAPIDispatch.o

# bmusb
ifeq ($(EMBEDDED_BMUSB),yes)
  OBJS += bmusb/bmusb.o bmusb/fake_capture.o
endif

%.o: %.cpp
	$(CXX) -MMD -MP $(CPPFLAGS) $(CXXFLAGS) -o $@ -c $<
%.o: %.cc
	$(CXX) -MMD -MP $(CPPFLAGS) $(CXXFLAGS) -o $@ -c $<

%.h: %.ui
	uic $< -o $@

%.moc.cpp: %.h
	moc $< -o $@

all: nageru

nageru: $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

mainwindow.o: mainwindow.cpp ui_mainwindow.h ui_display.h

aboutdialog.o: aboutdialog.cpp ui_aboutdialog.h

DEPS=$(OBJS:.o=.d)
-include $(DEPS)

clean:
	$(RM) $(OBJS) $(DEPS) nageru ui_aboutdialog.h ui_mainwindow.h ui_display.h ui_about.h aboutdialog.moc.cpp correlation_meter.moc.cpp lrameter.moc.cpp vumeter.moc.cpp glwidget.moc.cpp mainwindow.moc.cpp window.moc.cpp chain-*.frag *.dot

PREFIX=/usr/local
install:
	$(INSTALL) -m 755 -o root -g root -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/nageru
	$(INSTALL) -m 755 -o root -g root nageru $(DESTDIR)$(PREFIX)/bin/nageru
	$(INSTALL) -m 644 -o root -g root theme.lua $(DESTDIR)$(PREFIX)/share/nageru/theme.lua
	$(INSTALL) -m 644 -o root -g root simple.lua $(DESTDIR)$(PREFIX)/share/nageru/simple.lua
	$(INSTALL) -m 644 -o root -g root bg.jpeg $(DESTDIR)$(PREFIX)/share/nageru/bg.jpeg
