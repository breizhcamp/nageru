#ifndef _VA_DISPLAY_WITH_CLEANUP
#define _VA_DISPLAY_WITH_CLEANUP 1

#include <va/va.h>
#include <X11/Xlib.h>

#include <memory>

struct VADisplayWithCleanup {
	~VADisplayWithCleanup();

	VADisplay va_dpy;
	Display *x11_display = nullptr;
	bool can_use_zerocopy = true;
	int drm_fd = -1;
};
std::unique_ptr<VADisplayWithCleanup> va_open_display(const std::string &va_display);  // Can return nullptr on failure.

#endif  // !defined(_VA_DISPLAY_WITH_CLEANUP)
