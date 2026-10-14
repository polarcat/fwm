local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")
local net = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/network-lib.lua")

local icon_ = ""
local fg_norm_ = sys.get_color("normal-fg")
local fg_dim_ = sys.divide_hex(fg_norm_, 2)
local ifaces_ = net.get_wireless_interfaces()

function get_params()
	return {
		name = "wlan-dock",
		icon = icon_,
		text = nil,
		cmd = "fwm-script "
			.. os.getenv("FWM_HOME")
			.. "/scripts/lua/wlan-menu.lua",
		bg = sys.get_color("normal-bg"),
		fg = fg_norm_,
	}
end

function spawn(cmd)
	sys.spawn(cmd)
end

local function update_status(fd, iface)
	if net.iface_route_present(iface) then
		sys.puts(fd, icon_ .. " " .. fg_norm_)
	else
		sys.puts(fd, icon_ .. " " .. fg_dim_)
	end
end

function start(fd)
	if not ifaces_ then
		return
	end

	update_status(fd, ifaces_[1])

	local pipe = io.popen("fwm-rtlink", "r")
	if not pipe then
		print("[wlan-dock] Failed to start 'fwm-rtlink'")
		return
	end

	print("[wlan-dock] Monitor " .. ifaces_[1])
	for msg in pipe:lines() do
		if msg:match("RTM_*") then
			update_status(fd, ifaces_[1])
		end
	end

	pipe:close()
end
