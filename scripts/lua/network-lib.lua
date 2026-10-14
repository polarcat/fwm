--------------------------------------------------------------------------------
--
-- Network helpers.
--
--------------------------------------------------------------------------------

local network = {}

local lfs = require("lfs")

--------------------------------------------------------------------------------
--
-- Generic helpers.
--
--------------------------------------------------------------------------------
local function match_entry(dir, expr)
	for name in lfs.dir(dir) do
		if name ~= "." and name ~= ".." and name:match(expr) then
			return true, name
		end
	end

	return false, nil
end

local function validate_interface(iface)
	if not iface then
		return false
	elseif iface == "net" then
		return false
	elseif iface == "lo" then
		return false
	elseif iface:match("^docker.*$") then
		return false
	elseif file_exists("/sys/class/net/" .. iface .. "/wireless") then
		return false
	elseif match_entry("/sys/class/net/" .. iface, ".*docker.*") then
		return false
	else
		return true
	end
end

function network.get_ethernet_interfaces()
	local ifaces = {}
	for iface in lfs.dir("/sys/class/net/") do
		if iface ~= "." and iface ~= ".." and iface ~= "..." then
			if validate_interface(iface) then
				table.insert(ifaces, iface)
			end
		end
	end

	return ifaces
end

function network.iface_route_present(name)
	local file = io.open("/proc/net/route", "r")

	if not file then
		return false
	end

	file:read("*l")
	for line in file:lines() do
		local iface, dest = line:match("^(%S+)%s+(%S+)")
		-- If destination check is needed then compare dest to 00000000
		if iface == name then
			file:close()
			return true
		end
	end
	file:close()

	return false
end

--------------------------------------------------------------------------------
--
-- Discover onboard netowrk devices.
--
--------------------------------------------------------------------------------

local function is_dev_wireless(path)
	local attr = lfs.attributes(path)

	if attr and attr.mode == "directory" then
		return true
	else
		return false
	end
end

local function scan_pci_dev(path, lan, wlan)
	local attr = lfs.attributes(path)

	if not attr or (attr and attr.mode ~= "directory") then
		return
	end

	for dev in lfs.dir(path) do
		if dev ~= "." and dev ~= ".." then
			path = path .. "/" .. dev .. "/wireless"
			if is_dev_wireless(path) then
				table.insert(wlan, dev)
			else
				table.insert(lan, dev)
			end
		end
	end
end

local function scan_pci_bus(path, lan, wlan)
	for dev in lfs.dir(path) do
		if dev:match("^[0-9]+.*") then
			scan_pci_dev(path .. "/" .. dev .. "/net", lan, wlan)
		end
	end
end

--------------------------------------------------------------------------------
--
-- Public API.
--
--------------------------------------------------------------------------------

function network.get_network_interfaces()
	local lan = {}
	local wlan = {}
	for bus in lfs.dir("/sys/devices") do
		if bus:match("^.*pci[0-9]+.*") then
			scan_pci_bus("/sys/devices/" .. bus, lan, wlan)
		end
	end

	return lan, wlan
end

function network.get_wireless_interfaces()
	local lan = {}
	local wlan = {}
	for bus in lfs.dir("/sys/devices") do
		if bus:match("^.*pci[0-9]+.*") then
			scan_pci_bus("/sys/devices/" .. bus, lan, wlan)
		end
	end

	return wlan
end

function network.get_wired_interfaces()
	local lan = {}
	local wlan = {}
	for bus in lfs.dir("/sys/devices") do
		if bus:match("^.*pci[0-9]+.*") then
			scan_pci_bus("/sys/devices/" .. bus, lan, wlan)
		end
	end

	return lan
end

return network
