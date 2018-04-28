#include <stdio.h>

#include <string>

#include <QGL>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurface>
#include <QSurfaceFormat>

QGLWidget *global_share_widget = nullptr;
bool using_egl = false;

using namespace std;

QSurface *create_surface(const QSurfaceFormat &format)
{
	QOffscreenSurface *surface = new QOffscreenSurface;
	surface->setFormat(format);
	surface->create();
	if (!surface->isValid()) {
		fprintf(stderr, "ERROR: surface not valid!\n");
		exit(1);
	}
	return surface;
}

QSurface *create_surface_with_same_format(const QSurface *surface)
{
	return create_surface(surface->format());
}

QOpenGLContext *create_context(const QSurface *surface)
{
	QOpenGLContext *context = new QOpenGLContext;
	context->setShareContext(global_share_widget->context()->contextHandle());
	context->setFormat(surface->format());
	context->create();
	return context;
}

bool make_current(QOpenGLContext *context, QSurface *surface)
{
	return context->makeCurrent(surface);
}

void delete_context(QOpenGLContext *context)
{
	delete context;
}
