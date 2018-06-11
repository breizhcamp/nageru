CXX=g++
PKG_MODULES := Qt5Core Qt5Gui Qt5Widgets Qt5PrintSupport libjpeg
CXXFLAGS ?= -O2 -g -Wall  # Will be overridden by environment.
CXXFLAGS += -std=gnu++11 -fPIC $(shell pkg-config --cflags $(PKG_MODULES)) -pthread

LDLIBS=$(shell pkg-config --libs $(PKG_MODULES)) -pthread -lavformat -lavcodec -lavutil -lswscale

# Qt objects
OBJS_WITH_MOC = mainwindow.o jpeg_frame_view.o clip_list.o
OBJS += $(OBJS_WITH_MOC)
OBJS += $(OBJS_WITH_MOC:.o=.moc.o) 

OBJS += ffmpeg_raii.o main.o player.o

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

all: futatabi

mainwindow.o: ui_mainwindow.h

futatabi: $(OBJS) $(CEF_LIBS)
	$(CXX) -o $@ $^ $(LDFLAGS) $(LDLIBS)

DEPS=$(OBJS:.o=.d)
-include $(DEPS)

clean:
	$(RM) $(OBJS) $(DEPS) futatabi $(OBJS_WITH_MOC:.o=.moc.cpp) 
