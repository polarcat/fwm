local tags = {}

local lfs = require("lfs")

local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")

--------------------------------------------------------------------------------
--
-- Local functions.
--
--------------------------------------------------------------------------------

local function show_menu(menu)
	local menu_items = {}
	local tags_dir = os.getenv("FWM_HOME") .. "/screens/0/tags"
	for tag_id in lfs.dir(tags_dir) do
		if tag_id ~= "." and tag_id ~= ".." then
			local path = tags_dir .. "/" .. tag_id .. "/name"
			local file = io.open(path, "r")
			if file then
				local tag_name = file:read("*l")
				file:close()
				table.insert(menu_items, "\a\t"
					.. tag_name
					.. "\t"
					.. tag_id
					.. "\n")
			end
		end
	end

	if not menu_items then
		return
	end

	local res = menu.run(sys.config, menu_items)
	if not res then
		return
	end

	local fields = {}
	for field in res:gmatch("[^\t\n]+") do
		fields[#fields + 1] = field
	end

	sys.setprop("/dev/shm/fwm/.control", "focus-tag " .. fields[3])
end

--------------------------------------------------------------------------------
--
-- Public API.
--
--------------------------------------------------------------------------------

-- As a submenu.
function tags.start_menu(menu)
	show_menu(menu)
end

-- As a standalone menu.
function start()
	show_menu(sys.load_library("menulib-lua"))
end

return tags
