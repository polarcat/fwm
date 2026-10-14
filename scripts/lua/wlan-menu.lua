local wlan = {}

local lfs = require("lfs")
local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")
local net = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/network-lib.lua")

local basedir = "/etc/wpa_supplicant"
local ifaces_ = net.get_wireless_interfaces()

--------------------------------------------------------------------------------
--
-- Local functions.
--
--------------------------------------------------------------------------------

-- Return the ESSID for an interface, or nil if it cannot be found.
--
local function get_essid(iface)
	if not iface then
		return nil
	end

	local cmd = string.format("iwconfig %q 2>/dev/null", iface)
	local pipe = io.popen(cmd, "r")
	if not pipe then
		return nil
	end

	local output = pipe:read("*a")
	pipe:close()

	-- Handle output such as:
	--   ESSID:"My WiFi"
	--   ESSID:off/any
	--   ESSID:""
	local essid = output:match('ESSID:%s*"([^"]*)"')

	if essid and essid ~= "" then
		return essid
	end

	return nil
end

local function list_networks()
	local entries = {}
	for entry in lfs.dir(basedir .. "/configs") do
		if entry ~= "." and entry ~= ".." and entry ~= "..." then
			entries[#entries + 1] = entry
		end
	end
	table.sort(entries)

	local menu_items = {}

	table.insert(menu_items, "\a\t\n")
	table.insert(menu_items, "\n")

	for _, entry in ipairs(entries) do
		table.insert(menu_items, "\a\t" .. entry .. "\n")
	end

	return menu_items
end

local function disconnect()
	if not ifaces_ then
		return
	end

	local essid = get_essid(ifaces_[1])
	if not essid then
		return
	end

	local path1 = os.getenv("FWM_HOME") .. "/tmp/wlan-stop.sh"
	local file, err = io.open(path1, "w")
	if not file then
		error("Could not open file '" .. path1 .. "': " .. err)
	end

	file:write("#!/bin/sh\n")
	file:write("printf '\\033[2m\\nDisconnect " .. essid .. "\\033[0m'\n")
	file:write("sudo ifconfig " .. ifaces_[1] .. " down\n")
	file:write("sudo ip addr flush dev " .. ifaces_[1] .. "\n")
	file:close()

	path2 = os.getenv("FWM_HOME") .. "/tmp/wlan-stop-prompt.sh"
	file, err = io.open(path2, "w")
	if not file then
		error("Could not open file '" .. path2 .. "': " .. err)
	end

	file:write("#!/bin/sh\n")
	file:write("chmod 0755 " .. path1 .. "\n")
	file:write("SHELL='" .. path1 .. "' xterm -geometry 40x4\n")
	file:close()

	sys.run("/bin/sh " .. path2)
end

local function get_info()
	if not ifaces_ then
		return nil
	end

	local cmd = string.format("iwconfig %q 2>/dev/null", ifaces_[1])
	local pipe = io.popen(cmd, "r")
	if not pipe then
		return nil
	end

	local info = {}
	local i = 0
	for line in pipe:lines() do
		if i == 0 then
			table.insert(info, "\a\t" .. line .. "\n")
		else
			table.insert(info, "\t" .. line .. "\n")
		end
		i = i + 1
	end

	pipe:close()

	cmd = string.format("ifconfig %q 2>/dev/null", ifaces_[1])
	pipe = io.popen(cmd, "r")
	if not pipe then
		return nil
	end

	for line in pipe:lines() do
		local l = line:match("^.*inet.*$")
		if l then
			table.insert(info, "\t  " .. l .. "\n")
		end
	end

	pipe:close()

	return info
end

local function show_info(menu)
	local list = get_info()
	if list then
		sys.hide_searchbar()
		menu.run(sys.config, list)
		sys.show_searchbar()
	end
end

local function show_connect_menu(menu)
	local list = list_networks()
	local text = menu.run(sys.config, list)
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

	sys.spawn("wlan-connect " .. fields[2])
end

local function show_menu(menu)
	if not ifaces_ then
		return
	end

	local essid = get_essid(ifaces_[1])
	if not essid then
		show_connect_menu(menu)
		return
	end

	local menu_items = {
		"\a\t" .. essid .. "\n",
		"\n",
		"\a\tconnect\n",
		"\a\tdisconnect\n",
		"\a\tinfo\n",
	}

	local text = menu.run(sys.config, menu_items)
	if not text then
		return
	end

	local fields = {}
	for field in text:gmatch("[^\t\n]+") do
		fields[#fields + 1] = field
	end

	if fields[2] == "connect" then
		show_connect_menu(menu)
	elseif fields[2] == "disconnect" then
		disconnect()
	elseif fields[2] == "info" then
		show_info(menu)
	end
end

--------------------------------------------------------------------------------
--
-- Public API.
--
--------------------------------------------------------------------------------

-- As a submenu.
function wlan.start_menu(menu)
	show_menu(menu)
end

-- As a standalone menu.
function start()
	show_menu(sys.load_library("menulib-lua"))
end

return wlan
