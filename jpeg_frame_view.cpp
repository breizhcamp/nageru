#include "jpeg_frame_view.h"

#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <QGraphicsPixmapItem>
#include <QPixmap>

#include "defs.h"
#include "post_to_main_thread.h"

using namespace std;

string filename_for_frame(unsigned stream_idx, int64_t pts);

struct JPEGID {
	unsigned stream_idx;
	int64_t pts;
};
bool operator< (const JPEGID &a, const JPEGID &b) {
	return make_pair(a.stream_idx, a.pts) < make_pair(b.stream_idx, b.pts);
}

struct LRUPixmap {
	shared_ptr<QPixmap> pixmap;
	size_t last_used;
};

mutex cache_mu;
map<JPEGID, LRUPixmap> cache;  // Under cache_mu.
condition_variable any_pending_decodes;
deque<pair<JPEGID, JPEGFrameView *>> pending_decodes;  // Under cache_mu.
atomic<size_t> event_counter{0};

void prune_cache()
{
	// Assumes cache_mu is held.
	vector<size_t> lru_timestamps;
	for (const auto &key_and_value : cache) {
		lru_timestamps.push_back(key_and_value.second.last_used);
	}

	size_t cutoff_point = CACHE_SIZE / 10;  // Prune away the 10% oldest ones.
	nth_element(lru_timestamps.begin(), lru_timestamps.begin() + cutoff_point, lru_timestamps.end());
	size_t must_be_used_after = lru_timestamps[cutoff_point];
	for (auto it = cache.begin(); it != cache.end(); ) {
		if (it->second.last_used < must_be_used_after) {
			it = cache.erase(it);
		} else {
			++it;
		}
	}
}

void jpeg_decoder_thread()
{
	pthread_setname_np(pthread_self(), "JPEGDecoder");
	for ( ;; ) {
		JPEGID id;
		JPEGFrameView *dest;
		shared_ptr<QPixmap> pixmap;
		{
			unique_lock<mutex> lock(cache_mu);
			any_pending_decodes.wait(lock, [] {
				return !pending_decodes.empty();
			});
			id = pending_decodes.front().first;
			dest = pending_decodes.front().second;
			pending_decodes.pop_front();

			auto it = cache.find(id);
			if (it != cache.end()) {
				pixmap = it->second.pixmap;
				it->second.last_used = event_counter++;
			}
		}

		if (pixmap == nullptr) {
			// Not found in the cache, so we need to do a decode or drop the request.
			// Prune the queue if there are too many pending for this destination.
			// TODO: Could we get starvation here?
			size_t num_pending = 0;
			for (const pair<JPEGID, JPEGFrameView *> &decode : pending_decodes) {
				if (decode.second == dest) {
					++num_pending;
				}
			}
			if (num_pending > 3) {
				continue;
			}

			pixmap.reset(
				new QPixmap(QString::fromStdString(filename_for_frame(id.stream_idx, id.pts))));

			unique_lock<mutex> lock(cache_mu);
			cache[id] = LRUPixmap{ pixmap, event_counter++ };

			if (cache.size() > CACHE_SIZE) {
				prune_cache();
			}
		}

		dest->setPixmap(pixmap);
	}
}

JPEGFrameView::JPEGFrameView(QWidget *parent)
	: QGraphicsView(parent) {
	scene.addItem(&item);
	setScene(&scene);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	static once_flag once;
	call_once(once, [] {
		std::thread(&jpeg_decoder_thread).detach();
	});
}

void JPEGFrameView::update_frame()
{
	unique_lock<mutex> lock(cache_mu);
	pending_decodes.emplace_back(JPEGID{ stream_idx, pts }, this);
	any_pending_decodes.notify_all();
}

void JPEGFrameView::resizeEvent(QResizeEvent *event)
{
	fitInView(&item, Qt::KeepAspectRatio);
}

void JPEGFrameView::setPixmap(std::shared_ptr<QPixmap> pixmap)
{
	post_to_main_thread([this, pixmap] {
		item.setPixmap(*pixmap);
		fitInView(&item, Qt::KeepAspectRatio);
	});
}
