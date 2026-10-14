local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")

local icon_= ""
local capacity_prev_ = 0

local function update_capacity(fd, fg)
	local path = "/sys/class/power_supply/BAT0/capacity"
	local file = io.open(path, "r")
	if not file then
		print("Failed to open" .. path)
		return
	end

	local capacity = file:read("*l")
	file:close()

	capacity = tonumber(capacity)

	local icon = icon_
	if capacity == capacity_ then
		return
	elseif capacity == 100 then
		icon = ""
	elseif capacity >= 75 and capacity < 100 then
		icon = ""
	elseif capacity >= 25 and capacity < 75 then
		icon = ""
	else
		icon = "" -- empty battery icon
		fg = "0xff0000"
	end

	sys.puts(fd, icon .. " " .. fg .. " " .. capacity .. "%")
	capacity_ = capacity
end

function get_params()
	return {
		name = "bat-dock",
		icon = icon_,
		text = "0%   ",
		cmd = "fwm-script "
			.. os.getenv("FWM_HOME")
			.. "/scripts/lua/bat-info.lua",
		bg = sys.get_color("normal-bg"),
		fg = sys.get_color("normal-fg"),
	}
end

function spawn(cmd)
	sys.spawn(cmd)
end

function start(fd)
	local fg = sys.get_color("normal-fg")

	update_capacity(fd, fg)

	local pipe = io.popen("udevadm monitor", "r")
	if not pipe then
		print("[bat-dock] Failed to start 'udevadm monitor'")
		return
	end

	for msg in pipe:lines() do
		if msg:match("^.*change.*power_supply.*") then
			update_capacity(fd, fg)
		end
	end

	pipe:close()
end
