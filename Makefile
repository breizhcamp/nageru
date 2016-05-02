CXX=g++
PKG_MODULES = Qt5Core Qt5Gui Qt5Widgets Qt5OpenGLExtensions Qt5OpenGL libusb-1.0 movit lua52 libmicrohttpd epoxy x264
CXXFLAGS := -O2 -march=native -g -std=gnu++11 -Wall -Wno-deprecated-declarations -Werror -fPIC $(shell pkg-config --cflags $(PKG_MODULES)) -pthread -DMOVIT_SHADER_DIR=\"$(shell pkg-config --variable=shaderdir movit)\" -Idecklink/
LDFLAGS=$(shell pkg-config --libs $(PKG_MODULES)) -lEGL -lGL -pthread -lva -lva-drm -lva-x11 -lX11 -lavformat -lavcodec -lavutil -lswscale -lavresample -lzita-resampler -lasound -ldl

# Qt objects
OBJS=glwidget.o main.o mainwindow.o vumeter.o lrameter.o vu_common.o correlation_meter.o aboutdialog.o
OBJS += glwidget.moc.o mainwindow.moc.o vumeter.moc.o lrameter.moc.o correlation_meter.moc.o aboutdialog.moc.o

# Mixer objects
OBJS += mixer.o bmusb/bmusb.o pbo_frame_allocator.o context.o ref_counted_frame.o theme.o resampling_queue.o httpd.o ebu_r128_proc.o flags.o image_input.o stereocompressor.o filter.o alsa_output.o correlation_measurer.o

# Streaming and encoding objects
OBJS += quicksync_encoder.o x264_encoder.o x264_speed_control.o video_encoder.o metacube2.o mux.o audio_encoder.o

# DeckLink
OBJS += decklink_capture.o decklink/DeckLinkAPIDispatch.o

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
	$(CXX) $(LDFLAGS) -o $@ $^

mainwindow.o: mainwindow.cpp ui_mainwindow.h ui_display.h

aboutdialog.o: aboutdialog.cpp ui_aboutdialog.h

DEPS=$(OBJS:.o=.d)
-include $(DEPS)

clean:
	$(RM) $(OBJS) $(DEPS) nageru ui_mainwindow.h ui_display.h ui_about.h glwidget.moc.cpp mainwindow.moc.cpp window.moc.cpp chain-*.frag *.dot
