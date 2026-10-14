local system = {}

local lfs = require("lfs")

local ready = false

system.config = {
	name = "menu",
	hdpi = os.getenv("FWM_HDPI"),
	vdpi = os.getenv("FWM_VDPI"),
	fg = 0xa0a0a0,
	bg = 0x0f0f0f,
	selfg = 0xe0e0e0,
	selbg = 0x303030,
	font_scale = os.getenv("FWM_SCALE"),
	font_size = os.getenv("FWM_FONT_SIZE"),
	text_font = os.getenv("FWM_FONT"),
	icon_font = os.getenv("FWM_ICONS"),
	delineate = false,
	searchbar = true,
	selidx = 0,
}

--------------------------------------------------------------------------------
--
-- I/O helpers.
--
--------------------------------------------------------------------------------

local function file_exists(path)
	local f = io.open(path, "r")
	if f then
		f:close()
		return true
	end
	return false
end

function system.load_library(lib)
	local cpath = package.cpath
	package.cpath = os.getenv("FWM_HOME") .. "/lib/?.so;" .. package.cpath
	local ret = require(lib)
	package.cpath = cpath
	return ret
end

function system.load_script(script)
	return dofile(os.getenv("FWM_HOME")
		.. "/scripts/lua/"
		.. script
		.. ".lua")
end

function system.read_number(path)
	local file, err = io.open(path, "r")
	if not file then
		print(("[file] cannot open %s: %s"):format(path, err))
		return 0
	end

	local val = file:read("*l")
	file:close()

	local ret = tonumber(val)
	if not ret then
		print(("[file] invalid brightness value: %q"):format(val))
		return 0
	end

	return ret
end

--------------------------------------------------------------------------------
--
-- Misc helpers.
--
--------------------------------------------------------------------------------

function system.divide_hex(str, div)
	str = str:gsub("^0[xX]", "")
	local val = tonumber(str, 16)

	if not val then
		return val
	end

	return string.format("0x%x", math.floor(val / div))
end

function system.get_color(name)
	local home = os.getenv("FWM_HOME")
	local file = io.open(home .. "/colors/" .. name)

	if not file then
		return 0xff0000 -- indicate error with red
	end

	local color = file:read("*l")
	file:close()
	return color
end

function system.get_color_dim(name)
	return system.divide_hex(system.get_color(name), 2)
end

function system.get_shmdir()
	return "/dev/shm/fwm"
end

function system.hide_searchbar()
	system.config.searchbar = false
	system.config.selidx = 0
end

function system.show_searchbar()
	system.config.searchbar = true
	system.config.selidx = 0
end

function system.init()
	if not system.ready then
		system.ready = true
	end
end

function system.hash(text)
	local h = 5381

	for i = 1, #text do
		h = (h * 33 + text:byte(i)) % 2^32
	end

	return math.tointeger(h)
end

--------------------------------------------------------------------------------
--
-- C system library.
--
--------------------------------------------------------------------------------

local syslib = system.load_library("syslib-lua")

function system.sleep(sec)
	syslib.sleep(sec)
end

function system.msleep(ms)
	syslib.msleep(ms)
end

function system.setenv(name, value)
	syslib.setenv(name, value)
end

function system.unsetenv(name)
	syslib.unsetenv(name)
end

function system.spawn(cmd)
	os.execute(cmd .. " >/dev/null 2>&1 &")
end

function system.run(cmd)
	os.execute(cmd .. " >/dev/null 2>&1")
end

function system.setprop(prop, val)
	syslib.setprop(prop, val)
end

function system.puts(fd, val)
	syslib.puts(fd, val)
end

return system
