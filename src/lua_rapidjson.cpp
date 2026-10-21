/*
** $Id: lua_rapidjson.cpp $
** rapidjson binding library
** See Copyright Notice in LICENSE
*/
#define lua_rapidjson_c
#define LUA_LIB

#include <vector>
#include <algorithm>

#include <rapidjson/internal/stack.h>
#include <rapidjson/rapidjson.h>
#include <rapidjson/encodedstream.h>
#include <rapidjson/error/en.h>
#include <rapidjson/error/error.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/rapidjson.h>
#if defined(LUA_RAPIDJSON_COMPAT)
  #include "reader_dkcompat.hpp"
#else
  #include <rapidjson/reader.h>
#endif

#include "lua_rapidjson.hpp"
#include "StringStream.hpp"

#include "lua_rapidjsonlib.h"

/* Registry Table */
#define LUA_RAPIDJSON_REG "lua_rapidjson"
#define LUA_RAPIDJSON_ENCODER LUA_RAPIDJSON_REG "_encoder"
#define LUA_RAPIDJSON_DECODER LUA_RAPIDJSON_REG "_decoder"

/*
** If LUA_COMPILED_AS_HPP is enabled (... and LUA_USE_LONGJMP_HPP is not), it is
** assumed LUAI_THROW/LUAI_TRY is implemented with try/catch blocks and no
** interoperability concerns arise (i.e., stack-tossing vs. stack-unwinding).
*/
#undef LUA_RAPIDJSON_ANCHOR
#if !defined(LUA_COMPILED_AS_HPP) && !defined(LUA_USE_LONGJMP_HPP)
  #define LUA_RAPIDJSON_ANCHOR
#endif

typedef lua_Integer json_regType;  // Registry Table KeyType

/* Registry Subtable Keys */
#define LUA_RAPIDJSON_REG_FLAGS 1
#define LUA_RAPIDJSON_REG_DEPTH 2
#define LUA_RAPIDJSON_REG_INDENT 3
#define LUA_RAPIDJSON_REG_INDENT_AMT 4
#define LUA_RAPIDJSON_REG_MAXDEC 5
#define LUA_RAPIDJSON_REG_PRESET 6

#define json_conf_getfield(L, I, K) lua_rawgeti((L), (I), (K))
#define json_conf_setfield(L, I, K) lua_rawseti((L), (I), (K))

#define lua_absindex(L, i) ((i) > 0 || (i) <= LUA_REGISTRYINDEX ? (i) : lua_gettop(L) + (i) + 1)
static int lua_rapidjson_getsubtable (lua_State *L, int idx, const char *key) {
  if (json_getfield(L, idx, key) == LUA_TTABLE)
    return 1;  // table already there
  else {
    lua_pop(L, 1);  // remove previous result
    idx = lua_absindex(L, idx);
    lua_createtable(L, LUA_RAPIDJSON_REG_PRESET + 1, 0);
    lua_pushvalue(L, -1);  // copy to be left at top
    lua_setfield(L, idx, key);  // assign new table to field
    return 0;  // false, because did not find table there
  }
}

static lua_Integer geti (lua_State *L, int idx, json_regType key, lua_Integer opt) {
  json_conf_getfield(L, idx, key);

  const lua_Integer result = luaL_optinteger(L, -1, opt);
  lua_pop(L, 1);
  return result;
}

/* Push a integer into the registry table at the specified key */
static void seti (lua_State *L, int idx, json_regType key, lua_Integer value) {
  lua_pushinteger(L, value);  // [..., value]
  json_conf_setfield(L, json_rel_index(idx, 1), key);  // [...]
}

/*
** If the stack argument is a convertible to a size_t from an lua_Integer,
** returns the size_t. If the argument is absent or is nil, returns def.
** Otherwise, throw an error.
*/
static size_t luaL_optsizet (lua_State *L, int arg, size_t def) {
  if (lua_isnoneornil(L, arg))
    return def;
  else if (json_isinteger(L, arg)) {  // 5.1/5.2: Number not an integer
    const lua_Integer i = lua_tointeger(L, arg);
    if (i >= 0 && static_cast<size_t>(i) <= JSON_MAX_LUAINDEX)
      return static_cast<size_t>(i);

    luaL_argerror(L, arg, "invalid integer argument");
    return 0;
  }
  luaL_argerror(L, arg, lua_pushfstring(L, "integer expected"));
  return 0;
}

/* A luaL_checkoption that doesn't throw an error. */
static int luaL_optcheckoption (lua_State *L, int arg, const char *def, const char *const lst[], int ldef) {
  const char *name = (def) ? luaL_optstring(L, arg, def) : luaL_checkstring(L, arg);
  for (int i = 0; lst[i]; i++) {
    if (strcmp(lst[i], name) == 0)
      return i;
  }
  return ldef;
}

/*
** {==================================================================
** API Functions
** ===================================================================
*/

int json_null (lua_State *L) {
  lua_pushcfunction(L, json_null);
  return 1;
}

bool is_json_null (lua_State *L, int idx) {
  return lua_tocfunction(L, idx) == json_null;
}

bool has_json_type (lua_State *L, int idx, bool *is_array) {
  bool result = false;

  const int type = luaL_getmetafield(L, idx, LUA_RAPIDJSON_META_TYPE);
  if (type != LUA_METAFIELD_FAIL) {
    if (type == LUA_TSTRING) {
      *is_array = strcmp(lua_tostring(L, -1), LUA_RAPIDJSON_META_TYPE_ARRAY) == 0;
      result = true;
    }
    lua_pop(L, 1);
  }

  return result;
}

bool table_is_json_array (lua_State *L, int idx, lua_Integer flags, size_t *array_length) {
  const int stacktop = lua_gettop(L);
  const int i_idx = json_rel_index(idx, 1);

  bool has_type = false;  // Has __jsontype field;
  bool is_array = false;  // __jsontype field corresponds to an "array" value.

  size_t count = 0;  // Number of valid array elements
  size_t max = 0;  // Maximum (parsed) integer key
  size_t arraylen = 0;  // Supplied table.pack 'n' value

  json_checkstack(L, 3);
  has_type = has_json_type(L, idx, &is_array);

  lua_pushnil(L);  // [..., key]
  while (lua_next(L, i_idx)) {  // [..., key, value]
    lua_Integer n;
#if defined(LUA_RAPIDJSON_COMPAT)
    size_t strlen = 0;
    const char *key = nullptr;
#endif

    /* && within range of size_t */
    if (json_isinteger(L, -2) && (n = lua_tointeger(L, -2), (n >= 1 && static_cast<size_t>(n) <= JSON_MAX_LUAINDEX))) {
      const size_t nst = static_cast<size_t>(n);
      max = nst > max ? nst : max;
      count++;
    }
#if defined(LUA_RAPIDJSON_COMPAT)
    /* Similar to dkjson; support the common table.pack / { n = select("#", ...), ... } idiom */
    else if (lua_type(L, -2) == LUA_TSTRING
             && json_isinteger(L, -1)
             && (n = lua_tointeger(L, -1), (n >= 1 && static_cast<size_t>(n) <= JSON_MAX_LUAINDEX))
             && (key = lua_tolstring(L, -2, &strlen)) != nullptr
             && strlen == 1 && key[0] == 'n') {
      arraylen = static_cast<size_t>(n);
      max = arraylen > max ? arraylen : max;
    }
#endif
    else {
      lua_settop(L, stacktop);
      return false;
    }
    lua_pop(L, 1);  // [..., key]
  }

  *array_length = max;
  lua_settop(L, stacktop);

  /*
  ** encode2: encode an empty Lua table as an object iff its given an object
  ** jsontype. (Library addition:) Otherwise, only encode an empty table as an
  ** object if the JSON_ARRAY_EMPTY is not set.
  **/
  if (max == 0 && has_type && !is_array)
    return false;
  else if (max == count)  // all keys are positive integers from [1, count] or the table is empty;
    return max > 0 || (flags & JSON_ARRAY_EMPTY);
  else if (flags & JSON_ARRAY_WITH_HOLES)  // do not create an array with too many holes (inserted nils);
    return (max < LUA_RAPIDJSON_TABLE_CUTOFF) || max <= arraylen || (count >= (max >> 1));

  return false;
}

/* }================================================================== */

/*
** {==================================================================
** CoreAPI
** ===================================================================
*/

/* kParseDefaultFlags */
#define JSON_DECODE_DEFAULT 0x0

/*
** kParseFullPrecisionFlag + kParseCommentsFlag + kParseTrailingCommasFlag
** kParseNanAndInfFlag
*/
#define JSON_DECODE_EXTENDED 0x1

/* PrettyWriter indentation characters */
static const char pretty_indent[] = { ' ', '\t', '\n', '\r' };

/* Configuration options */
static const char *const option_keys[] = {
  "",
  "indent", "pretty",
  "sort_keys",
  "null",
  "nesting",
  "unsigned",
  "nan", "inf",
  "bit32",
  "lua_format_float",
  "lua_round_float",
  "vectorarray",
  "single_line",
  "empty_table_as_array",
  "with_hole",
  "decoder_preset",
  "max_depth",
  "indent_char",
  "indent_count", "level",  /* state.level in dkjson */
  "decimal_count",
  LUA_RAPIDJSON_STATE_KEYORDER,
  LUA_RAPIDJSON_STATE_EXCEPTION,
  nullptr
};

/* option_keys -> integer codes */
static const lua_Integer option_keys_num[] = {
  JSON_OPTION_RESERVED, /* RESERVED */
  JSON_PRETTY_PRINT, JSON_PRETTY_PRINT,
  JSON_SORT_KEYS,
  JSON_LUA_NULL,
  JSON_NESTING_NULL,
  JSON_UNSIGNED_INTEGERS,
  JSON_NAN_AND_INF, JSON_NAN_AND_INF,
  JSON_ENCODE_INT32,
  JSON_LUA_DTOA,
  JSON_LUA_GRISU,
  JSON_ENCODER_ARRAY_VECTOR,
  JSON_ARRAY_SINGLE_LINE,
  JSON_ARRAY_EMPTY,
  JSON_ARRAY_WITH_HOLES,
  JSON_DECODER_PRESET,
  JSON_ENCODER_MAX_DEPTH,
  JSON_ENCODER_INDENT,
  JSON_ENCODER_INDENT_AMT, JSON_ENCODER_INDENT_AMT,
  JSON_ENCODER_DECIMALS,
  JSON_TABLE_KEY_ORDER,
  JSON_ENCODER_HANDLER,
};

/* Decoder PrettyWriter/Writer preset configurations */
static const char *const decode_presets[] = {
  "default", "extended", nullptr
};

/* decode_presets -> integer codes */
static const lua_Integer decode_presets_num[] = {
  JSON_DECODE_DEFAULT,
  JSON_DECODE_EXTENDED,
};

static inline void create_shared_meta (lua_State *L, const char *meta, const char *type) {
  luaL_getsubtable(L, LUA_REGISTRYINDEX, meta);
  lua_pushstring(L, type);
  lua_setfield(L, -2, LUA_RAPIDJSON_META_TYPE);
  lua_pop(L, 1);
}

static int make_table_type (lua_State *L, int idx, const char *meta, const char *type) {
  if (lua_isnoneornil(L, idx))
    lua_createtable(L, 0, 0);
  else if (lua_istable(L, idx)) {
    lua_pushvalue(L, idx);  // [..., table]
    if (lua_getmetatable(L, -1)) {  // already has metatable, set __jsontype field
      lua_pushstring(L, type);  // [..., table, meta_table, type]
      lua_setfield(L, -2, LUA_RAPIDJSON_META_TYPE);
      lua_pop(L, 1);  // [..., table]
      return 1;
    }
  }
  else {
    return luaL_argerror(L, idx, "optional table excepted");
  }

  luaL_getmetatable(L, meta);  // [..., table, meta_table]
  lua_setmetatable(L, -2);  // [..., table]
  return 1;
}

/// <summary>
/// Helper for anchoring a rapidjson::Writer (via userdata) on the Lua stack.
/// Ensuring all intermediate data allocated by rapidjson can be deallocated on
/// error.
/// </summary>
struct EncoderData {
  using Buffer = rapidjson::GenericStringBuffer<rapidjson::UTF8<>, RAPIDJSON_ALLOCATOR>;

  template<unsigned writeFlags = rapidjson::kWriteDefaultFlags>
  using Basic = rapidjson::Writer<Buffer, LUA_RAPIDJSON_SOURCE, LUA_RAPIDJSON_TARGET, RAPIDJSON_ALLOCATOR, writeFlags>;

  template<unsigned writeFlags = rapidjson::kWriteDefaultFlags>
  using Pretty = rapidjson::PrettyWriter<Buffer, LUA_RAPIDJSON_SOURCE, LUA_RAPIDJSON_TARGET, RAPIDJSON_ALLOCATOR, writeFlags>;

  template<unsigned writeFlags = rapidjson::kWriteDefaultFlags>
  using BasicInf = Basic<writeFlags | rapidjson::kWriteNanAndInfFlag>;

  template<unsigned writeFlags = rapidjson::kWriteDefaultFlags>
  using PrettyInf = Pretty<writeFlags | rapidjson::kWriteNanAndInfFlag>;

  bool init;  // Has been constructed in-place
  lua_Integer flags;  // Encoding flags
  lua_Integer indent;  // Indentation character index
  lua_Integer indent_amt;  // Indentation character count
  lua_Integer parsemode;  // Parsing PrettyWriter/Writer mode (preset configuration)
  int depth;  // Maximum nested-table/recursive depth
  int decimals;  // rapidjson::Writer::kDefaultMaxDecimalPlaces;

  RAPIDJSON_ALLOCATOR *allocator = nullptr;
  Buffer _buffer;  // Output buffer.
  std::vector<LuaSAX::Key> _order;  // Pre-specified key order for tables.
  void *writer_ud = nullptr;  // Allocated encoder instance

  EncoderData(RAPIDJSON_ALLOCATOR *_allocator)
    : init(true), flags(JSON_DEFAULT), indent(0), indent_amt(4), parsemode(JSON_DECODE_DEFAULT),
      depth(LUA_RAPIDJSON_DEFAULT_DEPTH), decimals(LUA_NUMBER_FMT_LEN), allocator(_allocator), _buffer(_allocator)  {
  }

  /// <summary>
  /// Initialize necessary fields after a rapidjson::Allocator malloc.
  /// </summary>
  RAPIDJSON_FORCEINLINE void Preinitialize() {
    init = false;
    writer_ud = nullptr;
    allocator = nullptr;
  }

  /// <summary>
  /// Initialize the Encoder in-place.
  /// </summary>
  RAPIDJSON_FORCEINLINE void InitializeInPlace(RAPIDJSON_ALLOCATOR *_allocator) {
    ::new(this) EncoderData(_allocator);
  }

  /// <summary>
  /// Initialize the basic writer according to this encoders current configuration
  /// </summary>
  template<typename OS, typename SE, typename TE, typename SA, unsigned F>
  void Initialize(rapidjson::Writer<OS, SE, TE, SA, F> &writer) {
    writer.SetMaxDecimalPlaces(decimals);
  }

  /// <summary>
  /// Initialize the pretty writer according to this encoders current configuration
  /// </summary>
  template<typename OS, typename SE, typename TE, typename SA, unsigned F>
  void Initialize(rapidjson::PrettyWriter<OS, SE, TE, SA, F> &writer) {
    rapidjson::PrettyFormatOptions prettyFormat = rapidjson::PrettyFormatOptions::kFormatDefault;
    if (flags & JSON_ARRAY_SINGLE_LINE)
      prettyFormat = rapidjson::PrettyFormatOptions::kFormatSingleLineArray;

    writer.SetMaxDecimalPlaces(decimals);
    writer.SetIndent(pretty_indent[indent], static_cast<unsigned>(indent_amt));
    writer.SetFormatOptions(prettyFormat);
  }

  /// <summary>
  /// Encode the object at the given "idx"
  /// </summary>
  template<class Writer>
  int Encode(lua_State *L, int idx, int error_handler_idx = 0, int userdata_idx = 0) {
    LuaSAX::Encoder sax(flags, depth, error_handler_idx, _order);

#if defined(LUA_RAPIDJSON_ANCHOR)
    /*
    ** Allocate the writer w/ the specified allocator, freeing it after
    ** successful encoding (or during a __gc sweep on error).
    */
    Writer *wptr = reinterpret_cast<Writer *>(allocator->Malloc(sizeof(Writer)));
    if (wptr == nullptr) {
      /*
      ** lua_Alloc will throw an error on failure. See the @TODO wrt std::vector
      ** usage.
      **
      ** Unfortunately, much of the rapidjson library is written under the
      ** assumption that allocator::Malloc/Realloc will never return NULL (or
      ** throw exceptions).
      **
      ** @TODO Given that this library ignores the schema/document bits of
      ** rapidjson, its possible to introduce a CrtAllocator implementation that
      ** throws exceptions on failure.
      */
      throw rapidjson::LuaException("writer allocation failed");
    }

    ::new(wptr) Writer(_buffer, allocator);
    writer_ud = reinterpret_cast<void *>(wptr);

    Writer &writer = *wptr;
#else
    /* Writer only needs to reside on the stack; unwinds on error. */
    Writer writer(_buffer, allocator);
#endif

    Initialize(writer);

    /* Encode the object at the first index on the stack  */
    sax.encodeValue(L, writer, idx);

    /* Push encoded contents onto the Lua stack */
    lua_pushlstring(L, _buffer.GetString(), _buffer.GetSize());

    /* Cleanup userdata allocations instead of waiting for GC cycle. */
#if defined(LUA_RAPIDJSON_ANCHOR)
    if (userdata_idx > 0)
      CleanupUserdata(L, userdata_idx);
#else
    JSON_UNUSED(userdata_idx);
#endif
    return 1;
  }

  void CleanupUserdata(lua_State *L, int userdata_idx) {
    if (init) {
      _order.~vector();
      _buffer.~GenericStringBuffer();
      if (writer_ud != nullptr) {
        if (flags & JSON_PRETTY_PRINT)
          reinterpret_cast<EncoderData::Pretty<> *>(writer_ud)->~Pretty<>();
        else
          reinterpret_cast<EncoderData::Basic<> *>(writer_ud)->~Basic<>();

        /*
        ** If LUA_RAPIDJSON_ALLOCATOR is enabled, the allocator state is already
        ** packed in the header of the writer block (per rapidjson requirements)
        */
        RAPIDJSON_ALLOCATOR::Free(writer_ud);
      }

      writer_ud = nullptr;
      init = false;
    }

    lua_pushnil(L);
    lua_setmetatable(L, userdata_idx);
  }

  /// <summary>
  /// lua_error wrapper that deallocates intermediate data prior to invoking lua_error.
  /// </summary>
  RAPIDJSON_FORCEINLINE int _lua_error(lua_State *L, int userdata_idx) {
#if defined(LUA_RAPIDJSON_ANCHOR)
    if (userdata_idx > 0)
      CleanupUserdata(L, userdata_idx);
#else
    JSON_UNUSED(userdata_idx);
#endif
    return lua_error(L);
  }

  /// <summary>
  /// luaL_error wrapper that deallocates intermediate data prior to invoking lua_error.
  /// </summary>
  RAPIDJSON_FORCEINLINE int _luaL_error(lua_State *L, int userdata_idx, const char *msg) {
#if defined(LUA_RAPIDJSON_ANCHOR)
    if (userdata_idx > 0)
      CleanupUserdata(L, userdata_idx);
#else
    JSON_UNUSED(userdata_idx);
#endif
    return luaL_error(L, msg);
  }

  /// <summary>
  /// Garbage collection sweep
  /// </summary>
  static int __gc(lua_State *L) {
    void *udata = luaL_checkudata(L, 1, LUA_RAPIDJSON_ENCODER);
    if (udata != nullptr) {
      reinterpret_cast<EncoderData *>(udata)->CleanupUserdata(L, 1);
    }

    return 0;
  }
};

struct DecoderData {
  bool init;  // Has been constructed in-place
  lua_Integer flags;  // Decoding flags
  lua_Integer parsemode;  // Decoding configuration

  RAPIDJSON_ALLOCATOR *allocator;
  rapidjson::internal::Stack<RAPIDJSON_ALLOCATOR> stack;
  rapidjson::GenericReader<LUA_RAPIDJSON_SOURCE, LUA_RAPIDJSON_TARGET, RAPIDJSON_ALLOCATOR> reader;

  DecoderData(RAPIDJSON_ALLOCATOR *_allocator)
    : init(true), flags(JSON_DEFAULT), parsemode(JSON_DECODE_DEFAULT), allocator(_allocator), stack(_allocator, 0), reader(allocator) {
  }

  /// <summary>
  /// Initialize necessary fields after a rapidjson::Allocator malloc.
  /// </summary>
  RAPIDJSON_FORCEINLINE void Preinitialize() {
    init = false;
    allocator = nullptr;
  }

  /// <summary>
  /// Initialize the Decoder in-place.
  /// </summary>
  RAPIDJSON_FORCEINLINE void InitializeInPlace(RAPIDJSON_ALLOCATOR *_allocator) {
    ::new(this) DecoderData(_allocator);
  }

  /// <summary>
  /// </summary>
  /// <param name="L"></param>
  /// <param name="nullarg">Stack index of object that represents "null"</param>
  /// <param name="objectarg">Stack index of "object" metatable</param>
  /// <param name="arrayarg">Stack index of "array" metatable</param>
  /// <returns></returns>
  rapidjson::ParseResult Decode(lua_State *L, int userdata_idx, const char *contents, size_t len, size_t &position, int nullarg = -1, int objectarg = -1, int arrayarg = -1) {
    rapidjson::ParseResult result = rapidjson::ParseResult(rapidjson::ParseErrorCode::kParseErrorValueInvalid, position);
    if (parsemode == JSON_DECODE_EXTENDED)
      flags |= JSON_NAN_AND_INF;  // Temporary fix for propagating runtime "NanAndInf" checking

    rapidjson::extend::StringStream s(contents + (position - 1), len - (position - 1));
    LuaSAX::Decoder<RAPIDJSON_ALLOCATOR> decoder(L, stack, flags, nullarg, objectarg, arrayarg);
    switch (parsemode) {
      case JSON_DECODE_EXTENDED: {
        result = reader.Parse<rapidjson::ParseFlag::kParseDefaultFlags
#if defined(LUA_RAPIDJSON_COMPAT)
          | rapidjson::ParseFlag::kParseStopWhenDoneFlag
#endif
          | rapidjson::ParseFlag::kParseTrailingCommasFlag
          | rapidjson::ParseFlag::kParseFullPrecisionFlag
          | rapidjson::ParseFlag::kParseCommentsFlag
          | rapidjson::ParseFlag::kParseNanAndInfFlag
          // | rapidjson::ParseFlag::kParseEscapedApostropheFlag // Added 3e21bb429d492206c9ce2f3fd44264a5220913c4
        >(s, decoder);
        break;
      }
      case JSON_DECODE_DEFAULT: {
      default:
        result = reader.Parse<rapidjson::ParseFlag::kParseDefaultFlags
#if defined(LUA_RAPIDJSON_COMPAT)
          | rapidjson::ParseFlag::kParseStopWhenDoneFlag
#endif
          | rapidjson::ParseFlag::kParseTrailingCommasFlag
          | rapidjson::ParseFlag::kParseNanAndInfFlag
        >(s, decoder);
        break;
      }
    }

    position = s.Tell();

    // Cleanup userdata allocations instead of waiting for GC cycle.
#if defined(LUA_RAPIDJSON_ANCHOR)
    if (userdata_idx > 0)
      CleanupUserdata(L, userdata_idx);
#else
    JSON_UNUSED(userdata_idx);
#endif
    return result;
  }

  void CleanupUserdata(lua_State *L, int userdata_idx) {
    if (init) {
      stack.~Stack();
      reader.~GenericReader();

      init = false;
      allocator = nullptr;
    }

    lua_pushnil(L);
    lua_setmetatable(L, userdata_idx);
  }

  /// <summary>
  /// lua_error wrapper that deallocates intermediate data prior to invoking lua_error.
  /// </summary>
  RAPIDJSON_FORCEINLINE int _lua_error(lua_State *L, int userdata_idx) {
#if defined(LUA_RAPIDJSON_ANCHOR)
    if (userdata_idx > 0)
      CleanupUserdata(L, userdata_idx);
#else
    JSON_UNUSED(userdata_idx);
#endif
    return lua_error(L);
  }

  /// <summary>
  /// luaL_error wrapper that deallocates intermediate data prior to invoking lua_error.
  /// </summary>
  RAPIDJSON_FORCEINLINE int _luaL_error(lua_State *L, int userdata_idx, const char *msg) {
#if defined(LUA_RAPIDJSON_ANCHOR)
    if (userdata_idx > 0)
      CleanupUserdata(L, userdata_idx);
#else
    JSON_UNUSED(userdata_idx);
#endif
    return luaL_error(L, msg);
  }

  /// <summary>
  /// Garbage collection sweep
  /// </summary>
  static int __gc(lua_State *L) {
    void *udata = luaL_checkudata(L, 1, LUA_RAPIDJSON_DECODER);
    if (udata != nullptr) {
      reinterpret_cast<DecoderData *>(udata)->CleanupUserdata(L, 1);
    }

    return 0;
  }
};

extern "C" {

LUALIB_API int rapidjson_encode (lua_State *L) {
  int top = 0;
  int key_order_idx = 0; // Stack index of preset key ordering (temporary)
  int error_handler_idx = 0;  // Stack index of error handling function.
  int userdata_idx = 0;  // Stack index of the anchored rapidjson userdata.
  lua_settop(L, 2);  // Ensure second argument is created

  /* Create and anchor a userdata on the stack that manages intermediate rapidjson data */
#if defined(LUA_RAPIDJSON_ANCHOR)
  EncoderData *eud = reinterpret_cast<EncoderData *>(json_newuserdata(L, sizeof(EncoderData)));  // [..., userdata]
  eud->Preinitialize();

  top = userdata_idx = lua_gettop(L);  // Ensure lua_settop(L) contains the userdata
  luaL_getmetatable(L, LUA_RAPIDJSON_ENCODER);  // [..., userdata, metatable]
  lua_setmetatable(L, -2);  // [..., userdata]
#else
  top = lua_gettop(L);
#endif

  /* Parse default options */
  lua_rapidjson_getsubtable(L, LUA_REGISTRYINDEX, LUA_RAPIDJSON_REG);
  lua_Integer flags = geti(L, -1, LUA_RAPIDJSON_REG_FLAGS, JSON_DEFAULT);
  lua_Integer parsemode = geti(L, -1, LUA_RAPIDJSON_REG_PRESET, JSON_DECODE_DEFAULT);
  lua_Integer indent = geti(L, -1, LUA_RAPIDJSON_REG_INDENT, 0);
  lua_Integer indent_amt = geti(L, -1, LUA_RAPIDJSON_REG_INDENT_AMT, (indent == 0) ? 4 : 0);
  int depth = static_cast<int>(geti(L, -1, LUA_RAPIDJSON_REG_DEPTH, LUA_RAPIDJSON_DEFAULT_DEPTH));
  int decimals = static_cast<int>(geti(L, -1, LUA_RAPIDJSON_REG_MAXDEC, LUA_NUMBER_FMT_LEN));
  lua_pop(L, 1);

  if (lua_istable(L, 2)) {  // Parse all options from the additional argument table.
    bool has_key_order = false;
    bool has_exception_handler = false;

    lua_pushnil(L);
    while (lua_next(L, 2)) {  // [..., key, value]
      const lua_Integer opt = option_keys_num[luaL_optcheckoption(L, -2, nullptr, option_keys, 0)];
      switch (opt) {
        case JSON_PRETTY_PRINT:
        case JSON_SORT_KEYS:
        case JSON_LUA_NULL:
        case JSON_UNSIGNED_INTEGERS:
        case JSON_NAN_AND_INF:
        case JSON_ENCODE_INT32:
        case JSON_LUA_DTOA:
        case JSON_LUA_GRISU:
        case JSON_ENCODER_ARRAY_VECTOR:
        case JSON_ARRAY_SINGLE_LINE:
        case JSON_ARRAY_EMPTY:
        case JSON_ARRAY_WITH_HOLES:
          flags = lua_toboolean(L, -1) ? (flags | opt) : (flags & ~opt);
          break;
        case JSON_ENCODER_MAX_DEPTH:
          if ((depth = static_cast<int>(lua_tointeger(L, -1))) <= 0)
            return luaL_error(L, "invalid encoder depth");
          break;
        case JSON_ENCODER_DECIMALS:
          if ((decimals = static_cast<int>(lua_tointeger(L, -1))) <= 0)
            return luaL_error(L, "invalid decimal count");
          break;
        case JSON_ENCODER_INDENT:
          indent = lua_tointeger(L, -1);
          if (indent < 0 || indent >= 4)
            return luaL_error(L, "invalid indentation index");
          break;
        case JSON_ENCODER_INDENT_AMT:
          if ((indent_amt = lua_tointeger(L, -1)) < 0)
            return luaL_error(L, "invalid indentation amount");
          break;
        case JSON_ENCODER_HANDLER:
          has_exception_handler = lua_isfunction(L, -1);
          break;
        case JSON_TABLE_KEY_ORDER: {
          has_key_order = lua_istable(L, -1);
          break;
        }
        default:
          break;
      }
      lua_pop(L, 1);  // [..., key]
    }

    if (has_exception_handler) {
      lua_getfield(L, 2, LUA_RAPIDJSON_STATE_EXCEPTION);  // [... [, userdata] [, exception_handler]]
      error_handler_idx = lua_gettop(L);
    }

    if (has_key_order) {
      lua_getfield(L, 2, LUA_RAPIDJSON_STATE_KEYORDER);  // [... [, userdata] [, exception_handler] [, key_order]]
      key_order_idx = lua_gettop(L);
    }
  }
  else if (!lua_isnoneornil(L, 2)) {
    return luaL_error(L, "Argument 2: table or nothing expected");
  }

  /* Sanitize pretty_print parameters even when not using them. */
  if (indent < 0 || indent >= 4 || depth < 0)
    return luaL_error(L, "invalid encoder parameters");

  bool has_error_string = false;
  try {
    RAPIDJSON_ALLOCATOR_INIT(L, _allocator);
#if defined(LUA_RAPIDJSON_ANCHOR)
    eud->InitializeInPlace(&_allocator);
    EncoderData &encoder = *eud;
#else
    EncoderData encoder(&_allocator);
#endif
    encoder.flags = flags;
    encoder.parsemode = parsemode;
    encoder.indent = indent;
    encoder.indent_amt = indent_amt;
    encoder.depth = depth;
    encoder.decimals = decimals;
    if (key_order_idx > 0) {
      if (LuaSAX::populate_key_vector(L, key_order_idx, encoder._order) != 0)
        throw rapidjson::LuaException("invalid key_order element");
      lua_pop(L, 1);  // [... [, userdata] [, exception_handler]]
    }

    // After encoding: [... [, userdata] [, exception_handler], encoded_string]
    // ldo.moveresults  will cleanup the intermediate arguments.
    if (encoder.flags & JSON_PRETTY_PRINT) {
      if (encoder.flags & JSON_NAN_AND_INF)
        return encoder.Encode<EncoderData::PrettyInf<>>(L, 1, error_handler_idx, userdata_idx);
      else
        return encoder.Encode<EncoderData::Pretty<>>(L, 1, error_handler_idx, userdata_idx);
    }
    else {
      if (encoder.flags & JSON_NAN_AND_INF)
        return encoder.Encode<EncoderData::BasicInf<>>(L, 1, error_handler_idx, userdata_idx);
      else
        return encoder.Encode<EncoderData::Basic<>>(L, 1, error_handler_idx, userdata_idx);
    }
  }
  catch (const rapidjson::LuaCallException &e) {
    has_error_string = e.pushError(L, top);
  }
  catch (const rapidjson::LuaTypeException &e) {
    has_error_string = e.pushError(L, top);
  }
  catch (const std::exception &e) {
    lua_settop(L, top);
    has_error_string = rapidjson::LuaTypeException::_lua_pushstring(L, e.what());
  }
  catch (...) {
    lua_settop(L, top);
  }

  if (!has_error_string)
    lua_pushstring(L, "Unexpected exception");
  return lua_error(L);
}

LUALIB_API int rapidjson_decode (lua_State *L) {
  int top = 0;  // Ensure lua_settop(L) still contains the userdata
  int userdata_idx = 0;  // Stack index of the anchored rapidjson userdata.
  int trailer = 0;  // First argument after the input string/length

  const char *contents = nullptr;  // string being decoded
  size_t len = 0, position = 0;  // Length and offset of decoded string.

  /* Parse decoding configuration */
  lua_rapidjson_getsubtable(L, LUA_REGISTRYINDEX, LUA_RAPIDJSON_REG);
  const lua_Integer flags = geti(L, -1, LUA_RAPIDJSON_REG_FLAGS, JSON_DEFAULT);
  const lua_Integer parsemode = geti(L, -1, LUA_RAPIDJSON_REG_PRESET, JSON_DECODE_DEFAULT);
  lua_pop(L, 1);

  /*
  ** Similar to dkjson, attempt to coerce non-string types to strings.
  ** luaL_checklstring should throw an error when not possible.
  */
  trailer = 2;
  switch (lua_type(L, 1)) {
    case LUA_TNIL: break;
    default: {
      contents = luaL_checklstring(L, 1, &len);
      break;
    }
  }

  /* Parse trailing function arguments */
  int nullarg = -1;  // Stack index of object that represents "null"
  int objectarg = -1;  // Stack index of "object" metatable
  int arrayarg = -1;  // Stack index of "array" metatable

  position = luaL_optsizet(L, trailer, 1);
  nullarg = (lua_gettop(L) >= (trailer + 1)) ? (trailer + 1) : -1;
  objectarg = lua_istable(L, (trailer + 2)) ? (trailer + 2) : -1;
  arrayarg = lua_istable(L, (trailer + 3)) ? (trailer + 3) : -1;

  if (len == 0) {  // Gracefully handle empty strings
    lua_pushnil(L);
    lua_pushinteger(L, 0);
    lua_pushfstring(L, "%s (%d)", rapidjson::GetParseError_En(rapidjson::ParseErrorCode::kParseErrorDocumentEmpty), 0);
    return 3;
  }
  else if (position == 0 || position > len) {  // Optional trailing arguments
    return luaL_error(L, "invalid position");
  }

  /* Function has six potential parameters, setup decoder data after all have been parsed  */
#if defined(LUA_RAPIDJSON_ANCHOR)
  DecoderData *dud = reinterpret_cast<DecoderData *>(json_newuserdata(L, sizeof(DecoderData)));  // [..., userdata]
  dud->Preinitialize();

  top = userdata_idx = lua_gettop(L);
  luaL_getmetatable(L, LUA_RAPIDJSON_DECODER);  // [..., userdata, metatable]
  lua_setmetatable(L, -2);  // [..., userdata]
#else
  top = lua_gettop(L);
#endif

  bool has_error_string = false;
  try {
    RAPIDJSON_ALLOCATOR_INIT(L, _allocator);
#if defined(LUA_RAPIDJSON_ANCHOR)
    dud->InitializeInPlace(&_allocator);
    DecoderData &decoder = *dud;
#else
    DecoderData decoder(&_allocator);
#endif
    decoder.flags = flags;
    decoder.parsemode = parsemode;
    const rapidjson::ParseResult r = decoder.Decode(L, userdata_idx, contents, len, position, nullarg, objectarg, arrayarg);
    if (r.IsError()) {
      lua_settop(L, top);
#if defined(LUA_RAPIDJSON_EXPLICIT)
      lua_pushfstring(L, "%s (%d)", rapidjson::GetParseError_En(r.Code()), r.Offset());
      /* fall outside of try/catch */
#else
      lua_pushnil(L);
      lua_pushinteger(L, static_cast<lua_Integer>(r.Offset()));
      lua_pushfstring(L, "%s (%d)", rapidjson::GetParseError_En(r.Code()), r.Offset());
      return 3;
#endif
    }
    else {
      lua_pushinteger(L, 1 + static_cast<lua_Integer>(position));
      return 2;
    }
  }
  catch (const rapidjson::LuaCallException &e) {
    has_error_string = e.pushError(L, top);
  }
  catch (const rapidjson::LuaTypeException &e) {
    has_error_string = e.pushError(L, top);
  }
  catch (const std::exception &e) {
    lua_settop(L, top);
#if defined(LUA_RAPIDJSON_EXPLICIT)
    has_error_string = rapidjson::LuaTypeException::_lua_pushstring(L, e.what());
#else
    lua_pushnil(L);
    lua_pushinteger(L, -1);
    if (!rapidjson::LuaTypeException::_lua_pushstring(L, e.what()))
      lua_pushnil(L);  // Worst case scenario; push nil or a number (implicitly cvt2str)
    return 3;
#endif
  }
  catch (...) {
    lua_settop(L, top);
  }

  if (!has_error_string)
    lua_pushstring(L, "Unexpected exception");
  return lua_error(L);
}

LUALIB_API int rapidjson_setoption (lua_State *L) {
  lua_Integer v = 0;
  const lua_Integer opt = option_keys_num[luaL_checkoption(L, 1, nullptr, option_keys)];

  lua_settop(L, 2);  // create a 2nd argument if there isn't one
  lua_rapidjson_getsubtable(L, LUA_REGISTRYINDEX, LUA_RAPIDJSON_REG);
  switch (opt) {
    case JSON_PRETTY_PRINT:
    case JSON_SORT_KEYS:
    case JSON_LUA_NULL:
    case JSON_UNSIGNED_INTEGERS:
    case JSON_NAN_AND_INF:
    case JSON_ENCODE_INT32:
    case JSON_LUA_DTOA:
    case JSON_LUA_GRISU:
    case JSON_ENCODER_ARRAY_VECTOR:
    case JSON_ARRAY_SINGLE_LINE:
    case JSON_ARRAY_EMPTY:
    case JSON_ARRAY_WITH_HOLES: {
      v = geti(L, -1, LUA_RAPIDJSON_REG_FLAGS, JSON_DEFAULT);
      luaL_checktype(L, 2, LUA_TBOOLEAN);
      seti(L, -1, LUA_RAPIDJSON_REG_FLAGS, lua_toboolean(L, 2) ? (v | opt) : (v & ~opt));
      break;
    }
    case JSON_ENCODER_MAX_DEPTH:
      if ((v = luaL_checkinteger(L, 2)) > 0)
        seti(L, -1, LUA_RAPIDJSON_REG_DEPTH, v);
      break;
    case JSON_ENCODER_INDENT:
      if ((v = luaL_checkinteger(L, 2)) >= 0 || v < 4)
        seti(L, -1, LUA_RAPIDJSON_REG_INDENT, v);
      break;
    case JSON_ENCODER_INDENT_AMT:
      if ((v = luaL_checkinteger(L, 2)) >= 0)
        seti(L, -1, LUA_RAPIDJSON_REG_INDENT_AMT, v);
      break;
    case JSON_ENCODER_DECIMALS:
      if ((v = luaL_checkinteger(L, 2)) >= 0)
        seti(L, -1, LUA_RAPIDJSON_REG_MAXDEC, v);
      break;
    case JSON_DECODER_PRESET:
      v = decode_presets_num[luaL_optcheckoption(L, 2, nullptr, decode_presets, 0)];
      seti(L, -1, LUA_RAPIDJSON_REG_PRESET, v);
      break;
    default:
      break;
  }
  lua_pop(L, 1);
  return 0;
}

LUALIB_API int rapidjson_getoption (lua_State *L) {
  lua_Integer v = 0;
  const lua_Integer opt = option_keys_num[luaL_checkoption(L, 1, nullptr, option_keys)];

  lua_rapidjson_getsubtable(L, LUA_REGISTRYINDEX, LUA_RAPIDJSON_REG);  // [..., reg]
  switch (opt) {
    case JSON_PRETTY_PRINT:
    case JSON_SORT_KEYS:
    case JSON_LUA_NULL:
    case JSON_UNSIGNED_INTEGERS:
    case JSON_NAN_AND_INF:
    case JSON_ENCODE_INT32:
    case JSON_LUA_DTOA:
    case JSON_LUA_GRISU:
    case JSON_ENCODER_ARRAY_VECTOR:
    case JSON_ARRAY_SINGLE_LINE:
    case JSON_ARRAY_EMPTY:
    case JSON_ARRAY_WITH_HOLES:
      v = geti(L, -1, LUA_RAPIDJSON_REG_FLAGS, JSON_DEFAULT);
      lua_pushboolean(L, (v & opt) != 0);  // [..., reg, flag]
      break;
    case JSON_ENCODER_MAX_DEPTH:
      lua_pushinteger(L, geti(L, -1, LUA_RAPIDJSON_REG_DEPTH, LUA_RAPIDJSON_DEFAULT_DEPTH));  // [..., reg, depth]
      break;
    case JSON_ENCODER_INDENT:
      lua_pushinteger(L, geti(L, -1, LUA_RAPIDJSON_REG_INDENT, 0));  // [..., reg, indent]
      break;
    case JSON_ENCODER_INDENT_AMT:
      lua_pushinteger(L, geti(L, -1, LUA_RAPIDJSON_REG_INDENT_AMT, 0));  // [..., reg, amount]
      break;
    case JSON_ENCODER_DECIMALS:
      v = geti(L, -1, LUA_RAPIDJSON_REG_MAXDEC, rapidjson::Writer<rapidjson::StringBuffer>::kDefaultMaxDecimalPlaces);
      lua_pushinteger(L, v);  // [..., reg, decimals]
      break;
    case JSON_DECODER_PRESET: {
      v = geti(L, -1, LUA_RAPIDJSON_REG_PRESET, JSON_DECODE_DEFAULT);
      if (JSON_DECODE_DEFAULT <= v && v <= JSON_DECODE_EXTENDED)
        lua_pushstring(L, decode_presets[v]);  // [..., reg, preset]
      else
        lua_pushnil(L);
      break;
    }
    default: {
      lua_pop(L, 1);
      return 0;
    }
  }
  return 1;
}

LUALIB_API int rapidjson_object (lua_State *L) {
  make_table_type(L, 1, LUA_RAPIDJSON_REG_OBJECT, LUA_RAPIDJSON_META_TYPE_OBJECT);
  return 1;
}

LUALIB_API int rapidjson_array (lua_State *L) {
  make_table_type(L, 1, LUA_RAPIDJSON_REG_ARRAY, LUA_RAPIDJSON_META_TYPE_ARRAY);
  return 1;
}

LUALIB_API int rapidjson_isobject (lua_State *L) {
  bool is_array = 0;
  lua_pushboolean(L, has_json_type(L, 1, &is_array) && !is_array);
  return 1;
}

LUALIB_API int rapidjson_isarray (lua_State *L) {
  bool is_array = 0;
  lua_pushboolean(L, has_json_type(L, 1, &is_array) && is_array);
  return 1;
}

static int rapidjson_use_lpeg (lua_State *L) {
  return luaL_error(L, "use_lpeg has been deprecated!");
}

#if defined(LUA_RAPIDJSON_ANCHOR)
static void rapidjson_create_anchor (lua_State *L, const char *name, const luaL_Reg *l) {
  if (luaL_newmetatable(L, name)) {
    luaL_setfuncs(L, l, 0);
  }
  lua_pop(L, 1);  /* pop metatable */
}
#endif

LUAMOD_API int luaopen_rapidjson (lua_State *L) {
  static const luaL_Reg luajson_lib[] = {
    { "decode", rapidjson_decode },
    { "encode", rapidjson_encode },
    { "setoption", rapidjson_setoption },
    { "getoption", rapidjson_getoption },
    /* special tags and functions */
    { "null", json_null },
    { "object", rapidjson_object },
    { "array", rapidjson_array },
    { "isobject", rapidjson_isobject },
    { "isarray", rapidjson_isarray },
    { "use_lpeg", rapidjson_use_lpeg },
    /* library details */
    { "_NAME", nullptr },
    { "_VERSION", nullptr },
    { "_COPYRIGHT", nullptr },
    { "_DESCRIPTION", nullptr },
    { "_COMPAT", nullptr },
    { nullptr, nullptr }
  };

#if defined(LUA_RAPIDJSON_ANCHOR)
  static luaL_Reg rapidjson_encode_anchor[] {
    { "__gc", EncoderData::__gc },
    { "__close", EncoderData::__gc },
    { nullptr, nullptr },
  };

  static luaL_Reg rapidjson_decode_anchor[] {
    { "__gc", DecoderData::__gc },
    { "__close", DecoderData::__gc },
    { nullptr, nullptr },
  };

  rapidjson_create_anchor(L, LUA_RAPIDJSON_ENCODER, rapidjson_encode_anchor);
  rapidjson_create_anchor(L, LUA_RAPIDJSON_DECODER, rapidjson_decode_anchor);
#endif

  create_shared_meta(L, LUA_RAPIDJSON_REG_ARRAY, LUA_RAPIDJSON_META_TYPE_ARRAY);
  create_shared_meta(L, LUA_RAPIDJSON_REG_OBJECT, LUA_RAPIDJSON_META_TYPE_OBJECT);

  luaL_newlib(L, luajson_lib);

  lua_pushboolean(L, 0); lua_setfield(L, -2, "using_lpeg");
  lua_pushliteral(L, LUA_RAPIDJSON_NAME); lua_setfield(L, -2, "_NAME");
  lua_pushliteral(L, LUA_RAPIDJSON_VERSION); lua_setfield(L, -2, "_VERSION");
  lua_pushliteral(L, LUA_RAPIDJSON_COPYRIGHT); lua_setfield(L, -2, "_COPYRIGHT");
  lua_pushliteral(L, LUA_RAPIDJSON_DESCRIPTION); lua_setfield(L, -2, "_DESCRIPTION");
#if defined(LUA_RAPIDJSON_COMPAT)
  lua_pushboolean(L, 1); lua_setfield(L, -2, "_COMPAT");
#else
  lua_pushboolean(L, 0); lua_setfield(L, -2, "_COMPAT");
#endif

  return 1;
}

}
