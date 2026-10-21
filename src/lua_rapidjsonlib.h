/*
** dkjson compatibility:
**
**  <metatable>.__jsonorder
**    __jsonorder can overwrite the keyorder for a specific table.
**
**  <metatable>.__tojson (self, state)
**    You can provide your own __tojson function in a metatable. In this
**    function you can either add directly to the buffer and return true, or you
**    can return a string. On errors nil and a message should be returned.
**
**  [compat.lua]
**  json.quotestring (string): Quote a UTF-8 string and escape critical
**    characters using JSON escape sequences. This function is only necessary
**    when you build your own __tojson functions.
**
**  [compat.lua]
**  json.encodeexception (reason, value, state, defaultmessage): This function
**    can be used as value to the exception option. Instead of raising an error
**    this function encodes the error message as a string. This can help to
**    debug malformed input data.
*/
#ifndef lrapidjsonlib_h
#define lrapidjsonlib_h

#include <lua.h>

#define LUA_RAPIDJSON_NAME "lua-rapidjson"
#define LUA_RAPIDJSON_VERSION "lua-rapidjson 1.2.0"
#define LUA_RAPIDJSON_COPYRIGHT "Copyright (C) 2015 Xpol Wan; 2020, Gottfried Leibniz"
#define LUA_RAPIDJSON_DESCRIPTION "rapidjson bindings for Lua"

#if defined(__cplusplus)
extern "C" {
#endif

/*
**  json.encode (object [, state])
**    Create a string representing the object. "Object" can be a table, a
**    string, a number, a boolean, "nil", "json.null" or any object with a
**    function "__tojson" in its metatable. A table can only use strings and
**    numbers as keys and its values have to be valid objects as well.
**
**    "state" is an optional table with the following fields:
**
**      indent: When indent (a boolean) is set, the created string will contain
**        newlines and indentations. Otherwise it will be one long line.
**
**      keyorder: an array to specify the ordering of keys in the encoded
**      output. If an object has keys which are not in this array they are
**      written after the sorted keys.
**
**      indent_amt: This is the initial level of indentation used when indent is
**      set. For each level two spaces are added; when absent it is set to 0.
**
**      [DEPRECATED]
**      buffer: an array to store the strings for the result so they can be
**      concatenated at once. When it isn't given, the encode function will
**      create it temporary and will return the concatenated result.
**
**      [DEPRECATED]
**      bufferlen: the index of the last element of `buffer`.
**
**      [DEPRECATED]
**      tables: tables is a set to detect reference cycles. It is created
**      temporary when absent. Every table that is currently processed is used
**      as key, the value is true.
**
**      [TODO] - Partially implemented
**      exception:  When exception is given, it will be called whenever the
**      encoder cannot encode a given value. The parameters are reason, value,
**      state and defaultmessage. reason is either "reference cycle", "custom
**      encoder failed" or "unsupported type". value is the original value that
**      caused the exception, state is this state table, defaultmessage is the
**      message of the error that would usually be raised. You can either return
**      true and add directly to the buffer or you can return the string
**      directly. To keep raising an error return nil and the desired error
**      message. An example implementation for an exception function is given in
**      json.encodeexception.
*/
LUALIB_API int rapidjson_encode(lua_State *L);

/*
**  json.decode (string [, position [, null [, objectmeta [, arraymeta]]]])
**    Decode string starting at position or at 1 if position was omitted.
**
**    "null": an optional value to be returned for null values. The default is
**    nil, but you could set it to json.null or any other value.
**
**      Default "null" functionality (when null is nil) has been replaced with a
**      bit-flag.
**
**    "objectmeta"/"arraymeta": Every array or object that is decoded gets a
**    metatable with the __jsontype field set to either array or object. To
**    provide your own metatable or to prevent the assigning of metatables,
**    these options exist.
**
**    The return values are the object or nil, the position of the next
**    character that doesn't belong to the object, and in case of errors an
**    error message.
*/
LUALIB_API int rapidjson_decode(lua_State *L);

/*
** BOOLEAN:
**  indent - Created string will contain newlines and indentations.
**  pretty - Alias of "indent".
**  sort_keys - Sort keys of a table prior to encoding.
**  null - If enabled use lua_pushnil, otherwise the json.null sentinel.
**  nesting - Write null instead of throwing a LUA_RAPIDJSON_ERROR_DEPTH_LIMIT
**            error when the encoding depth exceeds 'max_depth'.
**  unsigned - Encode integers as unsigned integers/values.
**  nan - Allow writing of Infinity, -Infinity and NaN.
**  inf - Alias of "nan".
**  bit32 - Encode integers as 32-bit values.
**  lua_format_float - Use sprintf instead of rapidjson's native Grisu2.
**  lua_round_float - Massage Grisu2 by rounding at maxDecimalsPlaces.
**  single_line - Format arrays on a single line.
**  empty_table_as_array - Empty tables encoded as array.
**  with_hole - Allow tables to be encoded as arrays iff all keys are positive
**              integers, inserting "nil"s when encoding to satisfy the array
**              type.
**
** INTEGER:
**  max_depth - Maximum table recursion depth
**  indent_char - Index for indentation (0 = ' ', 1 = '\t', 2 = '\n', 3 = '\r').
**  indent_count - Number of indent characters for each indentation level.
**  level - Alias of "indent_count".
**  decimal_count - the maximum number of decimal places for double output.
**
** STRING:
**  decoder_preset - ["default", "extended"] - Preset parsing configuration.
**    "extended" enables all fields (see rapidjson::ParseFlag).
*/
LUALIB_API int rapidjson_setoption (lua_State *L);
LUALIB_API int rapidjson_getoption (lua_State *L);

/* */
LUALIB_API int rapidjson_object (lua_State *L);
LUALIB_API int rapidjson_isobject (lua_State *L);

/* */
LUALIB_API int rapidjson_array (lua_State *L);
LUALIB_API int rapidjson_isarray (lua_State *L);

#define LUA_RAPIDJSON_JSON_LIBNAME "json"
LUAMOD_API int (luaopen_rapidjson) (lua_State *L);

#if defined(__cplusplus)
}
#endif

#endif
