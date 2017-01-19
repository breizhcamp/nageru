#include <movit/effect_util.h>
#include <movit/util.h>
#include <movit/resource_pool.h>  // Must be above the Xlib includes.

#include <epoxy/egl.h>

#include "chroma_subsampler.h"
#include "decklink_output.h"
#include "decklink_util.h"
#include "flags.h"
#include "print_latency.h"
#include "resource_pool.h"
#include "timebase.h"

using namespace movit;
using namespace std;
using namespace std::chrono;

DeckLinkOutput::DeckLinkOutput(ResourcePool *resource_pool, QSurface *surface, unsigned width, unsigned height, unsigned card_index)
	: resource_pool(resource_pool), surface(surface), width(width), height(height), card_index(card_index)
{
	chroma_subsampler.reset(new ChromaSubsampler(resource_pool));
}

void DeckLinkOutput::set_device(IDeckLink *decklink)
{
	if (decklink->QueryInterface(IID_IDeckLinkOutput, (void**)&output) != S_OK) {
		fprintf(stderr, "Card %u has no outputs\n", card_index);
		exit(1);
	}

	IDeckLinkDisplayModeIterator *mode_it;
	if (output->GetDisplayModeIterator(&mode_it) != S_OK) {
		fprintf(stderr, "Failed to enumerate output display modes for card %u\n", card_index);
		exit(1);
	}

	video_modes.clear();

	for (const auto &it : summarize_video_modes(mode_it, card_index)) {
		if (it.second.width != width || it.second.height != height) {
			continue;
		}

		// We could support interlaced modes, but let's stay out of it for now,
		// since we don't have interlaced stream output.
		if (it.second.interlaced) {
			continue;
		}

		video_modes.insert(it);
	}

	mode_it->Release();

	// HDMI or SDI generally mean “both HDMI and SDI at the same time” on DeckLink cards
	// that support both; pick_default_video_connection() will generally pick one of those
	// if they exist. We're not very likely to need analog outputs, so we don't need a way
	// to change beyond that.
	video_connection = pick_default_video_connection(decklink, BMDDeckLinkVideoOutputConnections, card_index);
}

void DeckLinkOutput::start_output(uint32_t mode, int64_t base_pts)
{
	assert(output);

	should_quit = false;
	playback_started = false;
	this->base_pts = base_pts;

	IDeckLinkConfiguration *config = nullptr;
	if (output->QueryInterface(IID_IDeckLinkConfiguration, (void**)&config) != S_OK) {
		fprintf(stderr, "Failed to get configuration interface for output card\n");
		exit(1);
	}
	if (config->SetFlag(bmdDeckLinkConfigLowLatencyVideoOutput, true) != S_OK) {
		fprintf(stderr, "Failed to set low latency output\n");
		exit(1);
	}
	if (config->SetInt(bmdDeckLinkConfigVideoOutputConnection, video_connection) != S_OK) {
		fprintf(stderr, "Failed to set video output connection for card %u\n", card_index);
		exit(1);
	}
	if (config->SetFlag(bmdDeckLinkConfigUse1080pNotPsF, true) != S_OK) {
		fprintf(stderr, "Failed to set PsF flag for card\n");
		exit(1);
	}

	BMDDisplayModeSupport support;
	IDeckLinkDisplayMode *display_mode;
	if (output->DoesSupportVideoMode(mode, bmdFormat8BitYUV, bmdVideoOutputFlagDefault,
	                                 &support, &display_mode) != S_OK) {
		fprintf(stderr, "Couldn't ask for format support\n");
		exit(1);
	}

	if (support == bmdDisplayModeNotSupported) {
		fprintf(stderr, "Requested display mode not supported\n");
		exit(1);
	}

	BMDDisplayModeFlags flags = display_mode->GetFlags();
	if ((flags & bmdDisplayModeColorspaceRec601) && global_flags.ycbcr_rec709_coefficients) {
		fprintf(stderr, "WARNING: Chosen output mode expects Rec. 601 Y'CbCr coefficients.\n");
		fprintf(stderr, "         Consider --output-ycbcr-coefficients=rec601 (or =auto).\n");
	} else if ((flags & bmdDisplayModeColorspaceRec709) && !global_flags.ycbcr_rec709_coefficients) {
		fprintf(stderr, "WARNING: Chosen output mode expects Rec. 709 Y'CbCr coefficients.\n");
		fprintf(stderr, "         Consider --output-ycbcr-coefficients=rec709 (or =auto).\n");
	}

	BMDTimeValue time_value;
	BMDTimeScale time_scale;
	if (display_mode->GetFrameRate(&time_value, &time_scale) != S_OK) {
		fprintf(stderr, "Couldn't get frame rate\n");
		exit(1);
	}

	frame_duration = time_value * TIMEBASE / time_scale;

	display_mode->Release();

	HRESULT result = output->EnableVideoOutput(mode, bmdVideoOutputFlagDefault);
	if (result != S_OK) {
		fprintf(stderr, "Couldn't enable output with error 0x%x\n", result);
		exit(1);
	}
	if (output->SetScheduledFrameCompletionCallback(this) != S_OK) {
		fprintf(stderr, "Couldn't set callback\n");
		exit(1);
	}
	assert(OUTPUT_FREQUENCY == 48000);
	if (output->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType32bitInteger, 2, bmdAudioOutputStreamTimestamped) != S_OK) {
		fprintf(stderr, "Couldn't enable audio output\n");
		exit(1);
	}
	if (output->BeginAudioPreroll() != S_OK) {
		fprintf(stderr, "Couldn't begin audio preroll\n");
		exit(1);
	}

	present_thread = thread([this]{
		QOpenGLContext *context = create_context(this->surface);
		eglBindAPI(EGL_OPENGL_API);
		if (!make_current(context, this->surface)) {
			printf("display=%p surface=%p context=%p curr=%p err=%d\n", eglGetCurrentDisplay(), this->surface, context, eglGetCurrentContext(),
				eglGetError());
			exit(1);
		}
		present_thread_func();
		delete_context(context);
	});
}

void DeckLinkOutput::end_output()
{
	should_quit = true;
	frame_queues_changed.notify_all();
	present_thread.join();

	output->StopScheduledPlayback(0, nullptr, 0);
	output->DisableVideoOutput();
	output->DisableAudioOutput();

	// Wait until all frames are accounted for, and free them.
	{
		unique_lock<mutex> lock(frame_queue_mutex);
		while (!(frame_freelist.empty() && num_frames_in_flight == 0)) {
			frame_queues_changed.wait(lock, [this]{ return !frame_freelist.empty(); });
			frame_freelist.pop();
		}
	}
}

void DeckLinkOutput::send_frame(GLuint y_tex, GLuint cbcr_tex, const vector<RefCountedFrame> &input_frames, int64_t pts, int64_t duration)
{
	unique_ptr<Frame> frame = move(get_frame());
	chroma_subsampler->create_uyvy(y_tex, cbcr_tex, width, height, frame->uyvy_tex);

	// Download the UYVY texture to the PBO.
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	check_error();

	glBindBuffer(GL_PIXEL_PACK_BUFFER, frame->pbo);
	check_error();

	glBindTexture(GL_TEXTURE_2D, frame->uyvy_tex);
	check_error();
	glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, BUFFER_OFFSET(0));
	check_error();

	glBindTexture(GL_TEXTURE_2D, 0);
	check_error();
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	check_error();

	glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
	check_error();

	frame->fence = RefCountedGLsync(GL_SYNC_GPU_COMMANDS_COMPLETE, /*flags=*/0);
	check_error();
	glFlush();  // Make the DeckLink thread see the fence as soon as possible.
	check_error();

	frame->input_frames = input_frames;
	frame->received_ts = find_received_timestamp(input_frames);
	frame->pts = pts;
	frame->duration = duration;

	{
		unique_lock<mutex> lock(frame_queue_mutex);
		pending_video_frames.push(move(frame));
	}
	frame_queues_changed.notify_all();
}

void DeckLinkOutput::send_audio(int64_t pts, const std::vector<float> &samples)
{
	unique_ptr<int32_t[]> int_samples(new int32_t[samples.size()]);
	for (size_t i = 0; i < samples.size(); ++i) {
		int_samples[i] = lrintf(samples[i] * 2147483648.0f);
	}

	uint32_t frames_written;
	HRESULT result = output->ScheduleAudioSamples(int_samples.get(), samples.size() / 2,
		pts, TIMEBASE, &frames_written);
	if (result != S_OK) {
		fprintf(stderr, "ScheduleAudioSamples(pts=%ld) failed (result=0x%08x)\n", pts, result);
	} else {
		if (frames_written != samples.size() / 2) {
			fprintf(stderr, "ScheduleAudioSamples() returned short write (%u/%ld)\n", frames_written, samples.size() / 2);
		}
	}
}

void DeckLinkOutput::wait_for_frame(int64_t pts, int *dropped_frames, int64_t *frame_duration)
{
	*dropped_frames = 0;
	*frame_duration = this->frame_duration;

	const BMDTimeValue buffer = lrint(*frame_duration * global_flags.output_buffer_frames);
	const BMDTimeValue max_overshoot = lrint(*frame_duration * global_flags.output_slop_frames);
	BMDTimeValue target_time = pts - buffer;

	// While prerolling, we send out frames as quickly as we can.
	if (target_time < base_pts) {
		return;
	}

	if (!playback_started) {
		if (output->EndAudioPreroll() != S_OK) {
			fprintf(stderr, "Could not end audio preroll\n");
			exit(1);  // TODO
		}
		if (output->StartScheduledPlayback(base_pts, TIMEBASE, 1.0) != S_OK) {
			fprintf(stderr, "Could not start playback\n");
			exit(1);  // TODO
		}
		playback_started = true;
	}

	BMDTimeValue stream_frame_time;
	double playback_speed;
	output->GetScheduledStreamTime(TIMEBASE, &stream_frame_time, &playback_speed);

	// If we're ahead of time, wait for the frame to (approximately) start.
	if (stream_frame_time < target_time) {
		steady_clock::time_point t = steady_clock::now() +
			nanoseconds((target_time - stream_frame_time) * 1000000000 / TIMEBASE);
		this_thread::sleep_until(t);
		return;
	}

	// If we overshot the previous frame by just a little,
	// fire off one immediately.
	if (stream_frame_time < target_time + max_overshoot) {
		fprintf(stderr, "Warning: Frame was %ld ms late (but not skipping it due to --output-slop-frames).\n",
			lrint((stream_frame_time - target_time) * 1000.0 / TIMEBASE));
		return;
	}

	// Oops, we missed by more than one frame. Return immediately,
	// but drop so that we catch up.
	*dropped_frames = (stream_frame_time - target_time + *frame_duration - 1) / *frame_duration;
	fprintf(stderr, "Dropped %d output frames; skipping.\n", *dropped_frames);
}

HRESULT DeckLinkOutput::ScheduledFrameCompleted(/* in */ IDeckLinkVideoFrame *completedFrame, /* in */ BMDOutputFrameCompletionResult result)
{
	Frame *frame = static_cast<Frame *>(completedFrame);
	switch (result) {
	case bmdOutputFrameCompleted:
		break;
	case bmdOutputFrameDisplayedLate:
		fprintf(stderr, "Output frame displayed late (pts=%ld)\n", frame->pts);
		fprintf(stderr, "Consider increasing --output-buffer-frames if this persists.\n");
		break;
	case bmdOutputFrameDropped:
		fprintf(stderr, "Output frame was dropped (pts=%ld)\n", frame->pts);
		fprintf(stderr, "Consider increasing --output-buffer-frames if this persists.\n");
		break;
	case bmdOutputFrameFlushed:
		fprintf(stderr, "Output frame was flushed (pts=%ld)\n", frame->pts);
		break;
	default:
		fprintf(stderr, "Output frame completed with unknown status %d\n", result);
		break;
	}

	static int hei = 0;
	print_latency("DeckLink output latency (frame received → output on HDMI):", frame->received_ts, false, &hei);

	{
		lock_guard<mutex> lock(frame_queue_mutex);
		frame_freelist.push(unique_ptr<Frame>(frame));
		--num_frames_in_flight;
	}

	return S_OK;
}

HRESULT DeckLinkOutput::ScheduledPlaybackHasStopped()
{
	printf("playback stopped!\n");
	return S_OK;
}

unique_ptr<DeckLinkOutput::Frame> DeckLinkOutput::get_frame()
{
	lock_guard<mutex> lock(frame_queue_mutex);

	if (!frame_freelist.empty()) {
		unique_ptr<Frame> frame = move(frame_freelist.front());
		frame_freelist.pop();
		return frame;
	}

	unique_ptr<Frame> frame(new Frame);

	frame->uyvy_tex = resource_pool->create_2d_texture(GL_RGBA8, width / 2, height);

	glGenBuffers(1, &frame->pbo);
	check_error();
	glBindBuffer(GL_PIXEL_PACK_BUFFER, frame->pbo);
	check_error();
	glBufferStorage(GL_PIXEL_PACK_BUFFER, width * height * 2, NULL, GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT);
	check_error();
	frame->uyvy_ptr = (uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, width * height * 2, GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT);
	check_error();
	frame->uyvy_ptr_local.reset(new uint8_t[width * height * 2]);
	frame->resource_pool = resource_pool;

	return frame;
}

void DeckLinkOutput::present_thread_func()
{
	for ( ;; ) {
		unique_ptr<Frame> frame;
		{
                        unique_lock<mutex> lock(frame_queue_mutex);
                        frame_queues_changed.wait(lock, [this]{
                                return should_quit || !pending_video_frames.empty();
                        });
                        if (should_quit) {
				return;
			}
			frame = move(pending_video_frames.front());
			pending_video_frames.pop();
			++num_frames_in_flight;
		}

		glWaitSync(frame->fence.get(), /*flags=*/0, GL_TIMEOUT_IGNORED);
		check_error();
		frame->fence.reset();

		memcpy(frame->uyvy_ptr_local.get(), frame->uyvy_ptr, width * height * 2);

		// Release any input frames we needed to render this frame.
		frame->input_frames.clear();

		BMDTimeValue pts = frame->pts;
		BMDTimeValue duration = frame->duration;
		HRESULT res = output->ScheduleVideoFrame(frame.get(), pts, duration, TIMEBASE);
		if (res == S_OK) {
			frame.release();  // Owned by the driver now.
		} else {
			fprintf(stderr, "Could not schedule video frame! (error=0x%08x)\n", res);

			lock_guard<mutex> lock(frame_queue_mutex);
			frame_freelist.push(move(frame));
			--num_frames_in_flight;
		}
	}
}

HRESULT STDMETHODCALLTYPE DeckLinkOutput::QueryInterface(REFIID, LPVOID *)
{
	return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeckLinkOutput::AddRef()
{
	return refcount.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE DeckLinkOutput::Release()
{
	int new_ref = refcount.fetch_sub(1) - 1;
	if (new_ref == 0)
		delete this;
	return new_ref;
}

DeckLinkOutput::Frame::~Frame()
{
	glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
	check_error();
	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	check_error();
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	check_error();
	glDeleteBuffers(1, &pbo);
	check_error();
	resource_pool->release_2d_texture(uyvy_tex);
	check_error();
}

HRESULT STDMETHODCALLTYPE DeckLinkOutput::Frame::QueryInterface(REFIID, LPVOID *)
{
	return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeckLinkOutput::Frame::AddRef()
{
	return refcount.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE DeckLinkOutput::Frame::Release()
{
	int new_ref = refcount.fetch_sub(1) - 1;
	if (new_ref == 0)
		delete this;
	return new_ref;
}

long DeckLinkOutput::Frame::GetWidth()
{
	return global_flags.width;
}

long DeckLinkOutput::Frame::GetHeight()
{
	return global_flags.height;
}

long DeckLinkOutput::Frame::GetRowBytes()
{
	return global_flags.width * 2;
}

BMDPixelFormat DeckLinkOutput::Frame::GetPixelFormat()
{
	return bmdFormat8BitYUV;
}

BMDFrameFlags DeckLinkOutput::Frame::GetFlags()
{
	return bmdFrameFlagDefault;
}

HRESULT DeckLinkOutput::Frame::GetBytes(/* out */ void **buffer)
{
	*buffer = uyvy_ptr_local.get();
	return S_OK;
}

HRESULT DeckLinkOutput::Frame::GetTimecode(/* in */ BMDTimecodeFormat format, /* out */ IDeckLinkTimecode **timecode)
{
	fprintf(stderr, "STUB: GetTimecode()\n");
	return E_NOTIMPL;
}

HRESULT DeckLinkOutput::Frame::GetAncillaryData(/* out */ IDeckLinkVideoFrameAncillary **ancillary)
{
	fprintf(stderr, "STUB: GetAncillaryData()\n");
	return E_NOTIMPL;
}