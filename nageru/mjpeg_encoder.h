#ifndef _MJPEG_ENCODER_H
#define _MJPEG_ENCODER_H 1

#include "shared/ffmpeg_raii.h"
#include "ref_counted_frame.h"

extern "C" {

#include <libavformat/avio.h>

}  // extern "C"

#include <atomic>
#include <bmusb/bmusb.h>
#include <condition_variable>
#include <list>
#include <mutex>
#include <queue>
#include <stdint.h>
#include <string>
#include <thread>

#include <va/va.h>

class HTTPD;
struct jpeg_compress_struct;
struct VADisplayWithCleanup;
struct VectorDestinationManager;

class MJPEGEncoder {
public:
	MJPEGEncoder(HTTPD *httpd, const std::string &va_display);
	~MJPEGEncoder();
	void stop();
	void upload_frame(int64_t pts, unsigned card_index, RefCountedFrame frame, const bmusb::VideoFormat &video_format, size_t y_offset, size_t cbcr_offset);

private:
	static constexpr int quality = 90;

	struct VAResources {
		unsigned width, height;
		VASurfaceID surface;
		VAContextID context;
		VABufferID data_buffer;
	};

	// RAII wrapper to release VAResources on return (even on error).
	class ReleaseVAResources {
	public:
		ReleaseVAResources() : committed(true) {}

		ReleaseVAResources(MJPEGEncoder *mjpeg, const VAResources &resources)
			: mjpeg(mjpeg), resources(resources) {}

		ReleaseVAResources(ReleaseVAResources &) = delete;

		ReleaseVAResources(ReleaseVAResources &&other)
			: mjpeg(other.mjpeg), resources(other.resources), committed(other.committed) {
			other.commit();
		}

		ReleaseVAResources &operator= (ReleaseVAResources &) = delete;

		ReleaseVAResources &operator= (ReleaseVAResources &&other) {
			if (!committed) {
				mjpeg->release_va_resources(resources);
			}
			mjpeg = other.mjpeg;
			resources = std::move(other.resources);
			committed = other.committed;
			other.commit();
			return *this;
		}

		~ReleaseVAResources()
		{
			if (!committed) {
				mjpeg->release_va_resources(resources);
			}
		}

		void commit() { committed = true; }

	private:
		MJPEGEncoder *mjpeg = nullptr;
		VAResources resources;
		bool committed = false;
	};

	struct QueuedFrame {
		int64_t pts;
		unsigned card_index;
		RefCountedFrame frame;
		bmusb::VideoFormat video_format;
		size_t y_offset, cbcr_offset;

		// Only for frames in the process of being encoded by VA-API.
		VAResources resources;
		ReleaseVAResources resource_releaser;
	};

	void encoder_thread_func();
	void va_receiver_thread_func();
	void encode_jpeg_va(QueuedFrame &&qf);
	std::vector<uint8_t> encode_jpeg_libjpeg(const QueuedFrame &qf);
	void write_mjpeg_packet(int64_t pts, unsigned card_index, const std::vector<uint8_t> &jpeg);
	void init_jpeg_422(unsigned width, unsigned height, VectorDestinationManager *dest, jpeg_compress_struct *cinfo);
	std::vector<uint8_t> get_jpeg_header(unsigned width, unsigned height, jpeg_compress_struct *cinfo);

	static int write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);
	int write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);

	std::thread encoder_thread, va_receiver_thread;

	std::mutex mu;
	std::queue<QueuedFrame> frames_to_be_encoded;  // Under mu.
	std::condition_variable any_frames_to_be_encoded;  // Governs changes in both frames_to_be_encoded and frames_under_encoding

	std::queue<QueuedFrame> frames_encoding;  // Under mu. Used for VA-API only.
	std::condition_variable any_frames_encoding;

	AVFormatContextWithCloser avctx;
	HTTPD *httpd;
	std::string mux_header;
	std::atomic<bool> should_quit{false};
	bool running = false;

	std::unique_ptr<VADisplayWithCleanup> va_dpy;
	VAConfigID config_id;

	struct VAData {
		std::vector<uint8_t> jpeg_header;
		VAEncPictureParameterBufferJPEG pic_param;
		VAQMatrixBufferJPEG q;
		VAHuffmanTableBufferJPEGBaseline huff;
		VAEncSliceParameterBufferJPEG parms;
	};
	std::map<std::pair<unsigned, unsigned>, VAData> va_data_for_resolution;
	VAData get_va_data_for_resolution(unsigned width, unsigned height);

	std::list<VAResources> va_resources_freelist;
	std::mutex va_resources_mutex;
	VAResources get_va_resources(unsigned width, unsigned height);
	void release_va_resources(VAResources resources);

	static std::unique_ptr<VADisplayWithCleanup> try_open_va(const std::string &va_display, std::string *error, VAConfigID *config_id);

	uint8_t *tmp_y, *tmp_cbcr, *tmp_cb, *tmp_cr;  // Private to the encoder thread. Used by the libjpeg backend only.
};

#endif  // !defined(_MJPEG_ENCODER_H)
