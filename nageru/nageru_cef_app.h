#ifndef _NAGERU_CEF_APP_H
#define _NAGERU_CEF_APP_H 1

// NageruCefApp deals with global state around CEF, in particular the global
// CEF event loop. CEF is pretty picky about which threads everything runs on;
// in particular, the documentation says CefExecute, CefInitialize and
// CefRunMessageLoop must all be on the main thread (ie., the first thread
// created). However, Qt wants to run _its_ event loop on this thread, too,
// and integrating the two has proved problematic (see also the comment in
// main.cpp). It seems that as long as you don't have two GLib loops running,
// it's completely fine in practice to have a separate thread for the main loop
// (running CefInitialize, CefRunMessageLoop, and finally CefDestroy).
// Many other tasks (like most things related to interacting with browsers)
// have to be run from the message loop, but that's fine; CEF gives us tools
// to post tasks to it.

#include <stdio.h>

#include <cef_app.h>
#include <cef_browser.h>
#include <cef_client.h>
#include <cef_version.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <thread>
#include <vector>

// Takes in arbitrary lambdas and converts them to something CefPostTask() will accept.
class CEFTaskAdapter : public CefTask
{
public:
	CEFTaskAdapter(const std::function<void()>&& func)
		: func(std::move(func)) {}
	void Execute() override { func(); }

private:
	std::function<void()> func;

	IMPLEMENT_REFCOUNTING(CEFTaskAdapter);
};

// Runs and stops the CEF event loop, and also makes some startup tasks.
class NageruCefApp : public CefApp, public CefRenderProcessHandler, public CefBrowserProcessHandler {
public:
	NageruCefApp() {}

	// Starts up the CEF main loop if it does not already run, and blocks until
	// CEF is properly initialized. You can call initialize_ref() multiple times,
	// which will then increase the refcount.
	void initialize_cef();

	// If the refcount goes to zero, shut down the main loop and uninitialize CEF.
	void unref_cef();

	// Closes the given browser, and blocks until it is done closing.
	//
	// NOTE: We can't call unref_cef() from close_browser(), since
	// CefRefPtr<T> does not support move semantics, so it would have a
	// refcount of either zero or two going into close_browser (not one,
	// as it should). The latter means the caller would hold on to an extra
	// reference to the browser (which triggers an assert failure), and the
	// former would mean that the browser gets deleted before it's closed.
	void close_browser(CefRefPtr<CefBrowser> browser);

	CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override
	{
		return this;
	}

	CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override
	{
		return this;
	}

	void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line) override;

private:
	void cef_thread_func();

	std::thread cef_thread;
	std::mutex cef_mutex;
	int cef_thread_refcount = 0;  // Under <cef_mutex>.
	bool cef_initialized = false;  // Under <cef_mutex>.
	std::condition_variable cef_initialized_cond;

	IMPLEMENT_REFCOUNTING(NageruCefApp);
};

#endif  // !defined(_NAGERU_CEF_APP_H)
