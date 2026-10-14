local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")

local icon_ = ""

local function update_status(prop, fg_norm, fg_dim)
	local path = "/sys/class/power_supply/AC/online"
	local file = io.open(path, "r")
	if not file then
		print("Failed to open" .. path)
		return
	end

	local status = file:read("*l")
	file:close()

	if status:gsub("%s+", "") == "1" then
		sys.puts(prop, icon_ .. " " .. fg_norm)
	else
		sys.puts(prop, icon_ .. " " .. fg_dim)
	end
end

function get_params()
	return {
		name = "ac-dock",
		icon = icon_,
		text = nil,
		cmd = "fwm-script "
			.. os.getenv("FWM_HOME")
			.. "/scripts/lua/suspend-menu.lua",
		bg = sys.get_color("normal-bg"),
		fg = sys.get_color("normal-fg"),
	}
end

function spawn(cmd)
	sys.spawn(cmd)
end

function start(fd)
	local fg_norm = sys.get_color("normal-fg")
	local fg_dim = sys.divide_hex(fg_norm, 2)

	update_status(fd, fg_norm, fg_dim)

	local pipe = io.popen("udevadm monitor", "r")
	if not pipe then
		print("[ac-dock] Failed to start 'udevadm monitor'")
		return
	end

	for msg in pipe:lines() do
		if msg:match("^.*change.*power_supply.*") then
			update_status(fd, fg_norm, fg_dim)
		end
	end

	pipe:close()
end
