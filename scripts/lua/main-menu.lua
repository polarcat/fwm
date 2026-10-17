local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")

local wlan = sys.load_script("wlan-menu")
local lan = sys.load_script("lan-menu")
local suspend = sys.load_script("suspend-menu")
local tags = sys.load_script("tags-menu")
local display = sys.load_script("display-menu")
local sound = sys.load_script("sound-menu")
local menu = sys.load_library("menulib-lua")

local main_menu = {
	"\a\t\n",
	"\n",
	"\a\tterminal\n",
	"\a\tfirefox\n",
	"\a\ttags\n",
	"\a\twifi\n",
	"\a\tethernet\n",
	"\a\tsound\n",
	"\a\tdisplay\n",
	"\n",
	"\a\tclipboard\n",
	"\n",
	"\a\tlock\n",
	"\a\tsuspend\n",
	"\a\texit\n",
	"\n",
}

local function show_main_menu()
	local text = menu.run(sys.config, main_menu)
	if not text then
		return
	end

	local fields = {}
	for field in text:gmatch("[^\t\n]+") do
		fields[#fields + 1] = field
	end

	if not fields then
		return
	end

	if fields[2] == "terminal" then
		sys.spawn("xterm")
	elseif fields[2] == "firefox" then
		sys.spawn("dbus-run-session -- firefox")
	elseif fields[2] == "tags" then
		tags.start_menu(menu)
	elseif fields[2] == "wifi" then
		wlan.start_menu(menu)
	elseif fields[2] == "ethernet" then
		lan.start_menu(menu)
	elseif fields[2] == "display" then
		display.start_menu(menu)
	elseif fields[2] == "sound" then
		sound.start_menu(menu)
	elseif fields[2] == "clipboard" then
		sys.spawn("fwm-script "
			.. os.getenv("FWM_HOME")
			.. "/scripts/lua/clipboard-menu.lua"
			)
	elseif fields[2] == "lock" then
		sys.spawn("lock")
	elseif fields[2] == "suspend" then
		suspend.start_menu(menu)
	elseif fields[2] == "exit" then
		if os.getenv("WAYLAND_DISPLAY") ~= nil then
			sys.run("killall -SIGTERM weston")
		else
			sys.run("killall -SIGTERM fwm")
		end
	end
end

function start()
	if not menu then
		return
	end

	show_main_menu()
end
