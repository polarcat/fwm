local display = {}

local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")
local dpy = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/display-lib.lua")

local primary = "eDP-1"

--------------------------------------------------------------------------------
--
-- Local functions.
--
--------------------------------------------------------------------------------

local function toggle_primary_brightness(on)
	sys.spawn("xrandr --output " .. primary .. " --brightness " .. on)
end

local function disconnect_displays()
	local displays = dpy.get_all_displays()
	for _, display in ipairs(displays) do
		if not display:match("^eDP") then
			print("Disconnect display: " .. display)
			sys.spawn("xrandr --output " .. display .. " --off")
		end
	end
end

local function connect_displays()
	local displays = dpy.get_all_displays()
	local pos = {
		"--left-of ",
		"--right-of ",
		"--above ",
		"--below ",
	}

	local pos_idx = 1
	for _, display in ipairs(displays) do
		if not display:match("^eDP") then
			local cmd = "xrandr --output " .. display .. " --auto "

			cmd = cmd .. pos[pos_idx] .. primary
			pos_idx = pos_idx + 1
			if pos_idx > 4 then
				print("Auto-layout only works for 4 displays")
				break
			end
			print(cmd)
			sys.spawn(cmd)
		end
	end
end

local function show_connect_menu(menu, name)
	local menu_items = {
		"\a\t" .. name .. "\t\n",
		"\n",
		"\a\tleft\t\n",
		"\a\tright\t\n",
		"\a\tabove\t\n",
		"\a\tbelow\t\n",
		"\n",
		"\a\trotate\tnormal\n",
		"\a\trotate\tleft\n",
		"\a\trotate\tright\n",
		"\n",
		"\a\tdisconnect\t\n",
	}

	local res = menu.run(sys.config, menu_items)
	if not res then
		return
	end

	local fields = {}
	for field in res:gmatch("[^\t\n]+") do
		fields[#fields + 1] = field
	end

	if not fields then
		return
	end

	local pos
	if fields[2] == "left" then
		pos = "--left-of "
	elseif fields[2] == "right" then
		pos = "--right-of "
	elseif fields[2] == "above" then
		pos = "--above "
	elseif fields[2] == "below" then
		pos = "--below "
	elseif fields[2] == "rotate" then
		local cmd = "xrandr --output " .. name .. " --rotate " .. fields[3]
		print(cmd)
		sys.spawn(cmd)
		return
	elseif fields[2] == "disconnect" then
		disconnect_display(name)
		return
	else
		return
	end

	local cmd = "xrandr --output " .. name .. " --auto " .. pos .. primary
	print(cmd)
	sys.spawn(cmd)
end

local function show_brightness_menu(menu, name)
	local max = sys.read_number(
		"/sys/class/backlight/intel_backlight/max_brightness"
		)
	local cur = sys.read_number(
		"/sys/class/backlight/intel_backlight/actual_brightness"
		)

	cur = string.format("%d", cur * 100 / max)
	local menu_items = {
		"\a\t" .. name .. " brightness " .. cur .. "%\n",
		"\n",
		"\a\tlow\n",
		"\a\tmed\n",
		"\a\tmax\n",
	}

	local res = menu.run(sys.config, menu_items)
	if not res then
		return
	end

	local fields = {}
	for field in res:gmatch("[^\t\n]+") do
		fields[#fields + 1] = field
	end

	if not fields then
		return
	end

	if fields[2] == "low" then
		cur = string.format("%d", max * 10 / 100)
	elseif fields[2] == "med" then
		cur = string.format("%d", max * 50 / 100)
	elseif fields[2] == "max" then
		cur = max
	else
		cur = cur
		return
	end

	sys.spawn("sudo brightness " .. cur)
end

local function show_menu(menu)
	local displays = dpy.get_connected_displays()
	if not displays then
		menu.run(sys.config, { "\a\tNo connected displays found\n" })
		return
	end

	local menu_items = {}

	table.insert(menu_items, "\a\t\t\n")
	table.insert(menu_items, "\n")

	for _, display in ipairs(displays) do
		local name, mode = display:match("^(%S+)%s+(%S+)$")
		table.insert(menu_items, "\a\t" .. name .. "\t" .. mode .. "\n")
	end
	table.insert(menu_items, "\n")
	table.insert(menu_items, "\a\tconnect\tall\n")
	table.insert(menu_items, "\a\tdisconnect\tall\n")

	local res = menu.run(sys.config, menu_items)
	if not res then
		return
	end

	local fields = {}
	for field in res:gmatch("[^\t\n]+") do
		fields[#fields + 1] = field
	end

	if not fields then
		return
	end

	if fields[2] == "connect" then
		connect_displays()
	elseif fields[2] == "disconnect" then
		disconnect_displays()
	elseif fields[2] then
		if fields[2] == primary then
			show_brightness_menu(menu, fields[2])
		else
			show_connect_menu(menu, fields[2])
		end
	end
end

--------------------------------------------------------------------------------
--
-- Public API.
--
--------------------------------------------------------------------------------

-- As a submenu.
function display.start_menu(menu)
	show_menu(menu)
end

-- As a standalone menu.
function start()
	show_menu(sys.load_library("menulib-lua"))
end

return display
