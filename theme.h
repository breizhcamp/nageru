#ifndef _THEME_H
#define _THEME_H 1

#include <lua.hpp>
#include <movit/flat_input.h>
#include <movit/ycbcr_input.h>
#include <stdbool.h>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "bmusb/bmusb.h"
#include "ref_counted_frame.h"
#include "tweaked_inputs.h"

class CEFCapture;
class FFmpegCapture;
class LiveInputWrapper;
struct InputState;

namespace movit {
class Effect;
class EffectChain;
class ResourcePool;
}  // namespace movit

class Theme {
public:
	Theme(const std::string &filename, const std::vector<std::string> &search_dirs, movit::ResourcePool *resource_pool, unsigned num_cards);
	~Theme();

	struct Chain {
		movit::EffectChain *chain;
		std::function<void()> setup_chain;

		// FRAME_HISTORY frames for each input, in order. Will contain duplicates
		// for non-interlaced inputs.
		std::vector<RefCountedFrame> input_frames;
	};

	Chain get_chain(unsigned num, float t, unsigned width, unsigned height, InputState input_state);

	int get_num_channels() const { return num_channels; }
	int map_signal(int signal_num);
	void set_signal_mapping(int signal_num, int card_num);
	std::string get_channel_name(unsigned channel);
	int get_channel_signal(unsigned channel);
	bool get_supports_set_wb(unsigned channel);
	void set_wb(unsigned channel, double r, double g, double b);
	std::string get_channel_color(unsigned channel);

	std::vector<std::string> get_transition_names(float t);

	void transition_clicked(int transition_num, float t);
	void channel_clicked(int preview_num);

	movit::ResourcePool *get_resource_pool() const { return resource_pool; }

	// Should be called as part of VideoInput.new() only.
	void register_video_input(FFmpegCapture *capture)
	{
		video_inputs.push_back(capture);
	}

	std::vector<FFmpegCapture *> get_video_inputs() const
	{
		return video_inputs;
	}

#ifdef HAVE_CEF
	// Should be called as part of HTMLInput.new() only.
	void register_html_input(CEFCapture *capture)
	{
		html_inputs.push_back(capture);
	}

	std::vector<CEFCapture *> get_html_inputs() const
	{
		return html_inputs;
	}
#endif

	void register_video_signal_connection(movit::EffectChain *chain, LiveInputWrapper *live_input, FFmpegCapture *capture)
	{
		video_signal_connections[chain].emplace_back(VideoSignalConnection { live_input, capture });
	}

#ifdef HAVE_CEF
	void register_html_signal_connection(movit::EffectChain *chain, LiveInputWrapper *live_input, CEFCapture *capture)
	{
		html_signal_connections[chain].emplace_back(CEFSignalConnection { live_input, capture });
	}
#endif

	struct MenuEntry {
		std::string text;
		int lua_ref;
	};
	std::vector<MenuEntry> get_theme_menu() { return theme_menu; }  // Can be empty for no menu.
	void theme_menu_entry_clicked(int lua_ref);

	// Will be invoked every time the theme sets a new menu.
	// Is not invoked for a menu that exists at the time of the callback.
	void set_theme_menu_callback(std::function<void()> callback)
	{
		theme_menu_callback = callback;
	}

private:
	void register_constants();
	void register_class(const char *class_name, const luaL_Reg *funcs);
	int set_theme_menu(lua_State *L);

	std::string theme_path;

	std::mutex m;
	lua_State *L;  // Protected by <m>.
	const InputState *input_state = nullptr;  // Protected by <m>. Only set temporarily, during chain setup.
	movit::ResourcePool *resource_pool;
	int num_channels;
	unsigned num_cards;

	std::mutex map_m;
	std::map<int, int> signal_to_card_mapping;  // Protected by <map_m>.

	std::vector<FFmpegCapture *> video_inputs;
	struct VideoSignalConnection {
		LiveInputWrapper *wrapper;
		FFmpegCapture *source;
	};
	std::unordered_map<movit::EffectChain *, std::vector<VideoSignalConnection>>
		 video_signal_connections;
#ifdef HAVE_CEF
	std::vector<CEFCapture *> html_inputs;
	struct CEFSignalConnection {
		LiveInputWrapper *wrapper;
		CEFCapture *source;
	};
	std::unordered_map<movit::EffectChain *, std::vector<CEFSignalConnection>>
		html_signal_connections;
#endif

	std::vector<MenuEntry> theme_menu;
	std::function<void()> theme_menu_callback;

	friend class LiveInputWrapper;
	friend int ThemeMenu_set(lua_State *L);
};

// LiveInputWrapper is a facade on top of an YCbCrInput, exposed to
// the Lua code. It contains a function (connect_signal()) intended
// to be called during chain setup, that picks out the current frame
// (in the form of a set of textures) from the input state given by
// the mixer, and communicates that state over to the actual YCbCrInput.
class LiveInputWrapper {
public:
	// Note: <override_bounce> is irrelevant for PixelFormat_8BitBGRA.
	LiveInputWrapper(Theme *theme, movit::EffectChain *chain, bmusb::PixelFormat pixel_format, bool override_bounce, bool deinterlace);

	void connect_signal(int signal_num);  // Must be called with the theme's <m> lock held, since it accesses theme->input_state.
	void connect_signal_raw(int signal_num, const InputState &input_state);
	movit::Effect *get_effect() const
	{
		if (deinterlace) {
			return deinterlace_effect;
		} else if (pixel_format == bmusb::PixelFormat_8BitBGRA) {
			return rgba_inputs[0];
		} else {
			return ycbcr_inputs[0];
		}
	}

private:
	Theme *theme;  // Not owned by us.
	bmusb::PixelFormat pixel_format;
	movit::YCbCrFormat input_ycbcr_format;
	std::vector<movit::YCbCrInput *> ycbcr_inputs;  // Multiple ones if deinterlacing. Owned by the chain.
	std::vector<movit::FlatInput *> rgba_inputs;  // Multiple ones if deinterlacing. Owned by the chain.
	movit::Effect *deinterlace_effect = nullptr;  // Owned by the chain.
	bool deinterlace;
};

#endif  // !defined(_THEME_H)
