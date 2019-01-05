#include "midi_device.h"

#include <alsa/asoundlib.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <thread>
#include <utility>

using namespace std;

MIDIDevice::MIDIDevice(MIDIReceiver *receiver)
	: receiver(receiver)
{
	should_quit_fd = eventfd(/*initval=*/0, /*flags=*/0);
	assert(should_quit_fd != -1);
}

MIDIDevice::~MIDIDevice()
{
	should_quit = true;
	const uint64_t one = 1;
	if (write(should_quit_fd, &one, sizeof(one)) != sizeof(one)) {
		perror("write(should_quit_fd)");
		exit(1);
	}
	midi_thread.join();
	close(should_quit_fd);
}

void MIDIDevice::start_thread()
{
	midi_thread = thread(&MIDIDevice::thread_func, this);
}

#define RETURN_ON_ERROR(msg, expr) do {                            \
	int err = (expr);                                          \
	if (err < 0) {                                             \
		fprintf(stderr, msg ": %s\n", snd_strerror(err));  \
		return;                                            \
	}                                                          \
} while (false)

#define WARN_ON_ERROR(msg, expr) do {                              \
	int err = (expr);                                          \
	if (err < 0) {                                             \
		fprintf(stderr, msg ": %s\n", snd_strerror(err));  \
	}                                                          \
} while (false)


void MIDIDevice::thread_func()
{
	pthread_setname_np(pthread_self(), "MIDIDevice");

	snd_seq_t *seq;
	int err;

	RETURN_ON_ERROR("snd_seq_open", snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0));
	RETURN_ON_ERROR("snd_seq_nonblock", snd_seq_nonblock(seq, 1));
	RETURN_ON_ERROR("snd_seq_client_name", snd_seq_set_client_name(seq, "nageru"));
	RETURN_ON_ERROR("snd_seq_create_simple_port",
		snd_seq_create_simple_port(seq, "nageru",
			SND_SEQ_PORT_CAP_READ |
				SND_SEQ_PORT_CAP_SUBS_READ |
				SND_SEQ_PORT_CAP_WRITE |
				SND_SEQ_PORT_CAP_SUBS_WRITE,
			SND_SEQ_PORT_TYPE_MIDI_GENERIC |
				SND_SEQ_PORT_TYPE_APPLICATION));

	int queue_id = snd_seq_alloc_queue(seq);
	RETURN_ON_ERROR("snd_seq_create_queue", queue_id);
	RETURN_ON_ERROR("snd_seq_start_queue", snd_seq_start_queue(seq, queue_id, nullptr));

	// The sequencer object is now ready to be used from other threads.
	{
		lock_guard<mutex> lock(mu);
		alsa_seq = seq;
		alsa_queue_id = queue_id;
	}

	// Listen to the announce port (0:1), which will tell us about new ports.
	RETURN_ON_ERROR("snd_seq_connect_from", snd_seq_connect_from(seq, 0, /*client=*/0, /*port=*/1));

	// Now go through all ports and subscribe to them.
	snd_seq_client_info_t *cinfo;
	snd_seq_client_info_alloca(&cinfo);

	snd_seq_client_info_set_client(cinfo, -1);
	while (snd_seq_query_next_client(seq, cinfo) >= 0) {
		int client = snd_seq_client_info_get_client(cinfo);

		snd_seq_port_info_t *pinfo;
		snd_seq_port_info_alloca(&pinfo);

		snd_seq_port_info_set_client(pinfo, client);
		snd_seq_port_info_set_port(pinfo, -1);
		while (snd_seq_query_next_port(seq, pinfo) >= 0) {
			constexpr int mask = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
			if ((snd_seq_port_info_get_capability(pinfo) & mask) == mask) {
				lock_guard<mutex> lock(mu);
				subscribe_to_port_lock_held(seq, *snd_seq_port_info_get_addr(pinfo));
			}
		}
	}

	int num_alsa_fds = snd_seq_poll_descriptors_count(seq, POLLIN);
	unique_ptr<pollfd[]> fds(new pollfd[num_alsa_fds + 1]);

	while (!should_quit) {
		snd_seq_poll_descriptors(seq, fds.get(), num_alsa_fds, POLLIN);
		fds[num_alsa_fds].fd = should_quit_fd;
		fds[num_alsa_fds].events = POLLIN;
		fds[num_alsa_fds].revents = 0;

		err = poll(fds.get(), num_alsa_fds + 1, -1);
		if (err == 0 || (err == -1 && errno == EINTR)) {
			continue;
		}
		if (err == -1) {
			perror("poll");
			break;
		}
		if (fds[num_alsa_fds].revents) {
			// Activity on should_quit_fd.
			break;
		}

		// Seemingly we can get multiple events in a single poll,
		// and if we don't handle them all, poll will _not_ alert us!
		while (!should_quit) {
			snd_seq_event_t *event;
			err = snd_seq_event_input(seq, &event);
			if (err < 0) {
				if (err == -EINTR) continue;
				if (err == -EAGAIN) break;
				if (err == -ENOSPC) {
					fprintf(stderr, "snd_seq_event_input: Some events were lost.\n");
					continue;
				}
				fprintf(stderr, "snd_seq_event_input: %s\n", snd_strerror(err));
				return;
			}
			if (event) {
				handle_event(seq, event);
			}
		}
	}
}

void MIDIDevice::handle_event(snd_seq_t *seq, snd_seq_event_t *event)
{
	if (event->source.client == snd_seq_client_id(seq)) {
		// Ignore events we sent out ourselves.
		return;
	}

	lock_guard<mutex> lock(mu);
	switch (event->type) {
	case SND_SEQ_EVENT_CONTROLLER: {
		receiver->controller_received(event->data.control.param, event->data.control.value);
		break;
	}
	case SND_SEQ_EVENT_NOTEON: {
		receiver->note_on_received(event->data.note.note);
		break;
	}
	case SND_SEQ_EVENT_PORT_START:
		subscribe_to_port_lock_held(seq, event->data.addr);
		break;
	case SND_SEQ_EVENT_PORT_EXIT:
		printf("MIDI port %d:%d went away.\n", event->data.addr.client, event->data.addr.port);
		break;
	case SND_SEQ_EVENT_PORT_SUBSCRIBED:
		if (event->data.connect.sender.client != 0 &&  // Ignore system senders.
		    event->data.connect.sender.client != snd_seq_client_id(seq) &&
		    event->data.connect.dest.client == snd_seq_client_id(seq)) {
			receiver->update_num_subscribers(++num_subscribed_ports);
		}
		break;
	case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
		if (event->data.connect.sender.client != 0 &&  // Ignore system senders.
		    event->data.connect.sender.client != snd_seq_client_id(seq) &&
		    event->data.connect.dest.client == snd_seq_client_id(seq)) {
			receiver->update_num_subscribers(--num_subscribed_ports);
		}
		break;
	case SND_SEQ_EVENT_NOTEOFF:
	case SND_SEQ_EVENT_CLIENT_START:
	case SND_SEQ_EVENT_CLIENT_EXIT:
	case SND_SEQ_EVENT_CLIENT_CHANGE:
	case SND_SEQ_EVENT_PORT_CHANGE:
		break;
	default:
		printf("Ignoring MIDI event of unknown type %d.\n", event->type);
	}
}

void MIDIDevice::subscribe_to_port_lock_held(snd_seq_t *seq, const snd_seq_addr_t &addr)
{
	// Client 0 (SNDRV_SEQ_CLIENT_SYSTEM) is basically the system; ignore it.
	// MIDI through (SNDRV_SEQ_CLIENT_DUMMY) echoes back what we give it, so ignore that, too.
	if (addr.client == 0 || addr.client == 14) {
		return;
	}

	// Don't listen to ourselves.
	if (addr.client == snd_seq_client_id(seq)) {
		return;
	}

	int err = snd_seq_connect_from(seq, 0, addr.client, addr.port);
	if (err < 0) {
		// Just print out a warning (i.e., don't die); it could
		// very well just be e.g. another application.
		printf("Couldn't subscribe to MIDI port %d:%d (%s).\n",
			addr.client, addr.port, snd_strerror(err));
	} else {
		printf("Subscribed to MIDI port %d:%d.\n", addr.client, addr.port);
	}

	// For sending data back.
	err = snd_seq_connect_to(seq, 0, addr.client, addr.port);
	if (err < 0) {
		printf("Couldn't subscribe MIDI port %d:%d (%s) to us.\n",
			addr.client, addr.port, snd_strerror(err));
	} else {
		printf("Subscribed MIDI port %d:%d to us.\n", addr.client, addr.port);
	}

	// The current status of the device is unknown, so refresh it.
	set<unsigned> active_lights;
	for (const auto &it : current_light_status) {
		if (it.second) {
			active_lights.insert(it.first);
		}
	}
	current_light_status.clear();
	update_lights_lock_held(active_lights);
}

void MIDIDevice::update_lights_lock_held(const set<unsigned> &active_lights)
{
	if (alsa_seq == nullptr) {
		return;
	}

	unsigned num_events = 0;
	for (unsigned note_num = 1; note_num <= 127; ++note_num) {
		bool active = active_lights.count(note_num);
		if (current_light_status.count(note_num) &&
		    current_light_status[note_num] == active) {
			// Already known to be in the desired state.
			continue;
		}

		snd_seq_event_t ev;
		snd_seq_ev_clear(&ev);

		// Some devices drop events if we throw them onto them
		// too quickly. Add a 1 ms delay for each.
		snd_seq_real_time_t tm{0, num_events++ * 1000000};
		snd_seq_ev_schedule_real(&ev, alsa_queue_id, true, &tm);
		snd_seq_ev_set_source(&ev, 0);
		snd_seq_ev_set_subs(&ev);

		// For some reason, not all devices respond to note off.
		// Use note-on with velocity of 0 (which is equivalent) instead.
		snd_seq_ev_set_noteon(&ev, /*channel=*/0, note_num, active ? 1 : 0);
		WARN_ON_ERROR("snd_seq_event_output", snd_seq_event_output(alsa_seq, &ev));
		current_light_status[note_num] = active;
	}
	WARN_ON_ERROR("snd_seq_drain_output", snd_seq_drain_output(alsa_seq));
}
