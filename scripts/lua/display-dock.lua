local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")

local icon_ = ""

function get_params()
	return {
		name = "display-dock",
		icon = icon_,
		text = nil,
		cmd = "fwm-script "
			.. os.getenv("FWM_HOME")
			.. "/scripts/lua/display-menu.lua",
		bg = sys.get_color("normal-bg"),
		fg = sys.get_color("normal-fg"),
	}
end

function spawn(cmd)
	sys.spawn(cmd)
end

function start(path)
	return
end
