-- The theme is what decides what's actually shown on screen, what kind of
-- transitions are available (if any), and what kind of inputs there are,
-- if any. In general, it drives the entire display logic by creating Movit
-- chains, setting their parameters and then deciding which to show when.
--
-- Themes are written in Lua, which reflects a simplified form of the Movit API
-- where all the low-level details (such as texture formats) are handled by the
-- C++ side and you generally just build chains.
io.write("hello from lua\n");

-- A chain to show input 0 on screen.
local input0_chain = EffectChain.new(16, 9);
--input0_chain:add_input(Inputs.create(0));  -- TODO: We probably want something more fluid.
local preview_input0 = input0_chain:add_live_input();
preview_input0:connect_signal(0);  -- First input card. Can be changed whenever you want.
input0_chain:finalize(false);

-- The main live chain. Currently just about input 0 with some color correction.
local main_chain = EffectChain.new(16, 9);
-- local input0 = main_chain:add_input(Inputs.create(0));
local input0 = main_chain:add_live_input();
input0:connect_signal(0);
local wb_effect = main_chain:add_effect(WhiteBalanceEffect.new(), input0);
wb_effect:set_float("output_color_temperature", 1234.0);
main_chain:finalize(true);
-- local input1 = main_chain.add_input(Inputs.create(1));
-- local resample_effect = main_chain.add_effect(ResampleEffect.new(), input0);
-- local padding_effect = main_chain.add_effect(IntegralPaddingEffect.new());

-- Returns the number of outputs in addition to the live (0) and preview (1).
-- Called only once, at the start of the program.
function num_channels()
	return 0;
end

-- Called every frame.
function get_transitions()
	return {"Cut", "Fade", "Zoom!"};
end

function transition_clicked(num, t)
	-- Presumably do some sort of transition here.
	io.write("STUB: transition_clicked\n");
end

function channel_clicked(num, t)
	-- Presumably change the preview here.
	io.write("STUB: channel_clicked\n");
end

-- Called every frame. Get the chain for displaying at input <num>,
-- where 0 is live, 1 is preview, 2 is the first channel to display
-- in the bottom bar, and so on up to num_channels()+1. t is the 
-- current time in seconds. width and height are the dimensions of
-- the output, although you can ignore them if you don't need them
-- (they're useful if you want to e.g. know what to resample by).
-- If you want to change any parameters in the chain, this is also
-- the right place.
--
-- NOTE: The chain returned must be finalized with the Y'CbCr flag
-- if and only if num==0.
function get_chain(num, t, width, height)
	if num == 0 then
		prepare = function()
	--		io.write("prepare is being called back\n");
			input0:connect_signal(1);
			wb_effect:set_float("output_color_temperature", 3500.0 + t * 100.0);
		end
		return main_chain, prepare;
	end
	if num == 1 then
		prepare = function()
		end
		return input0_chain, prepare;
	end
end

