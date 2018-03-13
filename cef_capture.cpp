#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <chrono>
#include <memory>
#include <string>

#include "cef_capture.h"
#include "nageru_cef_app.h"

#undef CHECK
#include <cef_app.h>
#include <cef_browser.h>
#include <cef_client.h>

#include "bmusb/bmusb.h"

using namespace std;
using namespace std::chrono;
using namespace bmusb;

extern CefRefPtr<NageruCefApp> cef_app;

CEFCapture::CEFCapture(const string &url, unsigned width, unsigned height)
	: cef_client(new NageruCEFClient(width, height, this)),
	  width(width),
	  height(height),
	  start_url(url)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "CEF card %d", card_index + 1);
	description = buf;
}

CEFCapture::~CEFCapture()
{
	if (has_dequeue_callbacks) {
		dequeue_cleanup_callback();
	}
}

void CEFCapture::post_to_cef_ui_thread(std::function<void()> &&func)
{
	lock_guard<recursive_mutex> lock(browser_mutex);
	if (browser != nullptr) {
		CefPostTask(TID_UI, new CEFTaskAdapter(std::move(func)));
	} else {
		deferred_tasks.push_back(std::move(func));
	}
}

void CEFCapture::set_url(const string &url)
{
	post_to_cef_ui_thread([this, url] {
		loaded = false;
		browser->GetMainFrame()->LoadURL(url);
	});
}

void CEFCapture::reload()
{
	post_to_cef_ui_thread([this] {
		loaded = false;
		browser->Reload();
	});
}

void CEFCapture::set_max_fps(int max_fps)
{
	post_to_cef_ui_thread([this, max_fps] {
		browser->GetHost()->SetWindowlessFrameRate(max_fps);
		this->max_fps = max_fps;
	});
}

void CEFCapture::execute_javascript_async(const string &js)
{
	post_to_cef_ui_thread([this, js] {
		if (loaded) {
			CefString script_url("<theme eval>");
			int start_line = 1;
			browser->GetMainFrame()->ExecuteJavaScript(js, script_url, start_line);
		} else {
			deferred_javascript.push_back(js);
		}
	});
}

void CEFCapture::resize(unsigned width, unsigned height)
{
	lock_guard<mutex> lock(resolution_mutex);
	this->width = width;
	this->height = height;
}

void CEFCapture::OnPaint(const void *buffer, int width, int height)
{
	steady_clock::time_point timestamp = steady_clock::now();

	VideoFormat video_format;
	video_format.width = width;
	video_format.height = height;
	video_format.stride = width * 4;
	video_format.frame_rate_nom = max_fps;
	video_format.frame_rate_den = 1;
	video_format.has_signal = true;
	video_format.is_connected = true;

	FrameAllocator::Frame video_frame = video_frame_allocator->alloc_frame();
	if (video_frame.data == nullptr) {
		// We lost a frame, so we need to invalidate the entire thing.
		// (CEF only sends OnPaint when there are actual changes,
		// so we need to do this explicitly, or we could be stuck on an
		// old frame forever if the image doesn't change.)
		post_to_cef_ui_thread([this] {
			lock_guard<recursive_mutex> lock(browser_mutex);
			if (browser != nullptr) {  // Could happen if we are shutting down.
				browser->GetHost()->Invalidate(PET_VIEW);
			}
		});
		++timecode;
	} else {
		assert(video_frame.size >= unsigned(width * height * 4));
		assert(!video_frame.interleaved);
		memcpy(video_frame.data, buffer, width * height * 4);
		video_frame.len = video_format.stride * height;
		video_frame.received_timestamp = timestamp;
		frame_callback(timecode++,
			video_frame, 0, video_format,
			FrameAllocator::Frame(), 0, AudioFormat());
	}
}

void CEFCapture::OnLoadEnd()
{
	post_to_cef_ui_thread([this] {
		loaded = true;
		for (const string &js : deferred_javascript) {
			CefString script_url("<theme eval>");
			int start_line = 1;
			browser->GetMainFrame()->ExecuteJavaScript(js, script_url, start_line);
		}
		deferred_javascript.clear();
	});
}

#define FRAME_SIZE (8 << 20)  // 8 MB.

void CEFCapture::configure_card()
{
	if (video_frame_allocator == nullptr) {
		owned_video_frame_allocator.reset(new MallocFrameAllocator(FRAME_SIZE, NUM_QUEUED_VIDEO_FRAMES));
		set_video_frame_allocator(owned_video_frame_allocator.get());
	}
}

void CEFCapture::start_bm_capture()
{
	cef_app->initialize_cef();

	CefPostTask(TID_UI, new CEFTaskAdapter([this]{
		lock_guard<recursive_mutex> lock(browser_mutex);

		CefBrowserSettings browser_settings;
		browser_settings.web_security = cef_state_t::STATE_DISABLED;
		browser_settings.webgl = cef_state_t::STATE_ENABLED;
		browser_settings.windowless_frame_rate = max_fps;

		CefWindowInfo window_info;
		window_info.SetAsWindowless(0);
		browser = CefBrowserHost::CreateBrowserSync(window_info, cef_client, start_url, browser_settings, nullptr);
		for (function<void()> &task : deferred_tasks) {
			task();
		}
		deferred_tasks.clear();
	}));
}

void CEFCapture::stop_dequeue_thread()
{
	{
		lock_guard<recursive_mutex> lock(browser_mutex);
		cef_app->close_browser(browser);
		browser = nullptr;  // Or unref_cef() will be sad.
	}
	cef_app->unref_cef();
}

std::map<uint32_t, VideoMode> CEFCapture::get_available_video_modes() const
{
	VideoMode mode;

	char buf[256];
	snprintf(buf, sizeof(buf), "%ux%u", width, height);
	mode.name = buf;

	mode.autodetect = false;
	mode.width = width;
	mode.height = height;
	mode.frame_rate_num = max_fps;
	mode.frame_rate_den = 1;
	mode.interlaced = false;

	return {{ 0, mode }};
}

std::map<uint32_t, std::string> CEFCapture::get_available_video_inputs() const
{
	return {{ 0, "HTML video input" }};
}

std::map<uint32_t, std::string> CEFCapture::get_available_audio_inputs() const
{
	return {{ 0, "Fake HTML audio input (silence)" }};
}

void CEFCapture::set_video_mode(uint32_t video_mode_id)
{
	assert(video_mode_id == 0);
}

void CEFCapture::set_video_input(uint32_t video_input_id)
{
	assert(video_input_id == 0);
}

void CEFCapture::set_audio_input(uint32_t audio_input_id)
{
	assert(audio_input_id == 0);
}

void NageruCEFClient::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList &dirtyRects, const void *buffer, int width, int height)
{
	parent->OnPaint(buffer, width, height);
}

bool NageruCEFClient::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect)
{
	return parent->GetViewRect(rect);
}

bool CEFCapture::GetViewRect(CefRect &rect)
{
	lock_guard<mutex> lock(resolution_mutex);
	rect = CefRect(0, 0, width, height);
	return true;
}

void NageruCEFClient::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode)
{
	parent->OnLoadEnd();
}
