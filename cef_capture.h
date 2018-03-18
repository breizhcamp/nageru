#ifndef _CEF_CAPTURE_H
#define _CEF_CAPTURE_H 1

// CEFCapture represents a single CEF virtual capture card (usually, there would only
// be one globally), similar to FFmpegCapture. It owns a CefBrowser, which calls
// OnPaint() back every time it has a frame. Note that it runs asynchronously;
// there's no way to get frame-perfect sync.

#include <assert.h>
#include <stdint.h>

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#undef CHECK
#include <cef_client.h>
#include <cef_base.h>
#include <cef_render_handler.h>

#include <bmusb/bmusb.h>

class CefBrowser;
class CefRect;
class CEFCapture;

// A helper class for CEFCapture to proxy information to CEF, without becoming
// CEF-refcounted itself.
class NageruCEFClient : public CefClient, public CefRenderHandler, public CefLoadHandler
{
public:
	NageruCEFClient(int width, int height, CEFCapture *parent)
		: width(width), height(height), parent(parent) {}

	CefRefPtr<CefRenderHandler> GetRenderHandler() override
	{
		return this;
	}

	CefRefPtr<CefLoadHandler> GetLoadHandler() override
	{
		return this;
	}

	// CefRenderHandler.

	void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList &dirtyRects, const void *buffer, int width, int height) override;

	bool GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect);

	// CefLoadHandler.

	void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;

private:
	int width, height;
	CEFCapture *parent;

	IMPLEMENT_REFCOUNTING(NageruCEFClient);
};

class CEFCapture : public bmusb::CaptureInterface
{
public:
	CEFCapture(const std::string &url, unsigned width, unsigned height);
	~CEFCapture();

	void set_card_index(int card_index)
	{
		this->card_index = card_index;
	}

	int get_card_index() const
	{
		return card_index;
	}

	void set_url(const std::string &url);
	void reload();
	void set_max_fps(int max_fps);
	void execute_javascript_async(const std::string &js);
	void resize(unsigned width, unsigned height);

	// Callbacks from NageruCEFClient.
	void OnPaint(const void *buffer, int width, int height);
	bool GetViewRect(CefRect &rect);
	void OnLoadEnd();

	// CaptureInterface.
	void set_video_frame_allocator(bmusb::FrameAllocator *allocator) override
	{
		video_frame_allocator = allocator;
		if (owned_video_frame_allocator.get() != allocator) {
			owned_video_frame_allocator.reset();
		}
	}

	bmusb::FrameAllocator *get_video_frame_allocator() override
	{
		return video_frame_allocator;
	}

	// Does not take ownership.
	void set_audio_frame_allocator(bmusb::FrameAllocator *allocator) override
	{
	}

	bmusb::FrameAllocator *get_audio_frame_allocator() override
	{
		return nullptr;
	}

	void set_frame_callback(bmusb::frame_callback_t callback) override
	{
		frame_callback = callback;
	}

	void set_dequeue_thread_callbacks(std::function<void()> init, std::function<void()> cleanup) override
	{
		dequeue_init_callback = init;
		dequeue_cleanup_callback = cleanup;
		has_dequeue_callbacks = true;
	}

	std::string get_description() const override
	{
		return description;
	}

	void configure_card() override;
	void start_bm_capture() override;
	void stop_dequeue_thread() override;
	bool get_disconnected() const override { return false; }

	std::set<bmusb::PixelFormat> get_available_pixel_formats() const override
	{
		return std::set<bmusb::PixelFormat>{ bmusb::PixelFormat_8BitBGRA };
	}

	void set_pixel_format(bmusb::PixelFormat pixel_format) override
	{
		assert(pixel_format == bmusb::PixelFormat_8BitBGRA);
	}

	bmusb::PixelFormat get_current_pixel_format() const
	{
		return bmusb::PixelFormat_8BitBGRA;
	}

	std::map<uint32_t, bmusb::VideoMode> get_available_video_modes() const override;
	void set_video_mode(uint32_t video_mode_id) override;
	uint32_t get_current_video_mode() const override { return 0; }

	std::map<uint32_t, std::string> get_available_video_inputs() const override;
	void set_video_input(uint32_t video_input_id) override;
	uint32_t get_current_video_input() const override { return 0; }

	std::map<uint32_t, std::string> get_available_audio_inputs() const override;
	void set_audio_input(uint32_t audio_input_id) override;
	uint32_t get_current_audio_input() const override { return 0; }

private:
	void post_to_cef_ui_thread(std::function<void()> &&func, int64_t delay_ms = 0);

	CefRefPtr<NageruCEFClient> cef_client;

	// Needs to be different from browser_mutex below, since GetViewRect
	// can be called unpredictably from when we are already holding
	// <browser_mutex>.
	std::mutex resolution_mutex;
	unsigned width, height;  // Under <resolution_mutex>.

	int card_index = -1;

	bool has_dequeue_callbacks = false;
	std::function<void()> dequeue_init_callback = nullptr;
	std::function<void()> dequeue_cleanup_callback = nullptr;

	bmusb::FrameAllocator *video_frame_allocator = nullptr;
	std::unique_ptr<bmusb::FrameAllocator> owned_video_frame_allocator;
	bmusb::frame_callback_t frame_callback = nullptr;

	std::string description, start_url;
	std::atomic<int> max_fps{60};

	// Needs to be recursive because the lambda in OnPaint could cause
	// OnPaint itself to be called.
	std::recursive_mutex browser_mutex;
	CefRefPtr<CefBrowser> browser;  // Under <browser_mutex>.

	// Tasks waiting for <browser> to get ready. Under <browser_mutex>.
	std::vector<std::function<void()>> deferred_tasks;

	// Whether the last set_url() (includes the implicit one in the constructor)
	// has loaded yet. Accessed from the CEF thread only.
	bool loaded = false;

	// JavaScript waiting for the first page (well, any page) to have loaded.
	// Accessed from the CEF thread only.
	std::vector<std::string> deferred_javascript;

	int timecode = 0;
};

#endif  // !defined(_CEF_CAPTURE_H)
