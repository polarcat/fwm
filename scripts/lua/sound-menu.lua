local sound = {}

local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")
local control = sys.get_shmdir() .. "/docks/sound-dock"

--------------------------------------------------------------------------------
--
-- Local functions.
--
--------------------------------------------------------------------------------

local function get_volume()
	local cmd = "wpctl get-volume @DEFAULT_AUDIO_SINK@"
	local pipe = assert(io.popen(cmd, "r"))

	local str = pipe:read("*a")
	local res = pipe:close()

	if not res then
		print("[sound] wpctl failed")
		return 0, ""
	end

	local vol = str:match("Volume:%s*([%d%.]+)")
	if not vol then
		print(("[sound] could not parse wpctl output: %q"):format(str))
		vol = "0"
	end
	vol = tonumber(vol)

	local icon = ""
	local muted = ""
	if str:match("%[MUTED%]") then
		muted = "muted"
		icon = ""
	else
		muted = "un-muted"
		icon = ""
	end

	return string.format("%d", vol * 100), muted, icon
end

local function set_volume(vol, icon)
	sys.setprop(control, icon .. " 0xa0a0a0")
	local cmd = "wpctl set-volume @DEFAULT_AUDIO_SINK@ " .. vol .. "%"
	print("CMD=" .. cmd)
	sys.spawn("wpctl set-volume @DEFAULT_AUDIO_SINK@ " .. vol .. "%")
end

local function set_mute(muted, icon)
	local color
	if muted == 0 then
		color = sys.get_color("normal-fg")
	else
		color = sys.get_color_dim("normal-fg")
	end

	sys.setprop(control, icon .. " " .. color)
	sys.spawn("wpctl set-mute @DEFAULT_AUDIO_SINK@ " .. muted)
end

local function show_menu(menu)
	local off_icon = ""
	local med_icon = ""
	local max_icon = ""

	local cur, muted, icon = get_volume()
	local menu_items = {
		"\a\t" .. cur .. "%\n",
		"\a" .. icon .. "\t" .. muted .. "\n",
		"\a" .. med_icon .. "\tlow\n",
		"\a" .. med_icon .. "\tmed\n",
		"\a" .. max_icon .. "\thigh\n",
		"\a" .. max_icon .. "\tmax\n",
	}

	local selection, input = menu.run(sys.config, menu_items)
	if not selection then
		return
	end

	local fields = {}
	for field in selection:gmatch("[^\t\n]+") do
		fields[#fields + 1] = field
	end

	local str
	if not fields[2] and not input then
		return
	elseif fields[2]:match("%%$") and input then
		str = input
	else
		str = fields[2]
	end

	if str:match("%%$") then
		return -- nothing changed
	elseif str == "muted" then
		set_mute(0, max_icon)
	elseif str == "un-muted" then
		set_mute(1, max_icon)
	elseif str == "low" then
		set_volume(25, med_icon)
	elseif str == "med" then
		set_volume(50, med_icon)
	elseif str == "high" then
		set_volume(75, max_icon)
	elseif str == "max" then
		set_volume(100, max_icon)
	else
		local vol = tonumber(str)
		if vol > 0 and vol < 25 then
			set_volume(vol, med_icon)
		elseif vol >=25 and vol < 50 then
			set_volume(vol, med_icon)
		elseif vol >=50 and vol < 75 then
			set_volume(vol, max_icon)
		else
			set_volume(vol, max_icon)
		end
	end
end

--------------------------------------------------------------------------------
--
-- Public API.
--
--------------------------------------------------------------------------------

-- As a submenu.
function sound.start_menu(menu)
	show_menu(menu)
end

-- As a standalone menu.
function start()
	show_menu(sys.load_library("menulib-lua"))
end

return sound
