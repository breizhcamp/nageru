#include "print_latency.h"
#include "flags.h"

#include <stdio.h>
#include <chrono>
#include <string>

using namespace std;
using namespace std::chrono;

void print_latency(const string &header, const ReceivedTimestamps &received_ts, bool is_b_frame, int *frameno)
{
	if (received_ts.max_ts == steady_clock::time_point::min())
		return;

	// 101 is chosen so that it's prime, which is unlikely to get the same frame type every time.
	if (global_flags.print_video_latency && (++*frameno % 101) == 0) {
		const steady_clock::time_point now = steady_clock::now();
		printf("%-60s %4.0f ms (lowest-latency input), %4.0f ms (highest-latency input)",
			header.c_str(),
			1e3 * std::chrono::duration<double>(now - received_ts.max_ts).count(),
			1e3 * std::chrono::duration<double>(now - received_ts.min_ts).count());

		if (is_b_frame) {
			printf("  [on B-frame; potential extra latency]\n");
		} else {
			printf("\n");
		}
	}
}