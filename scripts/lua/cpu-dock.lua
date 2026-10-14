local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")

function get_params()
	return {
		name = "cpu-dock",
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
	spawn("fwm-cpumon -n cpu-dock"
		.. " -bg " .. sys.get_color("normal-bg")
		.. " -fg " .. sys.get_color("normal-fg")
		.. " -bw 1 "
		.. " -f 60"
		.. " -i 60"
		)
end
