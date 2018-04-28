extern "C" {
#include <libavformat/avformat.h>
}
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <epoxy/gl.h>  // IWYU pragma: keep
#include <QApplication>
#include <QCoreApplication>
#include <QGL>
#include <QSize>
#include <QSurfaceFormat>
#include <string>

#ifdef HAVE_CEF
#include <cef_app.h>
#include <cef_browser.h>
#include <cef_client.h>
#include <cef_version.h>
#endif

#include "basic_stats.h"
#ifdef HAVE_CEF
#include "nageru_cef_app.h"
#endif
#include "context.h"
#include "flags.h"
#include "image_input.h"
#include "mainwindow.h"
#include "mixer.h"
#include "quicksync_encoder.h"

#ifdef HAVE_CEF
CefRefPtr<NageruCefApp> cef_app;
#endif

int main(int argc, char *argv[])
{
#ifdef HAVE_CEF
	// Let CEF have first priority on parsing the command line, because we might be
	// launched as a CEF sub-process.
	CefMainArgs main_args(argc, argv);
	cef_app = CefRefPtr<NageruCefApp>(new NageruCefApp());
	int err = CefExecuteProcess(main_args, cef_app.get(), nullptr);
	if (err >= 0) {
		return err;
	}

	// CEF wants to use GLib for its main loop, which interferes with Qt's use of it.
	// The alternative is trying to integrate CEF into Qt's main loop, but that requires
	// fairly extensive cross-thread communication and that parts of CEF runs on Qt's UI
	// thread.
	setenv("QT_NO_GLIB", "1", 0);
#endif

	parse_flags(PROGRAM_NAGERU, argc, argv);

	if (global_flags.va_display.empty()) {
		// The user didn't specify a VA-API display. See if the default works,
		// and if not, let's try to help the user by seeing if there's any
		// that would work automatically.
		global_flags.va_display = QuickSyncEncoder::get_usable_va_display();
	}

	if (global_flags.va_display.empty() ||
	    global_flags.va_display[0] != '/') {
		// We normally use EGL for zerocopy, but if we use VA against DRM
		// instead of against X11, we turn it off, and then don't need EGL.
		setenv("QT_XCB_GL_INTEGRATION", "xcb_egl", 0);
		using_egl = true;
	}
	setlinebuf(stdout);
	av_register_all();

	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts, true);

	QSurfaceFormat fmt;
	fmt.setDepthBufferSize(0);
	fmt.setStencilBufferSize(0);
	fmt.setProfile(QSurfaceFormat::CoreProfile);
	fmt.setMajorVersion(3);
	fmt.setMinorVersion(1);

	// Turn off vsync, since Qt generally gives us at most frame rate
	// (display frequency) / (number of QGLWidgets active).
	fmt.setSwapInterval(0);

	QSurfaceFormat::setDefaultFormat(fmt);

	QGLFormat::setDefaultFormat(QGLFormat::fromSurfaceFormat(fmt));

	QApplication app(argc, argv);
	global_share_widget = new QGLWidget();
	if (!global_share_widget->isValid()) {
		fprintf(stderr, "Failed to initialize OpenGL. Nageru needs at least OpenGL 3.1 to function properly.\n");
		exit(1);
	}

	MainWindow mainWindow;
	mainWindow.resize(QSize(1500, 850));
	mainWindow.show();

	app.installEventFilter(&mainWindow);  // For white balance color picking.

	// Even on an otherwise unloaded system, it would seem writing the recording
	// to disk (potentially terabytes of data as time goes by) causes Nageru
	// to be pushed out of RAM. If we have the right privileges, simply lock us
	// into memory for better realtime behavior.
	if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
		perror("mlockall()");
		fprintf(stderr, "Failed to lock Nageru into RAM. You probably want to\n");
		fprintf(stderr, "increase \"memlock\" for your user in limits.conf\n");
		fprintf(stderr, "for better realtime behavior.\n");
		uses_mlock = false;
	} else {
		uses_mlock = true;
	}

	int rc = app.exec();
	global_mixer->quit();
	mainWindow.mixer_shutting_down();
	delete global_mixer;
	ImageInput::shutdown_updaters();
	return rc;
}
