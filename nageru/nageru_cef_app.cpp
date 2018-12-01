#include <cef_app.h>
#include <cef_browser.h>
#include <cef_client.h>
#include <cef_version.h>
#include <QTimer>
#include <QWidget>

#include "nageru_cef_app.h"

using namespace std;

void NageruCefApp::OnBeforeCommandLineProcessing(
	const CefString& process_type,
	CefRefPtr<CefCommandLine> command_line)
{
	command_line->AppendSwitch("disable-gpu");
	command_line->AppendSwitch("disable-gpu-compositing");
	command_line->AppendSwitch("enable-begin-frame-scheduling");
}

void NageruCefApp::initialize_cef()
{
	unique_lock<mutex> lock(cef_mutex);
	if (cef_thread_refcount++ == 0) {
		cef_thread = thread(&NageruCefApp::cef_thread_func, this);
	}
	cef_initialized_cond.wait(lock, [this]{ return cef_initialized; });
}

void NageruCefApp::close_browser(CefRefPtr<CefBrowser> browser)
{
	unique_lock<mutex> lock(cef_mutex);
	browser->GetHost()->CloseBrowser(/*force_close=*/true);
}

void NageruCefApp::unref_cef()
{
	unique_lock<mutex> lock(cef_mutex);
	if (--cef_thread_refcount == 0) {
		CefPostTask(TID_UI, new CEFTaskAdapter(&CefQuitMessageLoop));
		lock.unlock();
		cef_thread.join();
	}
}

void NageruCefApp::cef_thread_func()
{
	CefMainArgs main_args;
	CefSettings settings;
	//settings.log_severity = LOGSEVERITY_VERBOSE;
	settings.windowless_rendering_enabled = true;
	settings.no_sandbox = true;
	settings.command_line_args_disabled = false;
	CefInitialize(main_args, settings, this, nullptr);

	{
		lock_guard<mutex> lock(cef_mutex);
		cef_initialized = true;
	}
	cef_initialized_cond.notify_all();

	CefRunMessageLoop();

	CefShutdown();
}

