#ifndef _HTTPD_H
#define _HTTPD_H

// A class dealing with stream output to HTTP.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <utility>

extern "C" {
#include <libavutil/rational.h>
}

struct MHD_Connection;
struct MHD_Daemon;

class HTTPD {
public:
	// Returns a pair of content and content-type.
	using EndpointCallback = std::function<std::pair<std::string, std::string>()>;

	HTTPD();
	~HTTPD();

	// Should be called before start().
	void set_header(const std::string &data)
	{
		header = data;
	}

	// Should be called before start() (due to threading issues).
	enum CORSPolicy {
		NO_CORS_POLICY,
		ALLOW_ALL_ORIGINS
	};
	void add_endpoint(const std::string &url, const EndpointCallback &callback, CORSPolicy cors_policy)
	{
		endpoints[url] = Endpoint{ callback, cors_policy };
	}

	void start(int port);
	void stop();
	void add_data(const char *buf, size_t size, bool keyframe, int64_t time, AVRational timebase);
	int64_t get_num_connected_clients() const
	{
		return metric_num_connected_clients.load();
	}

private:
	static int answer_to_connection_thunk(void *cls, MHD_Connection *connection,
	                                      const char *url, const char *method,
	                                      const char *version, const char *upload_data,
	                                      size_t *upload_data_size, void **con_cls);

	int answer_to_connection(MHD_Connection *connection,
	                         const char *url, const char *method,
	                         const char *version, const char *upload_data,
	                         size_t *upload_data_size, void **con_cls);

	static void free_stream(void *cls);

	class Stream {
	public:
		enum Framing {
			FRAMING_RAW,
			FRAMING_METACUBE
		};
		Stream(HTTPD *parent, Framing framing)
			: parent(parent), framing(framing) {}

		static ssize_t reader_callback_thunk(void *cls, uint64_t pos, char *buf, size_t max);
		ssize_t reader_callback(uint64_t pos, char *buf, size_t max);

		enum DataType {
			DATA_TYPE_HEADER,
			DATA_TYPE_KEYFRAME,
			DATA_TYPE_OTHER
		};
		void add_data(const char *buf, size_t size, DataType data_type, int64_t time, AVRational timebase);
		void stop();
		HTTPD *get_parent() const { return parent; }

	private:
		HTTPD *parent;
		Framing framing;

		std::mutex buffer_mutex;
		bool should_quit = false;  // Under <buffer_mutex>.
		std::condition_variable has_buffered_data;
		std::deque<std::string> buffered_data;  // Protected by <buffer_mutex>.
		size_t used_of_buffered_data = 0;  // How many bytes of the first element of <buffered_data> that is already used. Protected by <mutex>.
		size_t seen_keyframe = false;
	};

	MHD_Daemon *mhd = nullptr;
	std::mutex streams_mutex;
	std::set<Stream *> streams;  // Not owned.
	struct Endpoint {
		EndpointCallback callback;
		CORSPolicy cors_policy;
	};
	std::unordered_map<std::string, Endpoint> endpoints;
	std::string header;

	// Metrics.
	std::atomic<int64_t> metric_num_connected_clients{ 0 };
};

#endif  // !defined(_HTTPD_H)
