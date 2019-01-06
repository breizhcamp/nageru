#ifndef _MIDI_DEVICE_H
#define _MIDI_DEVICE_H 1

// MIDIDevice is a class that pools incoming MIDI messages from
// all MIDI devices in the system, decodes them and sends them on.

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <thread>

typedef struct snd_seq_addr snd_seq_addr_t;
typedef struct snd_seq_event snd_seq_event_t;
typedef struct _snd_seq snd_seq_t;

class MIDIReceiver {
public:
	virtual ~MIDIReceiver() {}
	virtual void controller_received(int controller, int value) = 0;
	virtual void note_on_received(int note) = 0;
	virtual void update_num_subscribers(unsigned num_subscribers) = 0;
};

class MIDIDevice {
public:
	MIDIDevice(MIDIReceiver *receiver);
	~MIDIDevice();
	void start_thread();

	void update_lights(const std::set<unsigned> &active_lights)
	{
		std::lock_guard<std::mutex> lock(mu);
		update_lights_lock_held(active_lights);
	}

private:
	void thread_func();
	void handle_event(snd_seq_t *seq, snd_seq_event_t *event);
	void subscribe_to_port_lock_held(snd_seq_t *seq, const snd_seq_addr_t &addr);
	void update_lights_lock_held(const std::set<unsigned> &active_lights);

	std::atomic<bool> should_quit{false};
	int should_quit_fd;

	mutable std::mutex mu;
	MIDIReceiver *receiver;  // Under <mu>.

	std::thread midi_thread;
	std::map<unsigned, bool> current_light_status;  // Keyed by note number. Under <mu>.
	snd_seq_t *alsa_seq{nullptr};  // Under <mu>.
	int alsa_queue_id{-1};  // Under <mu>.
	std::atomic<int> num_subscribed_ports{0};
};

#endif  // !defined(_MIDI_DEVICE_H)
