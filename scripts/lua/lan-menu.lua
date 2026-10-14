local lan = {}

local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")
local net = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/network-lib.lua")

--------------------------------------------------------------------------------
--
-- Local functions.
--
--------------------------------------------------------------------------------

local function list_interfaces()
	local ifaces = net.get_wired_interfaces()
	if not ifaces then
		return nil
	end

	local menu_items = {}

	table.insert(menu_items, "\a\t\n")
	table.insert(menu_items, "\n")

	for _, iface in ipairs(ifaces) do
		table.insert(menu_items, "\a\t" .. iface .. "\n")
	end

	return menu_items
end

function disconnect(iface)
	local path1 = os.getenv("FWM_HOME") .. "/tmp/lan-stop.sh"
	local file, err = io.open(path1, "w")
	if not file then
		error("Could not open file '" .. path1 .. "': " .. err)
	end

	file:write("#!/bin/sh\n")
	file:write("printf '\\033[1m\\n Disconnecting \\033[34m"
		.. iface
		.. " ... \\033[0m'\n")
	file:write("sudo ifconfig " .. iface .. " down\n")
	file:write("sudo ip addr flush dev " .. iface .. "\n")
	file:write("sleep 1\n")
	file:close()

	path2 = os.getenv("FWM_HOME") .. "/tmp/lan-stop-prompt.sh"
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

local function get_info(iface)
	if not iface then
		return nil
	end

	local cmd = string.format("ifconfig %q 2>/dev/null", iface)
	local pipe = io.popen(cmd, "r")
	if not pipe then
		return nil
	end

	local info = {}
	local i = 0
	for line in pipe:lines() do
		if i == 0 then
			table.insert(info, "\a\t" .. line .. "\n")
		else
			table.insert(info, "\t" .. line .. "\n")
		end
		i = i + 1
	end

	pipe:close()

	return info
end

function setup_dhcp(iface)
	local path1 = os.getenv("FWM_HOME") .. "/tmp/lan-dhcp.sh"
	local file, err = io.open(path1, "w")
	if not file then
		error("Could not open file '" .. path1 .. "': " .. err)
	end

	file:write("#!/bin/sh\n")
	file:write("printf '\\033[2m'\n")
	file:write("printf '\\n■ Configure " .. iface .. " \\n'\n")
	file:write("sudo ifconfig " .. iface .. " up\n")
	file:write("printf '■ Waiting for DHCP ... \\n'\n")
	file:write("sudo dhclient " .. iface .. " ")
	file:close()

	path2 = os.getenv("FWM_HOME") .. "/tmp/lan-dhcp-prompt.sh"
	file, err = io.open(path2, "w")
	if not file then
		error("Could not open file '" .. path2 .. "': " .. err)
	end

	file:write("#!/bin/sh\n")
	file:write("chmod 0755 " .. path1 .. "\n")
	file:write("SHELL='" .. path1 .. "' xterm -geometry 40x5\n")
	file:close()

	sys.run("/bin/sh " .. path2)
end

local function setup_ip(iface, ip)
	local path1 = os.getenv("FWM_HOME") .. "/tmp/lan-manual.sh"
	local file, err = io.open(path1, "w")
	if not file then
		error("Could not open file '" .. path1 .. "': " .. err)
	end

	file:write("#!/bin/sh\n")
	file:write("printf '\\033[2m\\n'\n")
	file:write("set -x\n")
	file:write("sudo ifconfig " .. iface .. " " .. ip .. " up\n")
	file:write("sleep 1\n")
	file:close()

	path2 = os.getenv("FWM_HOME") .. "/tmp/lan-manual-prompt.sh"
	file, err = io.open(path2, "w")
	if not file then
		error("Could not open file '" .. path2 .. "': " .. err)
	end

	file:write("#!/bin/sh\n")
	file:write("chmod 0755 " .. path1 .. "\n")
	file:write("SHELL='" .. path1 .. "' xterm -geometry 48x4\n")
	file:close()

	sys.run("/bin/sh " .. path2)
end

local function show_ip_menu(menu, iface)
	local menu_items = {
		"\a\tEnter IP address\n",
	}

	local res = menu.run(sys.config, menu_items)
	if not res then
		return
	end

	local fields = {}
	for field in res:gmatch("[^\t\n]+") do
		fields[#fields + 1] = field
	end

	setup_ip(iface, "192.168.1.10")
end

local function show_method_menu(menu, iface)
	local menu_items = {
		"\a\t\n",
		"\n",
		"\a\tdhcp\n",
		"\a\tmanual\n",
	}

	local res = menu.run(sys.config, menu_items)
	if not res then
		return
	end

	local fields = {}
	for field in res:gmatch("[^\t\n]+") do
		fields[#fields + 1] = field
	end

	if fields[2] == "dhcp" then
		setup_dhcp(iface)
	elseif fields[2] == "manual" then
		show_ip_menu(menu, iface)
	end
end

local function show_connect_menu(menu, iface)
	local menu_items = {
		"\a\t" .. iface .. "\n",
		"\n",
		"\a\tconnect\n",
		"\a\tdisconnect\n",
		"\a\tinfo\n",
	}

	local res = menu.run(sys.config, menu_items)
	if not res then
		return
	end

	local fields = {}
	for field in res:gmatch("[^\t\n]+") do
		fields[#fields + 1] = field
	end

	if fields[2] == "connect" then
		show_method_menu(menu, iface)
	elseif fields[2] == "disconnect" then
		disconnect(iface)
	elseif fields[2] == "info" then
		local list = get_info(iface)
		if list then
			sys.hide_searchbar();
			menu.run(sys.config, list)
			sys.show_searchbar();
		end
	end
end

local function show_menu(menu)
	local list = list_interfaces()
	local res = menu.run(sys.config, list)
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

	if fields[2] then
		show_connect_menu(menu, fields[2])
	end
end

--------------------------------------------------------------------------------
--
-- Public API.
--
--------------------------------------------------------------------------------

-- As a submenu.
function lan.start_menu(menu)
	show_menu(menu)
end

-- As a standalone menu.
function start()
	show_menu(sys.load_library("menulib-lua"))
end

return lan
