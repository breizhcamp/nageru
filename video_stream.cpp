#include "video_stream.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include <jpeglib.h>
#include <unistd.h>

#include "chroma_subsampler.h"
#include "context.h"
#include "flow.h"
#include "httpd.h"
#include "jpeg_frame_view.h"
#include "movit/util.h"
#include "mux.h"
#include "player.h"
#include "util.h"

#include <epoxy/glx.h>

using namespace std;

extern HTTPD *global_httpd;

namespace {

string read_file(const string &filename)
{
	FILE *fp = fopen(filename.c_str(), "rb");
	if (fp == nullptr) {
		perror(filename.c_str());
		return "";
	}

	fseek(fp, 0, SEEK_END);
	long len = ftell(fp);
	rewind(fp);

	string ret;
	ret.resize(len);
	fread(&ret[0], len, 1, fp);
	fclose(fp);
	return ret;
}

}  // namespace

struct VectorDestinationManager {
	jpeg_destination_mgr pub;
	std::vector<uint8_t> dest;

	VectorDestinationManager()
	{
		pub.init_destination = init_destination_thunk;
		pub.empty_output_buffer = empty_output_buffer_thunk;
		pub.term_destination = term_destination_thunk;
	}

	static void init_destination_thunk(j_compress_ptr ptr)
	{
		((VectorDestinationManager *)(ptr->dest))->init_destination();
	}

	inline void init_destination()
	{
		make_room(0);
	}

	static boolean empty_output_buffer_thunk(j_compress_ptr ptr)
	{
		return ((VectorDestinationManager *)(ptr->dest))->empty_output_buffer();
	}

	inline bool empty_output_buffer()
	{
		make_room(dest.size());  // Should ignore pub.free_in_buffer!
		return true;
	}

	inline void make_room(size_t bytes_used)
	{
		dest.resize(bytes_used + 4096);
		dest.resize(dest.capacity());
		pub.next_output_byte = dest.data() + bytes_used;
		pub.free_in_buffer = dest.size() - bytes_used;
	}

	static void term_destination_thunk(j_compress_ptr ptr)
	{
		((VectorDestinationManager *)(ptr->dest))->term_destination();
	}

	inline void term_destination()
	{
		dest.resize(dest.size() - pub.free_in_buffer);
	}
};
static_assert(std::is_standard_layout<VectorDestinationManager>::value, "");

vector<uint8_t> encode_jpeg(const uint8_t *y_data, const uint8_t *cb_data, const uint8_t *cr_data, unsigned width, unsigned height)
{
	VectorDestinationManager dest;

	jpeg_compress_struct cinfo;
	jpeg_error_mgr jerr;
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);

	cinfo.dest = (jpeg_destination_mgr *)&dest;
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_RGB;
	jpeg_set_defaults(&cinfo);
	constexpr int quality = 90;
	jpeg_set_quality(&cinfo, quality, /*force_baseline=*/false);

	cinfo.image_width = width;
	cinfo.image_height = height;
	cinfo.raw_data_in = true;
	jpeg_set_colorspace(&cinfo, JCS_YCbCr);
	cinfo.comp_info[0].h_samp_factor = 2;
	cinfo.comp_info[0].v_samp_factor = 1;
	cinfo.comp_info[1].h_samp_factor = 1;
	cinfo.comp_info[1].v_samp_factor = 1;
	cinfo.comp_info[2].h_samp_factor = 1;
	cinfo.comp_info[2].v_samp_factor = 1;
	cinfo.CCIR601_sampling = true;  // Seems to be mostly ignored by libjpeg, though.
	jpeg_start_compress(&cinfo, true);

	JSAMPROW yptr[8], cbptr[8], crptr[8];
	JSAMPARRAY data[3] = { yptr, cbptr, crptr };
	for (unsigned y = 0; y < height; y += 8) {
		for (unsigned yy = 0; yy < 8; ++yy) {
			yptr[yy] = const_cast<JSAMPROW>(&y_data[(height - y - yy - 1) * width]);
			cbptr[yy] = const_cast<JSAMPROW>(&cb_data[(height - y - yy - 1) * width/2]);
			crptr[yy] = const_cast<JSAMPROW>(&cr_data[(height - y - yy - 1) * width/2]);
		}

		jpeg_write_raw_data(&cinfo, data, /*num_lines=*/8);
	}

	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);

	return move(dest.dest);
}

VideoStream::VideoStream()
{
	using namespace movit;

	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_sRGB;
	inout_format.gamma_curve = GAMMA_sRGB;

	ycbcr_format.luma_coefficients = YCBCR_REC_709;
	ycbcr_format.full_range = true;  // JPEG.
	ycbcr_format.num_levels = 256;
	ycbcr_format.chroma_subsampling_x = 2;
	ycbcr_format.chroma_subsampling_y = 1;
	ycbcr_format.cb_x_position = 0.0f;  // H.264 -- _not_ JPEG, even though our input is MJPEG-encoded
	ycbcr_format.cb_y_position = 0.5f;  // Irrelevant.
	ycbcr_format.cr_x_position = 0.0f;
	ycbcr_format.cr_y_position = 0.5f;

	YCbCrFormat ycbcr_output_format = ycbcr_format;
	ycbcr_output_format.chroma_subsampling_x = 1;

	// TODO: deduplicate code against JPEGFrameView?
	ycbcr_planar_convert_chain.reset(new EffectChain(1280, 720));
	ycbcr_planar_input = (movit::YCbCrInput *)ycbcr_planar_convert_chain->add_input(new YCbCrInput(inout_format, ycbcr_format, 1280, 720, YCBCR_INPUT_PLANAR));

	// One full Y'CbCr texture (for interpolation), one that's just Y (throwing away the
	// Cb and Cr channels). The second copy is sort of redundant, but it's the easiest way
	// of getting the gray data into a layered texture.
	ycbcr_planar_convert_chain->add_ycbcr_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, ycbcr_output_format);
	ycbcr_planar_convert_chain->add_ycbcr_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, ycbcr_output_format);
	ycbcr_planar_convert_chain->set_dither_bits(8);
	ycbcr_planar_convert_chain->finalize();

	// Same, for semiplanar inputs.
	ycbcr_semiplanar_convert_chain.reset(new EffectChain(1280, 720));
	ycbcr_semiplanar_input = (movit::YCbCrInput *)ycbcr_semiplanar_convert_chain->add_input(new YCbCrInput(inout_format, ycbcr_format, 1280, 720, YCBCR_INPUT_SPLIT_Y_AND_CBCR));

	// One full Y'CbCr texture (for interpolation), one that's just Y (throwing away the
	// Cb and Cr channels). The second copy is sort of redundant, but it's the easiest way
	// of getting the gray data into a layered texture.
	ycbcr_semiplanar_convert_chain->add_ycbcr_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, ycbcr_output_format);
	ycbcr_semiplanar_convert_chain->add_ycbcr_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, ycbcr_output_format);
	ycbcr_semiplanar_convert_chain->set_dither_bits(8);
	ycbcr_semiplanar_convert_chain->finalize();

	GLuint input_tex[num_interpolate_slots], gray_tex[num_interpolate_slots], cb_tex[num_interpolate_slots], cr_tex[num_interpolate_slots];
	glCreateTextures(GL_TEXTURE_2D_ARRAY, 10, input_tex);
	glCreateTextures(GL_TEXTURE_2D_ARRAY, 10, gray_tex);
	glCreateTextures(GL_TEXTURE_2D, 10, cb_tex);
	glCreateTextures(GL_TEXTURE_2D, 10, cr_tex);
	check_error();
	constexpr size_t width = 1280, height = 720;  // FIXME: adjustable width, height
	int levels = find_num_levels(width, height);
	for (size_t i = 0; i < num_interpolate_slots; ++i) {
		glTextureStorage3D(input_tex[i], levels, GL_RGBA8, width, height, 2);
		check_error();
		glTextureStorage3D(gray_tex[i], levels, GL_R8, width, height, 2);
		check_error();
		glTextureStorage2D(cb_tex[i], 1, GL_R8, width / 2, height);
		check_error();
		glTextureStorage2D(cr_tex[i], 1, GL_R8, width / 2, height);
		check_error();

		InterpolatedFrameResources resource;
		resource.input_tex = input_tex[i];
		resource.gray_tex = gray_tex[i];
		resource.cb_tex = cb_tex[i];
		resource.cr_tex = cr_tex[i];
		glCreateFramebuffers(2, resource.input_fbos);
		check_error();

		glNamedFramebufferTextureLayer(resource.input_fbos[0], GL_COLOR_ATTACHMENT0, input_tex[i], 0, 0);
		check_error();
		glNamedFramebufferTextureLayer(resource.input_fbos[0], GL_COLOR_ATTACHMENT1, gray_tex[i], 0, 0);
		check_error();
		glNamedFramebufferTextureLayer(resource.input_fbos[1], GL_COLOR_ATTACHMENT0, input_tex[i], 0, 1);
		check_error();
		glNamedFramebufferTextureLayer(resource.input_fbos[1], GL_COLOR_ATTACHMENT1, gray_tex[i], 0, 1);
		check_error();

		GLuint bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
		glNamedFramebufferDrawBuffers(resource.input_fbos[0], 2, bufs);
		check_error();
		glNamedFramebufferDrawBuffers(resource.input_fbos[1], 2, bufs);
		check_error();

		glCreateBuffers(1, &resource.pbo);
		check_error();
		glNamedBufferStorage(resource.pbo, width * height * 4, nullptr, GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT);
		check_error();
 		resource.pbo_contents = glMapNamedBufferRange(resource.pbo, 0, width * height * 4, GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT); 
		interpolate_resources.push_back(resource);
	}

	check_error();

	compute_flow.reset(new DISComputeFlow(width, height, operating_point2));
	interpolate.reset(new Interpolate(operating_point2, /*split_ycbcr_output=*/true));
	chroma_subsampler.reset(new ChromaSubsampler);
	check_error();
}

VideoStream::~VideoStream() {}

void VideoStream::start()
{
	AVFormatContext *avctx = avformat_alloc_context();
	avctx->oformat = av_guess_format("nut", nullptr, nullptr);

	uint8_t *buf = (uint8_t *)av_malloc(MUX_BUFFER_SIZE);
	avctx->pb = avio_alloc_context(buf, MUX_BUFFER_SIZE, 1, this, nullptr, nullptr, nullptr);
	avctx->pb->write_data_type = &VideoStream::write_packet2_thunk;
	avctx->pb->ignore_boundary_point = 1;

	Mux::Codec video_codec = Mux::CODEC_MJPEG;

	avctx->flags = AVFMT_FLAG_CUSTOM_IO;

	string video_extradata;

	constexpr int width = 1280, height = 720;  // Doesn't matter for MJPEG.
	stream_mux.reset(new Mux(avctx, width, height, video_codec, video_extradata, /*audio_codec_parameters=*/nullptr, COARSE_TIMEBASE,
		/*write_callback=*/nullptr, Mux::WRITE_FOREGROUND, {}));


	encode_thread = thread(&VideoStream::encode_thread_func, this);
}

void VideoStream::stop()
{
	encode_thread.join();
}

void VideoStream::schedule_original_frame(int64_t output_pts, unsigned stream_idx, int64_t input_pts)
{
	fprintf(stderr, "output_pts=%ld  original      input_pts=%ld\n", output_pts, input_pts);

	QueuedFrame qf;
	qf.type = QueuedFrame::ORIGINAL;
	qf.output_pts = output_pts;
	qf.stream_idx = stream_idx;
	qf.input_first_pts = input_pts;	

	unique_lock<mutex> lock(queue_lock);
	frame_queue.push_back(qf);
	queue_nonempty.notify_all();
}

void VideoStream::schedule_interpolated_frame(int64_t output_pts, unsigned stream_idx, int64_t input_first_pts, int64_t input_second_pts, float alpha)
{
	fprintf(stderr, "output_pts=%ld  interpolated  input_pts1=%ld input_pts2=%ld alpha=%.3f\n", output_pts, input_first_pts, input_second_pts, alpha);

	// Get the temporary OpenGL resources we need for doing the interpolation.
	InterpolatedFrameResources resources;
	{
		unique_lock<mutex> lock(queue_lock);
		if (interpolate_resources.empty()) {
			fprintf(stderr, "WARNING: Too many interpolated frames already in transit; dropping one.\n");
			JPEGFrameView::insert_interpolated_frame(stream_idx, output_pts, nullptr);
			return;
		}
		resources = interpolate_resources.front();
		interpolate_resources.pop_front();
	}

	QueuedFrame qf;
	qf.type = QueuedFrame::INTERPOLATED;
	qf.output_pts = output_pts;
	qf.stream_idx = stream_idx;
	qf.resources = resources;

	check_error();

	// Convert frame0 and frame1 to OpenGL textures.
	// TODO: Deduplicate against JPEGFrameView::setDecodedFrame?
	for (size_t frame_no = 0; frame_no < 2; ++frame_no) {
		JPEGID jpeg_id;
		jpeg_id.stream_idx = stream_idx;
		jpeg_id.pts = frame_no == 1 ? input_second_pts : input_first_pts;
		jpeg_id.interpolated = false;
		bool did_decode;
		shared_ptr<Frame> frame = decode_jpeg_with_cache(jpeg_id, DECODE_IF_NOT_IN_CACHE, &did_decode);
		ycbcr_format.chroma_subsampling_x = frame->chroma_subsampling_x;
		ycbcr_format.chroma_subsampling_y = frame->chroma_subsampling_y;

		if (frame->is_semiplanar) {
			ycbcr_semiplanar_input->change_ycbcr_format(ycbcr_format);
			ycbcr_semiplanar_input->set_width(frame->width);
			ycbcr_semiplanar_input->set_height(frame->height);
			ycbcr_semiplanar_input->set_pixel_data(0, frame->y.get());
			ycbcr_semiplanar_input->set_pixel_data(1, frame->cbcr.get());
			ycbcr_semiplanar_input->set_pitch(0, frame->pitch_y);
			ycbcr_semiplanar_input->set_pitch(1, frame->pitch_chroma);
			ycbcr_semiplanar_convert_chain->render_to_fbo(resources.input_fbos[frame_no], 1280, 720);
		} else {
			ycbcr_planar_input->change_ycbcr_format(ycbcr_format);
			ycbcr_planar_input->set_width(frame->width);
			ycbcr_planar_input->set_height(frame->height);
			ycbcr_planar_input->set_pixel_data(0, frame->y.get());
			ycbcr_planar_input->set_pixel_data(1, frame->cb.get());
			ycbcr_planar_input->set_pixel_data(2, frame->cr.get());
			ycbcr_planar_input->set_pitch(0, frame->pitch_y);
			ycbcr_planar_input->set_pitch(1, frame->pitch_chroma);
			ycbcr_planar_input->set_pitch(2, frame->pitch_chroma);
			ycbcr_planar_convert_chain->render_to_fbo(resources.input_fbos[frame_no], 1280, 720);
		}
	}

	glGenerateTextureMipmap(resources.input_tex);
	check_error();
	glGenerateTextureMipmap(resources.gray_tex);
	check_error();

	// Compute the interpolated frame.
	qf.flow_tex = compute_flow->exec(resources.gray_tex, DISComputeFlow::FORWARD_AND_BACKWARD, DISComputeFlow::DO_NOT_RESIZE_FLOW);
	check_error();
	tie(qf.output_tex, qf.cbcr_tex) = interpolate->exec(resources.input_tex, resources.gray_tex, qf.flow_tex, 1280, 720, alpha);
	check_error();

	// Subsample and split Cb/Cr.
	chroma_subsampler->subsample_chroma(qf.cbcr_tex, 1280, 720, resources.cb_tex, resources.cr_tex);

	// We could have released qf.flow_tex here, but to make sure we don't cause a stall
	// when trying to reuse it for the next frame, we can just as well hold on to it
	// and release it only when the readback is done.

	// Read it down (asynchronously) to the CPU.
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, resources.pbo);
	check_error();
	glGetTextureImage(qf.output_tex, 0, GL_RED, GL_UNSIGNED_BYTE, 1280 * 720 * 4, BUFFER_OFFSET(0));
	check_error();
	glGetTextureImage(resources.cb_tex, 0, GL_RED, GL_UNSIGNED_BYTE, 1280 * 720 * 3, BUFFER_OFFSET(1280 * 720));
	check_error();
	glGetTextureImage(resources.cr_tex, 0, GL_RED, GL_UNSIGNED_BYTE, 1280 * 720 * 3 - 640 * 720, BUFFER_OFFSET(1280 * 720 + 640 * 720));
	check_error();
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	// Set a fence we can wait for to make sure the CPU sees the read.
	glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
	check_error();
	qf.fence = RefCountedGLsync(GL_SYNC_GPU_COMMANDS_COMPLETE, /*flags=*/0);
	check_error();

	unique_lock<mutex> lock(queue_lock);
	frame_queue.push_back(qf);
	queue_nonempty.notify_all();
}

void VideoStream::encode_thread_func()
{
	pthread_setname_np(pthread_self(), "VideoStream");
	QSurface *surface = create_surface();
	QOpenGLContext *context = create_context(surface);
	bool ok = make_current(context, surface);
	if (!ok) {
		fprintf(stderr, "Video stream couldn't get an OpenGL context\n");
		exit(1);
	}

	for ( ;; ) {
		QueuedFrame qf;
		{
			unique_lock<mutex> lock(queue_lock);
			queue_nonempty.wait(lock, [this]{
				return !frame_queue.empty();
			});
			qf = frame_queue.front();
			frame_queue.pop_front();
		}

		if (qf.type == QueuedFrame::ORIGINAL) {
			// Send the JPEG frame on, unchanged.
			string jpeg = read_file(filename_for_frame(qf.stream_idx, qf.input_first_pts));
			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.stream_index = 0;
			pkt.data = (uint8_t *)jpeg.data();
			pkt.size = jpeg.size();
			stream_mux->add_packet(pkt, qf.output_pts, qf.output_pts);
		} else if (qf.type == QueuedFrame::INTERPOLATED) {
			glClientWaitSync(qf.fence.get(), /*flags=*/0, GL_TIMEOUT_IGNORED);

			const uint8_t *y = (const uint8_t *)qf.resources.pbo_contents;
			const uint8_t *cb = (const uint8_t *)qf.resources.pbo_contents + 1280 * 720;
			const uint8_t *cr = (const uint8_t *)qf.resources.pbo_contents + 1280 * 720 + 640 * 720;

			// Send a copy of the frame on to display.
			shared_ptr<Frame> frame(new Frame);
			frame->y.reset(new uint8_t[1280 * 720]);
			frame->cb.reset(new uint8_t[640 * 720]);
			frame->cr.reset(new uint8_t[640 * 720]);
			for (unsigned yy = 0; yy < 720; ++yy) {
				memcpy(frame->y.get() + 1280 * yy, y + 1280 * (719 - yy), 1280);
				memcpy(frame->cb.get() + 640 * yy, cb + 640 * (719 - yy), 640);
				memcpy(frame->cr.get() + 640 * yy, cr + 640 * (719 - yy), 640);
			}
			frame->is_semiplanar = false;
			frame->width = 1280;
			frame->height = 720;
			frame->chroma_subsampling_x = 2;
			frame->chroma_subsampling_y = 1;
			frame->pitch_y = 1280;
			frame->pitch_chroma = 640;
			JPEGFrameView::insert_interpolated_frame(qf.stream_idx, qf.output_pts, std::move(frame));

			// Now JPEG encode it, and send it on to the stream.
			vector<uint8_t> jpeg = encode_jpeg(y, cb, cr, 1280, 720);
			compute_flow->release_texture(qf.flow_tex);
			interpolate->release_texture(qf.output_tex);
			interpolate->release_texture(qf.cbcr_tex);

			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.stream_index = 0;
			pkt.data = (uint8_t *)jpeg.data();
			pkt.size = jpeg.size();
			stream_mux->add_packet(pkt, qf.output_pts, qf.output_pts);

			// Put the frame resources back.
			unique_lock<mutex> lock(queue_lock);
			interpolate_resources.push_back(qf.resources);
		}
	}
}

int VideoStream::write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
	VideoStream *video_stream = (VideoStream *)opaque;
	return video_stream->write_packet2(buf, buf_size, type, time);
}

int VideoStream::write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
	if (type == AVIO_DATA_MARKER_SYNC_POINT || type == AVIO_DATA_MARKER_BOUNDARY_POINT) {
		seen_sync_markers = true;
	} else if (type == AVIO_DATA_MARKER_UNKNOWN && !seen_sync_markers) {
		// We don't know if this is a keyframe or not (the muxer could
		// avoid marking it), so we just have to make the best of it.
		type = AVIO_DATA_MARKER_SYNC_POINT;
	}

	if (type == AVIO_DATA_MARKER_HEADER) {
		stream_mux_header.append((char *)buf, buf_size);
		global_httpd->set_header(stream_mux_header);
	} else {
		global_httpd->add_data((char *)buf, buf_size, type == AVIO_DATA_MARKER_SYNC_POINT, time, AVRational{ AV_TIME_BASE, 1 });
	}
	return buf_size;
}

