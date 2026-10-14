local bat = {}

local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")

--------------------------------------------------------------------------------
--
-- Public API.
--
--------------------------------------------------------------------------------

local function show_info(menu)
	local path = "/sys/class/power_supply/BAT0/uevent"
	local file = io.open(path, "r")
	if not file then
		print("Failed to open" .. path)
		return
	end

	local info = {}
	for line in file:lines() do
		local str, count = line:gsub("^POWER_SUPPLY_", "")
		local var, val = str:match("^([^=]+)=(.*)$")
		if var == "DEVTYPE" then
			table.insert(info, "\a\t" .. var .. "\t" .. val .. "\n")
		else
			table.insert(info, "\t" .. var .. "\t" .. val .. "\n")
		end
	end

	file:close()

	sys.hide_searchbar()
	menu.run(sys.config, info)
	sys.show_searchbar()
end

-- As a submenu.
function bat.start_menu(menu)
	show_info(menu)
end

-- As a standalone menu.
function start()
	show_info(sys.load_library("menulib-lua"))
end

return bat
