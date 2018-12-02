#include "httpd.h"

#include <assert.h>
#include <byteswap.h>
#include <endian.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <memory>
extern "C" {
#include <libavutil/avutil.h>
}

#include "defs.h"
#include "shared/metacube2.h"
#include "shared/metrics.h"

struct MHD_Connection;
struct MHD_Response;

using namespace std;

HTTPD::HTTPD()
{
	global_metrics.add("num_connected_clients", &metric_num_connected_clients, Metrics::TYPE_GAUGE);
}

HTTPD::~HTTPD()
{
	stop();
}

void HTTPD::start(int port)
{
	mhd = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION | MHD_USE_POLL_INTERNALLY | MHD_USE_DUAL_STACK,
	                       port,
	                       nullptr, nullptr,
	                       &answer_to_connection_thunk, this,
	                       MHD_OPTION_NOTIFY_COMPLETED, nullptr, this,
	                       MHD_OPTION_END);
	if (mhd == nullptr) {
		fprintf(stderr, "Warning: Could not open HTTP server. (Port already in use?)\n");
	}
}

void HTTPD::stop()
{
	if (mhd) {
		MHD_quiesce_daemon(mhd);
		for (Stream *stream : streams) {
			stream->stop();
		}
		MHD_stop_daemon(mhd);
		mhd = nullptr;
	}
}

void HTTPD::add_data(const char *buf, size_t size, bool keyframe, int64_t time, AVRational timebase)
{
	unique_lock<mutex> lock(streams_mutex);
	for (Stream *stream : streams) {
		stream->add_data(buf, size, keyframe ? Stream::DATA_TYPE_KEYFRAME : Stream::DATA_TYPE_OTHER, time, timebase);
	}
}

int HTTPD::answer_to_connection_thunk(void *cls, MHD_Connection *connection,
                                      const char *url, const char *method,
                                      const char *version, const char *upload_data,
                                      size_t *upload_data_size, void **con_cls)
{
	HTTPD *httpd = (HTTPD *)cls;
	return httpd->answer_to_connection(connection, url, method, version, upload_data, upload_data_size, con_cls);
}

int HTTPD::answer_to_connection(MHD_Connection *connection,
                                const char *url, const char *method,
				const char *version, const char *upload_data,
				size_t *upload_data_size, void **con_cls)
{
	// See if the URL ends in “.metacube”.
	HTTPD::Stream::Framing framing;
	if (strstr(url, ".metacube") == url + strlen(url) - strlen(".metacube")) {
		framing = HTTPD::Stream::FRAMING_METACUBE;
	} else {
		framing = HTTPD::Stream::FRAMING_RAW;
	}

	if (strcmp(url, "/metrics") == 0) {
		string contents = global_metrics.serialize();
		MHD_Response *response = MHD_create_response_from_buffer(
			contents.size(), &contents[0], MHD_RESPMEM_MUST_COPY);
		MHD_add_response_header(response, "Content-type", "text/plain");
		int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
		MHD_destroy_response(response);  // Only decreases the refcount; actual free is after the request is done.
		return ret;
	}
	if (endpoints.count(url)) {
		pair<string, string> contents_and_type = endpoints[url].callback();
		MHD_Response *response = MHD_create_response_from_buffer(
			contents_and_type.first.size(), &contents_and_type.first[0], MHD_RESPMEM_MUST_COPY);
		MHD_add_response_header(response, "Content-type", contents_and_type.second.c_str());
		if (endpoints[url].cors_policy == ALLOW_ALL_ORIGINS) {
			MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
		}
		int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
		MHD_destroy_response(response);  // Only decreases the refcount; actual free is after the request is done.
		return ret;
	}

	// Small hack; reject unknown /channels/foo.
	if (string(url).find("/channels/") == 0) {
		string contents = "Not found.";
		MHD_Response *response = MHD_create_response_from_buffer(
			contents.size(), &contents[0], MHD_RESPMEM_MUST_COPY);
		MHD_add_response_header(response, "Content-type", "text/plain");
		int ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
		MHD_destroy_response(response);  // Only decreases the refcount; actual free is after the request is done.
		return ret;
	}

	HTTPD::Stream *stream = new HTTPD::Stream(this, framing);
	stream->add_data(header.data(), header.size(), Stream::DATA_TYPE_HEADER, AV_NOPTS_VALUE, AVRational{ 1, 0 });
	{
		unique_lock<mutex> lock(streams_mutex);
		streams.insert(stream);
	}
	++metric_num_connected_clients;
	*con_cls = stream;

	// Does not strictly have to be equal to MUX_BUFFER_SIZE.
	MHD_Response *response = MHD_create_response_from_callback(
		(size_t)-1, MUX_BUFFER_SIZE, &HTTPD::Stream::reader_callback_thunk, stream, &HTTPD::free_stream);
	// TODO: Content-type?
	if (framing == HTTPD::Stream::FRAMING_METACUBE) {
		MHD_add_response_header(response, "Content-encoding", "metacube");
	}

	int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);  // Only decreases the refcount; actual free is after the request is done.

	return ret;
}

void HTTPD::free_stream(void *cls)
{
	HTTPD::Stream *stream = (HTTPD::Stream *)cls;
	HTTPD *httpd = stream->get_parent();
	{
		unique_lock<mutex> lock(httpd->streams_mutex);
		delete stream;
		httpd->streams.erase(stream);
	}
	--httpd->metric_num_connected_clients;
}

ssize_t HTTPD::Stream::reader_callback_thunk(void *cls, uint64_t pos, char *buf, size_t max)
{
	HTTPD::Stream *stream = (HTTPD::Stream *)cls;
	return stream->reader_callback(pos, buf, max);
}

ssize_t HTTPD::Stream::reader_callback(uint64_t pos, char *buf, size_t max)
{
	unique_lock<mutex> lock(buffer_mutex);
	has_buffered_data.wait(lock, [this]{ return should_quit || !buffered_data.empty(); });
	if (should_quit) {
		return 0;
	}

	ssize_t ret = 0;
	while (max > 0 && !buffered_data.empty()) {
		const string &s = buffered_data.front();
		assert(s.size() > used_of_buffered_data);
		size_t len = s.size() - used_of_buffered_data;
		if (max >= len) {
			// Consume the entire (rest of the) string.
			memcpy(buf, s.data() + used_of_buffered_data, len);
			buf += len;
			ret += len;
			max -= len;
			buffered_data.pop_front();
			used_of_buffered_data = 0;
		} else {
			// We don't need the entire string; just use the first part of it.
			memcpy(buf, s.data() + used_of_buffered_data, max);
			buf += max;
			used_of_buffered_data += max;
			ret += max;
			max = 0;
		}
	}

	return ret;
}

void HTTPD::Stream::add_data(const char *buf, size_t buf_size, HTTPD::Stream::DataType data_type, int64_t time, AVRational timebase)
{
	if (buf_size == 0) {
		return;
	}
	if (data_type == DATA_TYPE_KEYFRAME) {
		seen_keyframe = true;
	} else if (data_type == DATA_TYPE_OTHER && !seen_keyframe) {
		// Start sending only once we see a keyframe.
		return;
	}

	unique_lock<mutex> lock(buffer_mutex);

	if (framing == FRAMING_METACUBE) {
		int flags = 0;
		if (data_type == DATA_TYPE_HEADER) {
			flags |= METACUBE_FLAGS_HEADER;
		} else if (data_type == DATA_TYPE_OTHER) {
			flags |= METACUBE_FLAGS_NOT_SUITABLE_FOR_STREAM_START;
		}

		// If we're about to send a keyframe, send a pts metadata block
		// to mark its time.
		if ((flags & METACUBE_FLAGS_NOT_SUITABLE_FOR_STREAM_START) == 0 && time != AV_NOPTS_VALUE) {
			metacube2_pts_packet packet;
			packet.type = htobe64(METACUBE_METADATA_TYPE_NEXT_BLOCK_PTS);
			packet.pts = htobe64(time);
			packet.timebase_num = htobe64(timebase.num);
			packet.timebase_den = htobe64(timebase.den);

			metacube2_block_header hdr;
			memcpy(hdr.sync, METACUBE2_SYNC, sizeof(hdr.sync));
			hdr.size = htonl(sizeof(packet));
			hdr.flags = htons(METACUBE_FLAGS_METADATA);
			hdr.csum = htons(metacube2_compute_crc(&hdr));
			buffered_data.emplace_back((char *)&hdr, sizeof(hdr));
			buffered_data.emplace_back((char *)&packet, sizeof(packet));
		}

		metacube2_block_header hdr;
		memcpy(hdr.sync, METACUBE2_SYNC, sizeof(hdr.sync));
		hdr.size = htonl(buf_size);
		hdr.flags = htons(flags);
		hdr.csum = htons(metacube2_compute_crc(&hdr));
		buffered_data.emplace_back((char *)&hdr, sizeof(hdr));
	}
	buffered_data.emplace_back(buf, buf_size);

	// Send a Metacube2 timestamp every keyframe.
	if (framing == FRAMING_METACUBE && data_type == DATA_TYPE_KEYFRAME) {
		timespec now;
		clock_gettime(CLOCK_REALTIME, &now);

		metacube2_timestamp_packet packet;
		packet.type = htobe64(METACUBE_METADATA_TYPE_ENCODER_TIMESTAMP);
		packet.tv_sec = htobe64(now.tv_sec);
		packet.tv_nsec = htobe64(now.tv_nsec);

		metacube2_block_header hdr;
		memcpy(hdr.sync, METACUBE2_SYNC, sizeof(hdr.sync));
		hdr.size = htonl(sizeof(packet));
		hdr.flags = htons(METACUBE_FLAGS_METADATA);
		hdr.csum = htons(metacube2_compute_crc(&hdr));
		buffered_data.emplace_back((char *)&hdr, sizeof(hdr));
		buffered_data.emplace_back((char *)&packet, sizeof(packet));
	}

	has_buffered_data.notify_all();	
}

void HTTPD::Stream::stop()
{
	unique_lock<mutex> lock(buffer_mutex);
	should_quit = true;
	has_buffered_data.notify_all();
}
