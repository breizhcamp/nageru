#ifndef _QUEUE_SPOT_HOLDER
#define _QUEUE_SPOT_HOLDER 1

// A RAII class to hold a shared resource, in our case an (unordered!) spot in a queue,
// for as long as a frame is under computation.

class QueueInterface {
public:
	virtual ~QueueInterface() {}
	virtual void take_queue_spot() = 0;
	virtual void release_queue_spot() = 0;
};

class QueueSpotHolder {
public:
	QueueSpotHolder() : queue(nullptr) {}

	explicit QueueSpotHolder(QueueInterface *queue) : queue(queue) {
		queue->take_queue_spot();
	}

	QueueSpotHolder(QueueSpotHolder &&other) : queue(other.queue) {
		other.queue = nullptr;
	}

	QueueSpotHolder &operator=(QueueSpotHolder &&other) {
		queue = other.queue;
		other.queue = nullptr;
		return *this;
	}

	~QueueSpotHolder() {
		if (queue != nullptr) {
			queue->release_queue_spot();
		}
	}

	// Movable only.
	QueueSpotHolder(QueueSpotHolder &) = delete;
	QueueSpotHolder &operator=(QueueSpotHolder &) = delete;

private:
	QueueInterface *queue;	
};

#endif // !defined(_QUEUE_SPOT_HOLDER)
