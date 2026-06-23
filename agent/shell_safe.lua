-- UTF-8 safe shell utilities
-- Safe string truncation that never breaks multi-byte characters,
-- and safe escaping for passing strings through shell to osascript/notify-send.

local M = {}

local utf8_mod = require("utf8")

--- Truncate a UTF-8 string to at most max_bytes, never breaking
--- a multi-byte character in the middle.
--- @param str string
--- @param max_bytes number
--- @return string
function M.utf8_safe_sub(str, max_bytes)
  if type(str) ~= "string" then return "" end
  if #str <= max_bytes then return str end

  local result = ""
  local byte_count = 0
  for _, codepoint in utf8_mod.codes(str) do
    local char = utf8_mod.char(codepoint)
    if byte_count + #char > max_bytes then
      break
    end
    result = result .. char
    byte_count = byte_count + #char
  end
  return result
end

--- Escape a string for single-quoted shell argument.
--- Handles single quotes by closing, escaping, and reopening.
--- @param str string
--- @return string
function M.shell_escape(str)
  if type(str) ~= "string" then return "''" end
  return "'" .. str:gsub("'", "'\\''") .. "'"
end

--- Escape a string for AppleScript double-quoted string.
--- Escapes backslash, double-quote, newline, carriage return, and tab.
--- @param str string
--- @return string
function M.applescript_escape(str)
  if type(str) ~= "string" then return '""' end
  local escaped = str
    :gsub("\\", "\\\\")
    :gsub('"', '\\"')
    :gsub("\n", "\\n")
    :gsub("\r", "\\r")
    :gsub("\t", "\\t")
  return '"' .. escaped .. '"'
end

return M
