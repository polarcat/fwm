local tags = {}

local lfs = require("lfs")

local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")

--------------------------------------------------------------------------------
--
-- Local functions.
--
--------------------------------------------------------------------------------

local function find_free_tag_id(scr)
	local base = os.getenv("FWM_HOME") .. "/screens/" .. scr .. "/tags/"
	for i = 0, 255 do
		local path = base .. i
		local attr = lfs.attributes(path)
		if not attr then
			return i
		end
	end

	return -1
end

local function show_create_menu(menu, scr)
	local tag = find_free_tag_id(scr)
	if tag < 0 then
		return
	end

	local path = os.getenv("FWM_HOME")
		.. "/screens/" .. scr
		.. "/tags/" .. tag

	local menu_items = {
		"\tCreate tag\t\n",
	}

	local selection, input = menu.run(sys.config, menu_items)
	if not input or input == "_" then
		return
	end

	lfs.mkdir(path)

	sys.fputs(path .. "/name", input)
	sys.setprop(sys.get_shmdir() .. "/.control", "refresh-panel " .. scr)
end

local function show_rename_menu(menu, scr, tag, name)
	local path = os.getenv("FWM_HOME")
		.. "/screens/" .. scr
		.. "/tags/" .. tag
		.. "/name"

	local menu_items = {
		"\tRename tag '" .. name .. "'\t\n",
	}

	local selection, input = menu.run(sys.config, menu_items)
	if not input or input == "_" then
		return
	end

	sys.fputs(path, input)
	sys.setprop(sys.get_shmdir() .. "/.control", "refresh-panel " .. scr)
end

local function delete_tag(scr, tag)
	local path = os.getenv("FWM_HOME")
		.. "/screens/" .. scr
		.. "/tags/" .. tag


	sys.rmdir(path)
	sys.setprop(sys.get_shmdir() .. "/.control", "refresh-panel " .. scr)
end

local function show_edit_menu(menu)
	local taginfo = sys.fgets(sys.get_shmdir() .. "/current-tag");
	local scr, tag, count, name =
		taginfo:match("^(%S+)%s+(%S+)%s+(%S+)%s+(%S+)$")

	if not scr or not tag or not count or not name then
		return
	end

	local menu_items = {
		"\a\t\n",
		"\a\tcreate\n",
		"\a\trename\n",
	}

	if count == "0" then
		table.insert(menu_items, "\a\tdelete '" .. name .. "'\n")
	end

	local selection, input = menu.run(sys.config, menu_items)
	if not selection then
		return
	end

	local fields = {}
	for field in selection:gmatch("[^\t\n]+") do
		fields[#fields + 1] = field
	end

	if not fields and not input then
		return
	end

	if fields[2] == "create" then
		show_create_menu(menu, scr)
		return
	elseif fields[2] == "rename" then
		show_rename_menu(menu, scr, tag, name)
		return
	elseif fields[2] == "delete" then
		delete_tag(scr, tag)
		return
	end

	if not input then
		return
	end

	if input:match("^c" ) then
		show_create_menu(menu, scr)
	elseif input:match("^r" ) then
		show_rename_menu(menu, scr, tag, name)
	elseif input:match("^d" ) then
		delete_tag(scr, tag)
	end
end

local function show_menu(menu)
	local menu_items = { "\a\t\t\n", "\n" }
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

	table.insert(menu_items, "\n")
	table.insert(menu_items, "\a\tedit tag\t\n")

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

	if fields[2] == "edit tag" then
		show_edit_menu(menu)
	elseif fields[3] then
		sys.setprop("/dev/shm/fwm/.control", "focus-tag " .. fields[3])
	end
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
