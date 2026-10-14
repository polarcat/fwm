local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")

function get_params()
	return {
		name = "clock-dock",
		icon = nil,
		text = nil,
		cmd = nil,
		bg = sys.get_color("normal-bg"),
		fg = sys.get_color("normal-fg"),
	}
end

function spawn(cmd)
	sys.spawn(cmd)
end

function start(path)
	spawn("fwm-clock -n clock-dock"
		.. " -f " .. "\"%Y-%m-%d/%W %a %H:%M\""
		.. " -bg " .. sys.get_color("normal-bg")
		.. " -fg " .. sys.get_color("normal-fg")
		.. " -fs " .. os.getenv("FWM_FONT_SIZE")
		.. " -hd " .. os.getenv("FWM_HDPI")
		.. " -vd " .. os.getenv("FWM_VDPI")
		)
end
