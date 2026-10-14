local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")

local icon_ = ""

function get_params()
	return {
		name = "vpn-dock",
		icon = icon_,
		text = nil,
		cmd = "vpn-status",
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
