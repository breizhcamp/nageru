CXX=g++
PROTOC=protoc
PKG_MODULES := Qt5Core Qt5Gui Qt5Widgets Qt5OpenGLExtensions Qt5OpenGL Qt5PrintSupport libjpeg movit libmicrohttpd protobuf sqlite3
CXXFLAGS ?= -O2 -g -Wall  # Will be overridden by environment.
CXXFLAGS += -fPIC $(shell pkg-config --cflags $(PKG_MODULES)) -DMOVIT_SHADER_DIR=\"$(shell pkg-config --variable=shaderdir movit)\" -pthread

LDLIBS=$(shell pkg-config --libs $(PKG_MODULES)) -pthread -lavformat -lavcodec -lavutil -lswscale -lGL -lva -lva-drm -lva-x11 -lX11

# Qt objects
OBJS_WITH_MOC = mainwindow.o jpeg_frame_view.o clip_list.o
OBJS += $(OBJS_WITH_MOC)
OBJS += $(OBJS_WITH_MOC:.o=.moc.o) 

# Flow objects
OBJS += flow.o gpu_timers.o

OBJS += ffmpeg_raii.o main.o player.o httpd.o mux.o metacube2.o video_stream.o context.o chroma_subsampler.o
OBJS += vaapi_jpeg_decoder.o memcpy_interleaved.o db.o
OBJS += state.pb.o

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

all: futatabi flow vis eval

mainwindow.o: ui_mainwindow.h

clip_list.h: state.pb.h

db.h: state.pb.h

futatabi: $(OBJS) $(CEF_LIBS)
	$(CXX) -o $@ $^ $(LDFLAGS) $(LDLIBS)
flow: flow.o flow_main.o gpu_timers.o
	$(CXX) -o $@ $^ -lepoxy -lSDL2 -lSDL2_image
eval: eval.o util.o
	$(CXX) -o $@ $^
vis: vis.o util.o
	$(CXX) -o $@ $^

DEPS=$(OBJS:.o=.d)
-include $(DEPS)

clean:
	$(RM) $(OBJS) $(DEPS) flow_main.o gpu_timers.o futatabi $(OBJS_WITH_MOC:.o=.moc.cpp) *.pb.cc *.pb.h
