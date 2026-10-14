local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")

local icon_ = ""

function get_params()
	return {
		name = "usb-dock",
		icon = icon_,
		text = nil,
		cmd = nil,
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

	sys.puts(fd, icon_ .. " " .. fg_dim)

	local pipe = io.popen("udevadm monitor", "r")
	if not pipe then
		print("[usb-dock] Failed to start 'udevadm monitor'")
		return
	end

	for msg in pipe:lines() do
		if msg:match("^.*add.*usb.*") then
			sys.puts(fd, icon_ .. " " .. fg_norm)
		elseif msg:match("^.*remove.*usb.*") then
			sys.puts(fd, icon_ .. " " .. fg_dim)
		end
	end

	pipe:close()
end
