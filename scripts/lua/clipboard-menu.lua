local clipboard = {}

local lfs = require("lfs")
local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")

local clipboard_dir_ = sys.get_shmdir() .. "/.clipboard/"
local max_str = 64
local max_items = 16

--------------------------------------------------------------------------------
--
-- Local functions.
--
--------------------------------------------------------------------------------

local function paste(name)
	local path = clipboard_dir_ .. name
	os.execute(
		"xclip -i -selection primary " .. path .. ";"
		.. "xclip -i -selection secondary " .. path .. ";"
		.. "xclip -i -selection clipboard " .. path
		)
end

local function make_path(hash)
	local path = clipboard_dir_ .. hash
	local attr = lfs.attributes(path)
	if attr and attr.mode == "file" then
		return nil
	end

	return path
end

local function add_menu_item(name, i, menu_items)
	local file = io.open(clipboard_dir_ .. name, "r")
	if not file then
		return
	end

	local text = file:read("*a")
	file:close()

	local line = text:match("([^\r\n]*)") or ""
	line = line:gsub("[%z\1-\31\127]", "")
	local view = line:sub(1, max_str)

	if #view >= max_str then
		view = view .. "..."
	end

	local item = i
		.. "\t" .. view
		.. "\n"

	table.insert(menu_items, i .. "\t" .. view .. "\t" .. name .. "\n")
end

local function show_menu(menu)
	os.execute("mkdir -p " .. clipboard_dir_)

	local menu_items = { "\a\t\t\n" }
	local selections = { "primary", "secondary", "clipboard" }
	local tmp = sys.get_shmdir() .. "/.clipboard/temp"
	local text

	for _, sel in ipairs(selections) do
		local pipe = io.popen(
			"xclip -selection " .. sel .. " -o 2>/dev/null"
			, "r"
			)

		local view = nil
		if pipe then
			text = pipe:read("*a")
			pipe:close()
			local hash = sys.hash(text)
			path = make_path(hash)
		end

		if path then
			local file = io.open(path, "w")
			if file then
				file:write(text)
				file:close()
			end
		end
	end

	local entries = {}
	for name in lfs.dir(clipboard_dir_) do
		if name~= "." and name ~= ".." then
			local attr = lfs.attributes(clipboard_dir_ .. name)

			if attr and attr.modification then
				entries[#entries + 1] = {
					name = name,
					time = attr.modification
				}
			end
		end
	end

	table.sort(entries, function(a, b)
		return a.time > b.time
	end)

	local last_item
	local i = 1
	for _, entry in ipairs(entries) do
		add_menu_item(entry.name, i, menu_items)
		i = i + 1
		last_item = entry.name
	end

	local res = menu.run(sys.config, menu_items)
	if not res then
		return
	end

	local fields = {}
	for field in res:gmatch("[^\t\n]+") do
		fields[#fields + 1] = field
	end

	if not fields then
		return
	end

	if fields[3] then
		paste(fields[3])
	end

	if i >= max_items and last_item then
		local ok, err = os.remove(clipboard_dir_ .. last_item)
	end
end

--------------------------------------------------------------------------------
--
-- Public API.
--
--------------------------------------------------------------------------------

-- As a submenu.
function clipboard.start_menu(menu)
	show_menu(menu)
end

-- As a standalone menu.
function start()
	show_menu(sys.load_library("menulib-lua"))
end

return clipboard
