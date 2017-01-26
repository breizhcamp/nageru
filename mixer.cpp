#undef Success

#include "mixer.h"

#include <assert.h>
#include <epoxy/egl.h>
#include <movit/effect_chain.h>
#include <movit/effect_util.h>
#include <movit/flat_input.h>
#include <movit/image_format.h>
#include <movit/init.h>
#include <movit/resource_pool.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ratio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "DeckLinkAPI.h"
#include "LinuxCOM.h"
#include "alsa_output.h"
#include "bmusb/bmusb.h"
#include "bmusb/fake_capture.h"
#include "chroma_subsampler.h"
#include "context.h"
#include "decklink_capture.h"
#include "decklink_output.h"
#include "defs.h"
#include "disk_space_estimator.h"
#include "flags.h"
#include "input_mapping.h"
#include "pbo_frame_allocator.h"
#include "ref_counted_gl_sync.h"
#include "resampling_queue.h"
#include "timebase.h"
#include "video_encoder.h"

class IDeckLink;
class QOpenGLContext;

using namespace movit;
using namespace std;
using namespace std::chrono;
using namespace std::placeholders;
using namespace bmusb;

Mixer *global_mixer = nullptr;
bool uses_mlock = false;

namespace {

void insert_new_frame(RefCountedFrame frame, unsigned field_num, bool interlaced, unsigned card_index, InputState *input_state)
{
	if (interlaced) {
		for (unsigned frame_num = FRAME_HISTORY_LENGTH; frame_num --> 1; ) {  // :-)
			input_state->buffered_frames[card_index][frame_num] =
				input_state->buffered_frames[card_index][frame_num - 1];
		}
		input_state->buffered_frames[card_index][0] = { frame, field_num };
	} else {
		for (unsigned frame_num = 0; frame_num < FRAME_HISTORY_LENGTH; ++frame_num) {
			input_state->buffered_frames[card_index][frame_num] = { frame, field_num };
		}
	}
}

}  // namespace

void QueueLengthPolicy::update_policy(int queue_length)
{
	if (queue_length < 0) {  // Starvation.
		if (been_at_safe_point_since_last_starvation && safe_queue_length < 5) {
			++safe_queue_length;
			fprintf(stderr, "Card %u: Starvation, increasing safe limit to %u frames\n",
				card_index, safe_queue_length);
		}
		frames_with_at_least_one = 0;
		been_at_safe_point_since_last_starvation = false;
		return;
	}
	if (queue_length > 0) {
		if (queue_length >= int(safe_queue_length)) {
			been_at_safe_point_since_last_starvation = true;
		}
		if (++frames_with_at_least_one >= 1000 && safe_queue_length > 0) {
			--safe_queue_length;
			fprintf(stderr, "Card %u: Spare frames for more than 1000 frames, reducing safe limit to %u frames\n",
				card_index, safe_queue_length);
			frames_with_at_least_one = 0;
		}
	} else {
		frames_with_at_least_one = 0;
	}
}

Mixer::Mixer(const QSurfaceFormat &format, unsigned num_cards)
	: httpd(),
	  num_cards(num_cards),
	  mixer_surface(create_surface(format)),
	  h264_encoder_surface(create_surface(format)),
	  decklink_output_surface(create_surface(format)),
	  audio_mixer(num_cards)
{
	CHECK(init_movit(MOVIT_SHADER_DIR, MOVIT_DEBUG_OFF));
	check_error();

	// Since we allow non-bouncing 4:2:2 YCbCrInputs, effective subpixel precision
	// will be halved when sampling them, and we need to compensate here.
	movit_texel_subpixel_precision /= 2.0;

	resource_pool.reset(new ResourcePool);
	theme.reset(new Theme(global_flags.theme_filename, global_flags.theme_dirs, resource_pool.get(), num_cards));
	for (unsigned i = 0; i < NUM_OUTPUTS; ++i) {
		output_channel[i].parent = this;
		output_channel[i].channel = i;
	}

	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_sRGB;
	inout_format.gamma_curve = GAMMA_sRGB;

	// Display chain; shows the live output produced by the main chain (its RGBA version).
	display_chain.reset(new EffectChain(global_flags.width, global_flags.height, resource_pool.get()));
	check_error();
	display_input = new FlatInput(inout_format, FORMAT_RGB, GL_UNSIGNED_BYTE, global_flags.width, global_flags.height);  // FIXME: GL_UNSIGNED_BYTE is really wrong.
	display_chain->add_input(display_input);
	display_chain->add_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED);
	display_chain->set_dither_bits(0);  // Don't bother.
	display_chain->finalize();

	video_encoder.reset(new VideoEncoder(resource_pool.get(), h264_encoder_surface, global_flags.va_display, global_flags.width, global_flags.height, &httpd, global_disk_space_estimator));

	// Start listening for clients only once VideoEncoder has written its header, if any.
	httpd.start(9095);

	// First try initializing the then PCI devices, then USB, then
	// fill up with fake cards until we have the desired number of cards.
	unsigned num_pci_devices = 0;
	unsigned card_index = 0;

	{
		IDeckLinkIterator *decklink_iterator = CreateDeckLinkIteratorInstance();
		if (decklink_iterator != nullptr) {
			for ( ; card_index < num_cards; ++card_index) {
				IDeckLink *decklink;
				if (decklink_iterator->Next(&decklink) != S_OK) {
					break;
				}

				DeckLinkCapture *capture = new DeckLinkCapture(decklink, card_index);
				DeckLinkOutput *output = new DeckLinkOutput(resource_pool.get(), decklink_output_surface, global_flags.width, global_flags.height, card_index);
				output->set_device(decklink);
				configure_card(card_index, capture, /*is_fake_capture=*/false, output);
				++num_pci_devices;
			}
			decklink_iterator->Release();
			fprintf(stderr, "Found %u DeckLink PCI card(s).\n", num_pci_devices);
		} else {
			fprintf(stderr, "DeckLink drivers not found. Probing for USB cards only.\n");
		}
	}

	unsigned num_usb_devices = BMUSBCapture::num_cards();
	for (unsigned usb_card_index = 0; usb_card_index < num_usb_devices && card_index < num_cards; ++usb_card_index, ++card_index) {
		BMUSBCapture *capture = new BMUSBCapture(usb_card_index);
		capture->set_card_disconnected_callback(bind(&Mixer::bm_hotplug_remove, this, card_index));
		configure_card(card_index, capture, /*is_fake_capture=*/false, /*output=*/nullptr);
	}
	fprintf(stderr, "Found %u USB card(s).\n", num_usb_devices);

	unsigned num_fake_cards = 0;
	for ( ; card_index < num_cards; ++card_index, ++num_fake_cards) {
		FakeCapture *capture = new FakeCapture(global_flags.width, global_flags.height, FAKE_FPS, OUTPUT_FREQUENCY, card_index, global_flags.fake_cards_audio);
		configure_card(card_index, capture, /*is_fake_capture=*/true, /*output=*/nullptr);
	}

	if (num_fake_cards > 0) {
		fprintf(stderr, "Initialized %u fake cards.\n", num_fake_cards);
	}

	BMUSBCapture::set_card_connected_callback(bind(&Mixer::bm_hotplug_add, this, _1));
	BMUSBCapture::start_bm_thread();

	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		cards[card_index].queue_length_policy.reset(card_index);
	}

	chroma_subsampler.reset(new ChromaSubsampler(resource_pool.get()));

	if (global_flags.enable_alsa_output) {
		alsa.reset(new ALSAOutput(OUTPUT_FREQUENCY, /*num_channels=*/2));
	}
	if (global_flags.output_card != -1) {
		desired_output_card_index = global_flags.output_card;
		set_output_card_internal(global_flags.output_card);
	}
}

Mixer::~Mixer()
{
	BMUSBCapture::stop_bm_thread();

	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		{
			unique_lock<mutex> lock(card_mutex);
			cards[card_index].should_quit = true;  // Unblock thread.
			cards[card_index].new_frames_changed.notify_all();
		}
		cards[card_index].capture->stop_dequeue_thread();
		if (cards[card_index].output) {
			cards[card_index].output->end_output();
			cards[card_index].output.reset();
		}
	}

	video_encoder.reset(nullptr);
}

void Mixer::configure_card(unsigned card_index, CaptureInterface *capture, bool is_fake_capture, DeckLinkOutput *output)
{
	printf("Configuring card %d...\n", card_index);

	CaptureCard *card = &cards[card_index];
	if (card->capture != nullptr) {
		card->capture->stop_dequeue_thread();
	}
	card->capture.reset(capture);
	card->is_fake_capture = is_fake_capture;
	if (card->output.get() != output) {
		card->output.reset(output);
	}
	card->capture->set_frame_callback(bind(&Mixer::bm_frame, this, card_index, _1, _2, _3, _4, _5, _6, _7));
	if (card->frame_allocator == nullptr) {
		card->frame_allocator.reset(new PBOFrameAllocator(8 << 20, global_flags.width, global_flags.height));  // 8 MB.
	}
	card->capture->set_video_frame_allocator(card->frame_allocator.get());
	if (card->surface == nullptr) {
		card->surface = create_surface_with_same_format(mixer_surface);
	}
	while (!card->new_frames.empty()) card->new_frames.pop();
	card->last_timecode = -1;
	card->capture->configure_card();

	// NOTE: start_bm_capture() happens in thread_func().

	DeviceSpec device{InputSourceType::CAPTURE_CARD, card_index};
	audio_mixer.reset_resampler(device);
	audio_mixer.set_display_name(device, card->capture->get_description());
	audio_mixer.trigger_state_changed_callback();
}

void Mixer::set_output_card_internal(int card_index)
{
	// We don't really need to take card_mutex, since we're in the mixer
	// thread and don't mess with any queues (which is the only thing that happens
	// from other threads), but it's probably the safest in the long run.
	unique_lock<mutex> lock(card_mutex);
	if (output_card_index != -1) {
		// Switch the old card from output to input.
		CaptureCard *old_card = &cards[output_card_index];
		old_card->output->end_output();

		// Stop the fake card that we put into place.
		// This needs to _not_ happen under the mutex, to avoid deadlock
		// (delivering the last frame needs to take the mutex).
		bmusb::CaptureInterface *fake_capture = old_card->capture.get();
		lock.unlock();
		fake_capture->stop_dequeue_thread();
		lock.lock();
		old_card->capture = move(old_card->parked_capture);
		old_card->is_fake_capture = false;
		old_card->capture->start_bm_capture();
	}
	if (card_index != -1) {
		CaptureCard *card = &cards[card_index];
		bmusb::CaptureInterface *capture = card->capture.get();
		// TODO: DeckLinkCapture::stop_dequeue_thread can actually take
		// several seconds to complete (blocking on DisableVideoInput);
		// see if we can maybe do it asynchronously.
		lock.unlock();
		capture->stop_dequeue_thread();
		lock.lock();
		card->parked_capture = move(card->capture);
		bmusb::CaptureInterface *fake_capture = new FakeCapture(global_flags.width, global_flags.height, FAKE_FPS, OUTPUT_FREQUENCY, card_index, global_flags.fake_cards_audio);
		configure_card(card_index, fake_capture, /*is_fake_capture=*/true, card->output.release());
		card->queue_length_policy.reset(card_index);
		card->capture->start_bm_capture();
		card->output->start_output(bmdModeHD720p5994, pts_int);  // FIXME
	}
	output_card_index = card_index;
}

namespace {

int unwrap_timecode(uint16_t current_wrapped, int last)
{
	uint16_t last_wrapped = last & 0xffff;
	if (current_wrapped > last_wrapped) {
		return (last & ~0xffff) | current_wrapped;
	} else {
		return 0x10000 + ((last & ~0xffff) | current_wrapped);
	}
}

}  // namespace

void Mixer::bm_frame(unsigned card_index, uint16_t timecode,
                     FrameAllocator::Frame video_frame, size_t video_offset, VideoFormat video_format,
		     FrameAllocator::Frame audio_frame, size_t audio_offset, AudioFormat audio_format)
{
	DeviceSpec device{InputSourceType::CAPTURE_CARD, card_index};
	CaptureCard *card = &cards[card_index];

	if (is_mode_scanning[card_index]) {
		if (video_format.has_signal) {
			// Found a stable signal, so stop scanning.
			is_mode_scanning[card_index] = false;
		} else {
			static constexpr double switch_time_s = 0.1;  // Should be enough time for the signal to stabilize.
			steady_clock::time_point now = steady_clock::now();
			double sec_since_last_switch = duration<double>(steady_clock::now() - last_mode_scan_change[card_index]).count();
			if (sec_since_last_switch > switch_time_s) {
				// It isn't this mode; try the next one.
				mode_scanlist_index[card_index]++;
				mode_scanlist_index[card_index] %= mode_scanlist[card_index].size();
				cards[card_index].capture->set_video_mode(mode_scanlist[card_index][mode_scanlist_index[card_index]]);
				last_mode_scan_change[card_index] = now;
			}
		}
	}

	int64_t frame_length = int64_t(TIMEBASE) * video_format.frame_rate_den / video_format.frame_rate_nom;
	assert(frame_length > 0);

	size_t num_samples = (audio_frame.len > audio_offset) ? (audio_frame.len - audio_offset) / audio_format.num_channels / (audio_format.bits_per_sample / 8) : 0;
	if (num_samples > OUTPUT_FREQUENCY / 10) {
		printf("Card %d: Dropping frame with implausible audio length (len=%d, offset=%d) [timecode=0x%04x video_len=%d video_offset=%d video_format=%x)\n",
			card_index, int(audio_frame.len), int(audio_offset),
			timecode, int(video_frame.len), int(video_offset), video_format.id);
		if (video_frame.owner) {
			video_frame.owner->release_frame(video_frame);
		}
		if (audio_frame.owner) {
			audio_frame.owner->release_frame(audio_frame);
		}
		return;
	}

	int dropped_frames = 0;
	if (card->last_timecode != -1) {
		dropped_frames = unwrap_timecode(timecode, card->last_timecode) - card->last_timecode - 1;
	}

	// Number of samples per frame if we need to insert silence.
	// (Could be nonintegral, but resampling will save us then.)
	const int silence_samples = OUTPUT_FREQUENCY * video_format.frame_rate_den / video_format.frame_rate_nom;

	if (dropped_frames > MAX_FPS * 2) {
		fprintf(stderr, "Card %d lost more than two seconds (or time code jumping around; from 0x%04x to 0x%04x), resetting resampler\n",
			card_index, card->last_timecode, timecode);
		audio_mixer.reset_resampler(device);
		dropped_frames = 0;
	} else if (dropped_frames > 0) {
		// Insert silence as needed.
		fprintf(stderr, "Card %d dropped %d frame(s) (before timecode 0x%04x), inserting silence.\n",
			card_index, dropped_frames, timecode);

		bool success;
		do {
			success = audio_mixer.add_silence(device, silence_samples, dropped_frames, frame_length);
		} while (!success);
	}

	audio_mixer.add_audio(device, audio_frame.data + audio_offset, num_samples, audio_format, frame_length);

	// Done with the audio, so release it.
	if (audio_frame.owner) {
		audio_frame.owner->release_frame(audio_frame);
	}

	card->last_timecode = timecode;

	size_t expected_length = video_format.width * (video_format.height + video_format.extra_lines_top + video_format.extra_lines_bottom) * 2;
	if (video_frame.len - video_offset == 0 ||
	    video_frame.len - video_offset != expected_length) {
		if (video_frame.len != 0) {
			printf("Card %d: Dropping video frame with wrong length (%ld; expected %ld)\n",
				card_index, video_frame.len - video_offset, expected_length);
		}
		if (video_frame.owner) {
			video_frame.owner->release_frame(video_frame);
		}

		// Still send on the information that we _had_ a frame, even though it's corrupted,
		// so that pts can go up accordingly.
		{
			unique_lock<mutex> lock(card_mutex);
			CaptureCard::NewFrame new_frame;
			new_frame.frame = RefCountedFrame(FrameAllocator::Frame());
			new_frame.length = frame_length;
			new_frame.interlaced = false;
			new_frame.dropped_frames = dropped_frames;
			card->new_frames.push(move(new_frame));
			card->new_frames_changed.notify_all();
		}
		return;
	}

	PBOFrameAllocator::Userdata *userdata = (PBOFrameAllocator::Userdata *)video_frame.userdata;

	unsigned num_fields = video_format.interlaced ? 2 : 1;
	steady_clock::time_point frame_upload_start;
	bool interlaced_stride = false;
	if (video_format.interlaced) {
		// Send the two fields along as separate frames; the other side will need to add
		// a deinterlacer to actually get this right.
		assert(video_format.height % 2 == 0);
		video_format.height /= 2;
		assert(frame_length % 2 == 0);
		frame_length /= 2;
		num_fields = 2;
		if (video_format.second_field_start == 1) {
			interlaced_stride = true;
		}
		frame_upload_start = steady_clock::now();
	}
	userdata->last_interlaced = video_format.interlaced;
	userdata->last_has_signal = video_format.has_signal;
	userdata->last_is_connected = video_format.is_connected;
	userdata->last_frame_rate_nom = video_format.frame_rate_nom;
	userdata->last_frame_rate_den = video_format.frame_rate_den;
	RefCountedFrame frame(video_frame);

	// Upload the textures.
	size_t cbcr_width = video_format.width / 2;
	size_t cbcr_offset = video_offset / 2;
	size_t y_offset = video_frame.size / 2 + video_offset / 2;

	for (unsigned field = 0; field < num_fields; ++field) {
		// Put the actual texture upload in a lambda that is executed in the main thread.
		// It is entirely possible to do this in the same thread (and it might even be
		// faster, depending on the GPU and driver), but it appears to be trickling
		// driver bugs very easily.
		//
		// Note that this means we must hold on to the actual frame data in <userdata>
		// until the upload command is run, but we hold on to <frame> much longer than that
		// (in fact, all the way until we no longer use the texture in rendering).
		auto upload_func = [field, video_format, y_offset, cbcr_offset, cbcr_width, interlaced_stride, userdata]() {
			unsigned field_start_line;
			if (field == 1) {
				field_start_line = video_format.second_field_start;
			} else {
				field_start_line = video_format.extra_lines_top;
			}

			if (userdata->tex_y[field] == 0 ||
			    userdata->tex_cbcr[field] == 0 ||
			    video_format.width != userdata->last_width[field] ||
			    video_format.height != userdata->last_height[field]) {
				// We changed resolution since last use of this texture, so we need to create
				// a new object. Note that this each card has its own PBOFrameAllocator,
				// we don't need to worry about these flip-flopping between resolutions.
				glBindTexture(GL_TEXTURE_2D, userdata->tex_cbcr[field]);
				check_error();
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, cbcr_width, video_format.height, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
				check_error();
				glBindTexture(GL_TEXTURE_2D, userdata->tex_y[field]);
				check_error();
				glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, video_format.width, video_format.height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
				check_error();
				userdata->last_width[field] = video_format.width;
				userdata->last_height[field] = video_format.height;
			}

			GLuint pbo = userdata->pbo;
			check_error();
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
			check_error();

			size_t field_y_start = y_offset + video_format.width * field_start_line;
			size_t field_cbcr_start = cbcr_offset + cbcr_width * field_start_line * sizeof(uint16_t);

			if (global_flags.flush_pbos) {
				glFlushMappedBufferRange(GL_PIXEL_UNPACK_BUFFER, field_y_start, video_format.width * video_format.height);
				check_error();
				glFlushMappedBufferRange(GL_PIXEL_UNPACK_BUFFER, field_cbcr_start, cbcr_width * video_format.height * sizeof(uint16_t));
				check_error();
			}

			glBindTexture(GL_TEXTURE_2D, userdata->tex_cbcr[field]);
			check_error();
			if (interlaced_stride) {
				glPixelStorei(GL_UNPACK_ROW_LENGTH, cbcr_width * 2);
				check_error();
			} else {
				glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
				check_error();
			}
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cbcr_width, video_format.height, GL_RG, GL_UNSIGNED_BYTE, BUFFER_OFFSET(field_cbcr_start));
			check_error();
			glBindTexture(GL_TEXTURE_2D, userdata->tex_y[field]);
			check_error();
			if (interlaced_stride) {
				glPixelStorei(GL_UNPACK_ROW_LENGTH, video_format.width * 2);
				check_error();
			} else {
				glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
				check_error();
			}
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, video_format.width, video_format.height, GL_RED, GL_UNSIGNED_BYTE, BUFFER_OFFSET(field_y_start));
			check_error();
			glBindTexture(GL_TEXTURE_2D, 0);
			check_error();
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
			check_error();
			glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
			check_error();
		};

		if (field == 1) {
			// Don't upload the second field as fast as we can; wait until
			// the field time has approximately passed. (Otherwise, we could
			// get timing jitter against the other sources, and possibly also
			// against the video display, although the latter is not as critical.)
			// This requires our system clock to be reasonably close to the
			// video clock, but that's not an unreasonable assumption.
			steady_clock::time_point second_field_start = frame_upload_start +
				nanoseconds(frame_length * 1000000000 / TIMEBASE);
			this_thread::sleep_until(second_field_start);
		}

		{
			unique_lock<mutex> lock(card_mutex);
			CaptureCard::NewFrame new_frame;
			new_frame.frame = frame;
			new_frame.length = frame_length;
			new_frame.field = field;
			new_frame.interlaced = video_format.interlaced;
			new_frame.upload_func = upload_func;
			new_frame.dropped_frames = dropped_frames;
			new_frame.received_timestamp = video_frame.received_timestamp;  // Ignore the audio timestamp.
			card->new_frames.push(move(new_frame));
			card->new_frames_changed.notify_all();
		}
	}
}

void Mixer::bm_hotplug_add(libusb_device *dev)
{
	lock_guard<mutex> lock(hotplug_mutex);
	hotplugged_cards.push_back(dev);
}

void Mixer::bm_hotplug_remove(unsigned card_index)
{
	cards[card_index].new_frames_changed.notify_all();
}

void Mixer::thread_func()
{
	pthread_setname_np(pthread_self(), "Mixer_OpenGL");

	eglBindAPI(EGL_OPENGL_API);
	QOpenGLContext *context = create_context(mixer_surface);
	if (!make_current(context, mixer_surface)) {
		printf("oops\n");
		exit(1);
	}

	// Start the actual capture. (We don't want to do it before we're actually ready
	// to process output frames.)
	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		if (int(card_index) != output_card_index) {
			cards[card_index].capture->start_bm_capture();
		}
	}

	steady_clock::time_point start, now;
	start = steady_clock::now();

	int frame = 0;
	int stats_dropped_frames = 0;

	while (!should_quit) {
		if (desired_output_card_index != output_card_index) {
			set_output_card_internal(desired_output_card_index);
		}

		CaptureCard::NewFrame new_frames[MAX_VIDEO_CARDS];
		bool has_new_frame[MAX_VIDEO_CARDS] = { false };

		bool master_card_is_output;
		unsigned master_card_index;
		if (output_card_index != -1) {
			master_card_is_output = true;
			master_card_index = output_card_index;
		} else {
			master_card_is_output = false;
			master_card_index = theme->map_signal(master_clock_channel);
			assert(master_card_index < num_cards);
		}

		OutputFrameInfo	output_frame_info = get_one_frame_from_each_card(master_card_index, master_card_is_output, new_frames, has_new_frame);
		schedule_audio_resampling_tasks(output_frame_info.dropped_frames, output_frame_info.num_samples, output_frame_info.frame_duration);
		stats_dropped_frames += output_frame_info.dropped_frames;

		handle_hotplugged_cards();

		for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
			if (card_index == master_card_index || !has_new_frame[card_index]) {
				continue;
			}
			if (new_frames[card_index].frame->len == 0) {
				++new_frames[card_index].dropped_frames;
			}
			if (new_frames[card_index].dropped_frames > 0) {
				printf("Card %u dropped %d frames before this\n",
					card_index, int(new_frames[card_index].dropped_frames));
			}
		}

		// If the first card is reporting a corrupted or otherwise dropped frame,
		// just increase the pts (skipping over this frame) and don't try to compute anything new.
		if (!master_card_is_output && new_frames[master_card_index].frame->len == 0) {
			++stats_dropped_frames;
			pts_int += new_frames[master_card_index].length;
			continue;
		}

		for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
			if (!has_new_frame[card_index] || new_frames[card_index].frame->len == 0)
				continue;

			CaptureCard::NewFrame *new_frame = &new_frames[card_index];
			assert(new_frame->frame != nullptr);
			insert_new_frame(new_frame->frame, new_frame->field, new_frame->interlaced, card_index, &input_state);
			check_error();

			// The new texture might need uploading before use.
			if (new_frame->upload_func) {
				new_frame->upload_func();
				new_frame->upload_func = nullptr;
			}
		}

		int64_t frame_duration = output_frame_info.frame_duration;
		render_one_frame(frame_duration);
		++frame;
		pts_int += frame_duration;

		now = steady_clock::now();
		double elapsed = duration<double>(now - start).count();
		if (frame % 100 == 0) {
			printf("%d frames (%d dropped) in %.3f seconds = %.1f fps (%.1f ms/frame)",
				frame, stats_dropped_frames, elapsed, frame / elapsed,
				1e3 * elapsed / frame);
		//	chain->print_phase_timing();

			// Check our memory usage, to see if we are close to our mlockall()
			// limit (if at all set).
			rusage used;
			if (getrusage(RUSAGE_SELF, &used) == -1) {
				perror("getrusage(RUSAGE_SELF)");
				assert(false);
			}

			if (uses_mlock) {
				rlimit limit;
				if (getrlimit(RLIMIT_MEMLOCK, &limit) == -1) {
					perror("getrlimit(RLIMIT_MEMLOCK)");
					assert(false);
				}

				if (limit.rlim_cur == 0) {
					printf(", using %ld MB memory (locked)",
						long(used.ru_maxrss / 1024));
				} else {
					printf(", using %ld / %ld MB lockable memory (%.1f%%)",
						long(used.ru_maxrss / 1024),
						long(limit.rlim_cur / 1048576),
						float(100.0 * (used.ru_maxrss * 1024.0) / limit.rlim_cur));
				}
			} else {
				printf(", using %ld MB memory (not locked)",
					long(used.ru_maxrss / 1024));
			}

			printf("\n");
		}


		if (should_cut.exchange(false)) {  // Test and clear.
			video_encoder->do_cut(frame);
		}

#if 0
		// Reset every 100 frames, so that local variations in frame times
		// (especially for the first few frames, when the shaders are
		// compiled etc.) don't make it hard to measure for the entire
		// remaining duration of the program.
		if (frame == 10000) {
			frame = 0;
			start = now;
		}
#endif
		check_error();
	}

	resource_pool->clean_context();
}

bool Mixer::input_card_is_master_clock(unsigned card_index, unsigned master_card_index) const
{
	if (output_card_index != -1) {
		// The output card (ie., cards[output_card_index].output) is the master clock,
		// so no input card (ie., cards[card_index].capture) is.
		return false;
	}
	return (card_index == master_card_index);
}

Mixer::OutputFrameInfo Mixer::get_one_frame_from_each_card(unsigned master_card_index, bool master_card_is_output, CaptureCard::NewFrame new_frames[MAX_VIDEO_CARDS], bool has_new_frame[MAX_VIDEO_CARDS])
{
	OutputFrameInfo output_frame_info;
start:
	unique_lock<mutex> lock(card_mutex, defer_lock);
	if (master_card_is_output) {
		// Clocked to the output, so wait for it to be ready for the next frame.
		cards[master_card_index].output->wait_for_frame(pts_int, &output_frame_info.dropped_frames, &output_frame_info.frame_duration);
		lock.lock();
	} else {
		// Wait for the master card to have a new frame.
		// TODO: Add a timeout.
		lock.lock();
		cards[master_card_index].new_frames_changed.wait(lock, [this, master_card_index]{ return !cards[master_card_index].new_frames.empty() || cards[master_card_index].capture->get_disconnected(); });
	}

	if (master_card_is_output) {
		handle_hotplugged_cards();
	} else if (cards[master_card_index].new_frames.empty()) {
		// We were woken up, but not due to a new frame. Deal with it
		// and then restart.
		assert(cards[master_card_index].capture->get_disconnected());
		handle_hotplugged_cards();
		goto start;
	}

	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		CaptureCard *card = &cards[card_index];
		if (card->new_frames.empty()) {
			assert(!input_card_is_master_clock(card_index, master_card_index));
			card->queue_length_policy.update_policy(-1);
			continue;
		}
		new_frames[card_index] = move(card->new_frames.front());
		has_new_frame[card_index] = true;
		card->new_frames.pop();
		card->new_frames_changed.notify_all();

		if (input_card_is_master_clock(card_index, master_card_index)) {
			// We don't use the queue length policy for the master card,
			// but we will if it stops being the master. Thus, clear out
			// the policy in case we switch in the future.
			card->queue_length_policy.reset(card_index);
		} else {
			// If we have excess frames compared to the policy for this card,
			// drop frames from the head.
			card->queue_length_policy.update_policy(card->new_frames.size());
			while (card->new_frames.size() > card->queue_length_policy.get_safe_queue_length()) {
				card->new_frames.pop();
			}
		}
	}

	if (!master_card_is_output) {
		output_frame_info.dropped_frames = new_frames[master_card_index].dropped_frames;
		output_frame_info.frame_duration = new_frames[master_card_index].length;
	}

	// This might get off by a fractional sample when changing master card
	// between ones with different frame rates, but that's fine.
	int num_samples_times_timebase = OUTPUT_FREQUENCY * output_frame_info.frame_duration + fractional_samples;
	output_frame_info.num_samples = num_samples_times_timebase / TIMEBASE;
	fractional_samples = num_samples_times_timebase % TIMEBASE;
	assert(output_frame_info.num_samples >= 0);

	return output_frame_info;
}

void Mixer::handle_hotplugged_cards()
{
	// Check for cards that have been disconnected since last frame.
	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		CaptureCard *card = &cards[card_index];
		if (card->capture->get_disconnected()) {
			fprintf(stderr, "Card %u went away, replacing with a fake card.\n", card_index);
			FakeCapture *capture = new FakeCapture(global_flags.width, global_flags.height, FAKE_FPS, OUTPUT_FREQUENCY, card_index, global_flags.fake_cards_audio);
			configure_card(card_index, capture, /*is_fake_capture=*/true, /*output=*/nullptr);
			card->queue_length_policy.reset(card_index);
			card->capture->start_bm_capture();
		}
	}

	// Check for cards that have been connected since last frame.
	vector<libusb_device *> hotplugged_cards_copy;
	{
		lock_guard<mutex> lock(hotplug_mutex);
		swap(hotplugged_cards, hotplugged_cards_copy);
	}
	for (libusb_device *new_dev : hotplugged_cards_copy) {
		// Look for a fake capture card where we can stick this in.
		int free_card_index = -1;
		for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
			if (cards[card_index].is_fake_capture) {
				free_card_index = card_index;
				break;
			}
		}

		if (free_card_index == -1) {
			fprintf(stderr, "New card plugged in, but no free slots -- ignoring.\n");
			libusb_unref_device(new_dev);
		} else {
			// BMUSBCapture takes ownership.
			fprintf(stderr, "New card plugged in, choosing slot %d.\n", free_card_index);
			CaptureCard *card = &cards[free_card_index];
			BMUSBCapture *capture = new BMUSBCapture(free_card_index, new_dev);
			configure_card(free_card_index, capture, /*is_fake_capture=*/false, /*output=*/nullptr);
			card->queue_length_policy.reset(free_card_index);
			capture->set_card_disconnected_callback(bind(&Mixer::bm_hotplug_remove, this, free_card_index));
			capture->start_bm_capture();
		}
	}
}


void Mixer::schedule_audio_resampling_tasks(unsigned dropped_frames, int num_samples_per_frame, int length_per_frame)
{
	// Resample the audio as needed, including from previously dropped frames.
	assert(num_cards > 0);
	for (unsigned frame_num = 0; frame_num < dropped_frames + 1; ++frame_num) {
		const bool dropped_frame = (frame_num != dropped_frames);
		{
			// Signal to the audio thread to process this frame.
			// Note that if the frame is a dropped frame, we signal that
			// we don't want to use this frame as base for adjusting
			// the resampler rate. The reason for this is that the timing
			// of these frames is often way too late; they typically don't
			// “arrive” before we synthesize them. Thus, we could end up
			// in a situation where we have inserted e.g. five audio frames
			// into the queue before we then start pulling five of them
			// back out. This makes ResamplingQueue overestimate the delay,
			// causing undue resampler changes. (We _do_ use the last,
			// non-dropped frame; perhaps we should just discard that as well,
			// since dropped frames are expected to be rare, and it might be
			// better to just wait until we have a slightly more normal situation).
			unique_lock<mutex> lock(audio_mutex);
			bool adjust_rate = !dropped_frame;
			audio_task_queue.push(AudioTask{pts_int, num_samples_per_frame, adjust_rate});
			audio_task_queue_changed.notify_one();
		}
		if (dropped_frame) {
			// For dropped frames, increase the pts. Note that if the format changed
			// in the meantime, we have no way of detecting that; we just have to
			// assume the frame length is always the same.
			pts_int += length_per_frame;
		}
	}
}

void Mixer::render_one_frame(int64_t duration)
{
	// Get the main chain from the theme, and set its state immediately.
	Theme::Chain theme_main_chain = theme->get_chain(0, pts(), global_flags.width, global_flags.height, input_state);
	EffectChain *chain = theme_main_chain.chain;
	theme_main_chain.setup_chain();
	//theme_main_chain.chain->enable_phase_timing(true);

	GLuint y_tex, cbcr_tex;
	bool got_frame = video_encoder->begin_frame(&y_tex, &cbcr_tex);
	assert(got_frame);

	// Render main chain.
	GLuint cbcr_full_tex = resource_pool->create_2d_texture(GL_RG8, global_flags.width, global_flags.height);
	GLuint rgba_tex = resource_pool->create_2d_texture(GL_RGB565, global_flags.width, global_flags.height);  // Saves texture bandwidth, although dithering gets messed up.
	GLuint fbo = resource_pool->create_fbo(y_tex, cbcr_full_tex, rgba_tex);
	check_error();
	chain->render_to_fbo(fbo, global_flags.width, global_flags.height);
	resource_pool->release_fbo(fbo);

	chroma_subsampler->subsample_chroma(cbcr_full_tex, global_flags.width, global_flags.height, cbcr_tex);
	if (output_card_index != -1) {
		cards[output_card_index].output->send_frame(y_tex, cbcr_full_tex, theme_main_chain.input_frames, pts_int, duration);
	}
	resource_pool->release_2d_texture(cbcr_full_tex);

	// Set the right state for rgba_tex.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, rgba_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	const int64_t av_delay = lrint(global_flags.audio_queue_length_ms * 0.001 * TIMEBASE);  // Corresponds to the delay in ResamplingQueue.
	RefCountedGLsync fence = video_encoder->end_frame(pts_int + av_delay, duration, theme_main_chain.input_frames);

	// The live frame just shows the RGBA texture we just rendered.
	// It owns rgba_tex now.
	DisplayFrame live_frame;
	live_frame.chain = display_chain.get();
	live_frame.setup_chain = [this, rgba_tex]{
		display_input->set_texture_num(rgba_tex);
	};
	live_frame.ready_fence = fence;
	live_frame.input_frames = {};
	live_frame.temp_textures = { rgba_tex };
	output_channel[OUTPUT_LIVE].output_frame(live_frame);

	// Set up preview and any additional channels.
	for (int i = 1; i < theme->get_num_channels() + 2; ++i) {
		DisplayFrame display_frame;
		Theme::Chain chain = theme->get_chain(i, pts(), global_flags.width, global_flags.height, input_state);  // FIXME: dimensions
		display_frame.chain = chain.chain;
		display_frame.setup_chain = chain.setup_chain;
		display_frame.ready_fence = fence;
		display_frame.input_frames = chain.input_frames;
		display_frame.temp_textures = {};
		output_channel[i].output_frame(display_frame);
	}
}

void Mixer::audio_thread_func()
{
	pthread_setname_np(pthread_self(), "Mixer_Audio");

	while (!should_quit) {
		AudioTask task;

		{
			unique_lock<mutex> lock(audio_mutex);
			audio_task_queue_changed.wait(lock, [this]{ return should_quit || !audio_task_queue.empty(); });
			if (should_quit) {
				return;
			}
			task = audio_task_queue.front();
			audio_task_queue.pop();
		}

		ResamplingQueue::RateAdjustmentPolicy rate_adjustment_policy =
			task.adjust_rate ? ResamplingQueue::ADJUST_RATE : ResamplingQueue::DO_NOT_ADJUST_RATE;
		vector<float> samples_out = audio_mixer.get_output(
			double(task.pts_int) / TIMEBASE,
			task.num_samples,
			rate_adjustment_policy);

		// Send the samples to the sound card, then add them to the output.
		if (alsa) {
			alsa->write(samples_out);
		}
		if (output_card_index != -1) {
			const int64_t av_delay = lrint(global_flags.audio_queue_length_ms * 0.001 * TIMEBASE);  // Corresponds to the delay in ResamplingQueue.
			cards[output_card_index].output->send_audio(task.pts_int + av_delay, samples_out);
		}
		video_encoder->add_audio(task.pts_int, move(samples_out));
	}
}

void Mixer::release_display_frame(DisplayFrame *frame)
{
	for (GLuint texnum : frame->temp_textures) {
		resource_pool->release_2d_texture(texnum);
	}
	frame->temp_textures.clear();
	frame->ready_fence.reset();
	frame->input_frames.clear();
}

void Mixer::start()
{
	mixer_thread = thread(&Mixer::thread_func, this);
	audio_thread = thread(&Mixer::audio_thread_func, this);
}

void Mixer::quit()
{
	should_quit = true;
	audio_task_queue_changed.notify_one();
	mixer_thread.join();
	audio_thread.join();
}

void Mixer::transition_clicked(int transition_num)
{
	theme->transition_clicked(transition_num, pts());
}

void Mixer::channel_clicked(int preview_num)
{
	theme->channel_clicked(preview_num);
}

void Mixer::start_mode_scanning(unsigned card_index)
{
	assert(card_index < num_cards);
	if (is_mode_scanning[card_index]) {
		return;
	}
	is_mode_scanning[card_index] = true;
	mode_scanlist[card_index].clear();
	for (const auto &mode : cards[card_index].capture->get_available_video_modes()) {
		mode_scanlist[card_index].push_back(mode.first);
	}
	assert(!mode_scanlist[card_index].empty());
	mode_scanlist_index[card_index] = 0;
	cards[card_index].capture->set_video_mode(mode_scanlist[card_index][0]);
	last_mode_scan_change[card_index] = steady_clock::now();
}

Mixer::OutputChannel::~OutputChannel()
{
	if (has_current_frame) {
		parent->release_display_frame(&current_frame);
	}
	if (has_ready_frame) {
		parent->release_display_frame(&ready_frame);
	}
}

void Mixer::OutputChannel::output_frame(DisplayFrame frame)
{
	// Store this frame for display. Remove the ready frame if any
	// (it was seemingly never used).
	{
		unique_lock<mutex> lock(frame_mutex);
		if (has_ready_frame) {
			parent->release_display_frame(&ready_frame);
		}
		ready_frame = frame;
		has_ready_frame = true;
	}

	if (new_frame_ready_callback) {
		new_frame_ready_callback();
	}

	// Reduce the number of callbacks by filtering duplicates. The reason
	// why we bother doing this is that Qt seemingly can get into a state
	// where its builds up an essentially unbounded queue of signals,
	// consuming more and more memory, and there's no good way of collapsing
	// user-defined signals or limiting the length of the queue.
	if (transition_names_updated_callback) {
		vector<string> transition_names = global_mixer->get_transition_names();
		bool changed = false;
		if (transition_names.size() != last_transition_names.size()) {
			changed = true;
		} else {
			for (unsigned i = 0; i < transition_names.size(); ++i) {
				if (transition_names[i] != last_transition_names[i]) {
					changed = true;
					break;
				}
			}
		}
		if (changed) {
			transition_names_updated_callback(transition_names);
			last_transition_names = transition_names;
		}
	}
	if (name_updated_callback) {
		string name = global_mixer->get_channel_name(channel);
		if (name != last_name) {
			name_updated_callback(name);
			last_name = name;
		}
	}
	if (color_updated_callback) {
		string color = global_mixer->get_channel_color(channel);
		if (color != last_color) {
			color_updated_callback(color);
			last_color = color;
		}
	}
}

bool Mixer::OutputChannel::get_display_frame(DisplayFrame *frame)
{
	unique_lock<mutex> lock(frame_mutex);
	if (!has_current_frame && !has_ready_frame) {
		return false;
	}

	if (has_current_frame && has_ready_frame) {
		// We have a new ready frame. Toss the current one.
		parent->release_display_frame(&current_frame);
		has_current_frame = false;
	}
	if (has_ready_frame) {
		assert(!has_current_frame);
		current_frame = ready_frame;
		ready_frame.ready_fence.reset();  // Drop the refcount.
		ready_frame.input_frames.clear();  // Drop the refcounts.
		has_current_frame = true;
		has_ready_frame = false;
	}

	*frame = current_frame;
	return true;
}

void Mixer::OutputChannel::set_frame_ready_callback(Mixer::new_frame_ready_callback_t callback)
{
	new_frame_ready_callback = callback;
}

void Mixer::OutputChannel::set_transition_names_updated_callback(Mixer::transition_names_updated_callback_t callback)
{
	transition_names_updated_callback = callback;
}

void Mixer::OutputChannel::set_name_updated_callback(Mixer::name_updated_callback_t callback)
{
	name_updated_callback = callback;
}

void Mixer::OutputChannel::set_color_updated_callback(Mixer::color_updated_callback_t callback)
{
	color_updated_callback = callback;
}

mutex RefCountedGLsync::fence_lock;
