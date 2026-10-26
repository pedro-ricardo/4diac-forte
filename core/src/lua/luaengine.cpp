/*******************************************************************************
 * Copyright (c) 2015, 2026 fortiss GmbH, Johannes Kepler University Linz, Aimirim STI
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0
 *
 * Contributors:
 *   Martin Jobst - initial API and implementation and/or initial documentation
 *   Alois Zoitl  - upgraded to new FB memory layout
 *   Pedro Ricardo C. Souza - sandboxed modules, IEC value mapping for ARRAY and STRUCT
 *******************************************************************************/

#include "luaengine.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <luajit.h>
}

#include <cmath>

#include "stfunc_source.h"

#include "forte/datatypes/forte_array.h"
#include "forte/datatypes/forte_bool.h"
#include "forte/datatypes/forte_byte.h"
#include "forte/datatypes/forte_char.h"
#include "forte/datatypes/forte_date.h"
#include "forte/datatypes/forte_date_and_time.h"
#include "forte/datatypes/forte_dint.h"
#include "forte/datatypes/forte_dword.h"
#include "forte/datatypes/forte_int.h"
#include "forte/datatypes/forte_ldate.h"
#include "forte/datatypes/forte_ldate_and_time.h"
#include "forte/datatypes/forte_lint.h"
#include "forte/datatypes/forte_lreal.h"
#include "forte/datatypes/forte_ltime.h"
#include "forte/datatypes/forte_ltime_of_day.h"
#include "forte/datatypes/forte_lword.h"
#include "forte/datatypes/forte_real.h"
#include "forte/datatypes/forte_sint.h"
#include "forte/datatypes/forte_string.h"
#include "forte/datatypes/forte_struct.h"
#include "forte/datatypes/forte_time.h"
#include "forte/datatypes/forte_time_of_day.h"
#include "forte/datatypes/forte_udint.h"
#include "forte/datatypes/forte_uint.h"
#include "forte/datatypes/forte_ulint.h"
#include "forte/datatypes/forte_usint.h"
#include "forte/datatypes/forte_wchar.h"
#include "forte/datatypes/forte_word.h"
#include "forte/datatypes/forte_wstring.h"
#include "forte/util/devlog.h"

#ifndef FORTE_LUA_INSTRUCTION_LIMIT
#define FORTE_LUA_INSTRUCTION_LIMIT 0
#endif

namespace forte::lua {
  namespace {
    /* Runs once per state with the full base library. Receives the STfunc source and returns the sandbox
     * factory and the read only STfunc view. Every name a module can reach is captured here, so nothing a
     * module does to its own environment leaks into another module or into STfunc. */
    constexpr char cmBootstrap[] = R"lua(
local stfuncSource = ...
local error, ipairs, pairs, setmetatable, tostring = error, ipairs, pairs, setmetatable, tostring

local safe = {}
for _, name in ipairs({"assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget", "rawset",
                       "select", "setmetatable", "tonumber", "tostring", "type", "unpack", "xpcall"}) do
  safe[name] = _G[name]
end

local function copy(t)
  local c = {}
  for k, v in pairs(t) do
    c[k] = v
  end
  return c
end

local libraries = {string = copy(string), table = copy(table), math = copy(math), bit = copy(bit)}
libraries.string.dump = nil
libraries.table.unpack = libraries.table.unpack or unpack

local function readonly(t, name)
  return setmetatable({}, {
    __index = t,
    __newindex = function() error("attempt to modify " .. name, 2) end,
    __metatable = false
  })
end

local function newEnv(modules)
  local env = copy(safe)
  for name, lib in pairs(libraries) do
    env[name] = copy(lib)
  end
  env.require = function(name)
    local m = modules[name] or (name == "bit" and env.bit)
    if not m then
      error("module '" .. tostring(name) .. "' not found", 2)
    end
    return m
  end
  env._G = env
  return env
end

local chunk = assert(loadstring(stfuncSource, "=STfunc"))
setfenv(chunk, newEnv({}))
local stfunc = chunk()
if type(stfunc) ~= "table" then
  error("STfunc does not return a table")
end
local stfuncView = readonly(stfunc, "STfunc")
local modules = {STfunc = stfuncView}

return function() return newEnv(modules) end, stfuncView
)lua";

    // After the budget ran out every further instruction fails, so a pcall inside the module cannot swallow it
    void budgetHook(lua_State *paState, lua_Debug *) {
      lua_sethook(paState, budgetHook, LUA_MASKCOUNT, 1);
      luaL_error(paState, "instruction limit exceeded");
    }

    //! Runs the chunk at 1 in the environment the sandbox factory at 2 returns, returns the chunk's first result
    int runInSandbox(lua_State *paState) {
      lua_call(paState, 0, 1);
      lua_setfenv(paState, 1);
      lua_call(paState, 0, 1);
      return 1;
    }

    int setUpState(lua_State *paState) {
      static const struct {
          const char *mName;
          lua_CFunction mOpen;
      } libraries[] = {{"", luaopen_base},
                       {LUA_STRLIBNAME, luaopen_string},
                       {LUA_TABLIBNAME, luaopen_table},
                       {LUA_MATHLIBNAME, luaopen_math},
                       {LUA_BITLIBNAME, luaopen_bit},
                       {LUA_JITLIBNAME, luaopen_jit}};
      for (const auto &library : libraries) {
        lua_pushcfunction(paState, library.mOpen);
        lua_pushstring(paState, library.mName);
        lua_call(paState, 1, 0);
      }
      if (luaL_loadbuffer(paState, cmBootstrap, sizeof(cmBootstrap) - 1, "=forte") != 0) {
        return lua_error(paState);
      }
      lua_pushlstring(paState, reinterpret_cast<const char *>(cgSTfuncSource), sizeof(cgSTfuncSource) - 1);
      lua_call(paState, 1, 2);
      return 2;
    }

    // Truncates toward zero and wraps into 64 bits, the C conversion of a wide integer
    bool toBits(lua_Number paNumber, TForteUInt64 &paBits) {
      constexpr lua_Number cTwo63 = 9223372036854775808.0;
      constexpr lua_Number cTwo64 = 18446744073709551616.0;
      if (!std::isfinite(paNumber)) {
        return false;
      }
      lua_Number value = std::trunc(paNumber);
      if (value >= cTwo64 || value < -cTwo63) {
        value = std::fmod(value, cTwo64);
        if (value < 0) {
          value += cTwo64;
        }
      }
      if (value >= cTwo63) {
        paBits = static_cast<TForteUInt64>(value);
      } else {
        paBits = static_cast<TForteUInt64>(static_cast<TForteInt64>(value));
      }
      return true;
    }

    template<typename T>
    void pushNumber(lua_State *paState, const CIEC_ANY &paValue) {
      lua_pushnumber(paState,
                     static_cast<lua_Number>(static_cast<typename T::TValueType>(static_cast<const T &>(paValue))));
    }

    template<typename T>
    const char *getInteger(lua_State *paState, int paIndex, CIEC_ANY &paValue) {
      TForteUInt64 bits;
      if (lua_type(paState, paIndex) != LUA_TNUMBER) {
        return "a number is expected";
      }
      if (!toBits(lua_tonumber(paState, paIndex), bits)) {
        return "the number is not finite";
      }
      static_cast<T &>(paValue) = T(static_cast<typename T::TValueType>(bits));
      return nullptr;
    }

    template<typename T>
    const char *getReal(lua_State *paState, int paIndex, CIEC_ANY &paValue) {
      if (lua_type(paState, paIndex) != LUA_TNUMBER) {
        return "a number is expected";
      }
      static_cast<T &>(paValue) = T(static_cast<typename T::TValueType>(lua_tonumber(paState, paIndex)));
      return nullptr;
    }

    // Decodes one UTF-8 sequence of the basic multilingual plane, returns its length or 0 if malformed
    size_t decodeUtf8(const char *paText, size_t paLength, TForteWChar &paChar) {
      const auto *bytes = reinterpret_cast<const unsigned char *>(paText);
      if (paLength >= 1 && bytes[0] < 0x80) {
        paChar = bytes[0];
        return 1;
      }
      if (paLength >= 2 && (bytes[0] & 0xE0) == 0xC0 && (bytes[1] & 0xC0) == 0x80) {
        paChar = static_cast<TForteWChar>(((bytes[0] & 0x1F) << 6) | (bytes[1] & 0x3F));
        return (paChar >= 0x80) ? 2 : 0;
      }
      if (paLength >= 3 && (bytes[0] & 0xF0) == 0xE0 && (bytes[1] & 0xC0) == 0x80 && (bytes[2] & 0xC0) == 0x80) {
        paChar = static_cast<TForteWChar>(((bytes[0] & 0x0F) << 12) | ((bytes[1] & 0x3F) << 6) | (bytes[2] & 0x3F));
        return (paChar >= 0x800 && (paChar < 0xD800 || paChar > 0xDFFF)) ? 3 : 0;
      }
      return 0;
    }

    bool isUtf8Bmp(const char *paText, size_t paLength) {
      TForteWChar character;
      while (paLength > 0) {
        size_t used = decodeUtf8(paText, paLength, character);
        if (used == 0) {
          return false;
        }
        paText += used;
        paLength -= used;
      }
      return true;
    }

    void pushWChar(lua_State *paState, TForteWChar paChar) {
      char buffer[3];
      size_t length;
      if (paChar < 0x80) {
        buffer[0] = static_cast<char>(paChar);
        length = 1;
      } else if (paChar < 0x800) {
        buffer[0] = static_cast<char>(0xC0 | (paChar >> 6));
        buffer[1] = static_cast<char>(0x80 | (paChar & 0x3F));
        length = 2;
      } else {
        buffer[0] = static_cast<char>(0xE0 | (paChar >> 12));
        buffer[1] = static_cast<char>(0x80 | ((paChar >> 6) & 0x3F));
        buffer[2] = static_cast<char>(0x80 | (paChar & 0x3F));
        length = 3;
      }
      lua_pushlstring(paState, buffer, length);
    }

    const char *getString(lua_State *paState, int paIndex, const char *&paText, size_t &paLength) {
      if (lua_type(paState, paIndex) != LUA_TSTRING) {
        return "a string is expected";
      }
      paText = lua_tolstring(paState, paIndex, &paLength);
      if (paLength > CIEC_ANY_STRING::scmMaxStringLen) {
        return "the string is too long";
      }
      return nullptr;
    }
  } // namespace

  const unsigned int CLuaEngine::scmDefaultInstructionLimit = FORTE_LUA_INSTRUCTION_LIMIT;

  CLuaEngine::CLuaEngine(unsigned int paInstructionLimit) :
      mState(luaL_newstate()),
      mInstructionLimit(paInstructionLimit),
      mSandboxFactoryRef(LUA_NOREF),
      mSTfuncRef(LUA_NOREF),
      mValid(false) {
    if (mState == nullptr) {
      DEVLOG_ERROR("[Lua] could not create a Lua state\n");
      return;
    }
    lua_atpanic(mState, panic);
    mValid = setUp();
  }

  CLuaEngine::~CLuaEngine() {
    if (mState != nullptr) {
      lua_close(mState);
    }
  }

  bool CLuaEngine::setUp() {
    lua_pushcfunction(mState, setUpState);
    if (lua_pcall(mState, 0, 2, 0) != 0) {
      const char *message = lua_tostring(mState, -1);
      DEVLOG_ERROR("[Lua] could not set up the Lua state: %s\n", (message != nullptr) ? message : "unknown error");
      lua_settop(mState, 0);
      return false;
    }
    mSTfuncRef = luaL_ref(mState, LUA_REGISTRYINDEX);
    mSandboxFactoryRef = luaL_ref(mState, LUA_REGISTRYINDEX);
    if (mInstructionLimit > 0) {
      // compiled traces do not run hooks, so a budget needs the interpreter
      luaJIT_setmode(mState, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
    }
    return true;
  }

  bool CLuaEngine::loadModule(std::string_view paSource, const char *paChunkName, std::string &paError) {
    if (!mValid) {
      paError = "the Lua state is not available";
      return false;
    }
    const int top = lua_gettop(mState);
    std::string chunkName("=");
    chunkName += paChunkName;
    // "t": text only, precompiled byte code could break the VM
    if (luaL_loadbufferx(mState, paSource.data(), paSource.size(), chunkName.c_str(), "t") != 0) {
      const char *message = lua_tostring(mState, -1);
      paError = (message != nullptr) ? message : "syntax error";
      lua_settop(mState, top);
      return false;
    }
    // the sandbox is built in protected mode as well, it runs Lua code and allocates
    lua_pushcfunction(mState, runInSandbox);
    lua_insert(mState, -2);
    pushRef(mSandboxFactoryRef);
    if (!call(2, 1, paError)) {
      lua_settop(mState, top);
      return false;
    }
    if (!lua_istable(mState, -1)) {
      paError = "the module does not return a table";
      lua_settop(mState, top);
      return false;
    }
    return true;
  }

  bool CLuaEngine::pushTypeModule(const std::shared_ptr<const void> &paType,
                                  std::string_view paSource,
                                  const char *paChunkName,
                                  std::string &paError) {
    releaseExpiredTypeModules();
    if (auto it = mTypeModules.find(paType.get()); it != mTypeModules.end()) {
      pushRef(it->second.mRef);
      return true;
    }
    if (!loadModule(paSource, paChunkName, paError)) {
      return false;
    }
    lua_pushvalue(mState, -1);
    mTypeModules.emplace(paType.get(), STypeModule{.mType = paType, .mRef = ref()});
    return true;
  }

  void CLuaEngine::releaseExpiredTypeModules() {
    // an expired entry's address may be reused by a new type, so its table must go before the next lookup
    for (auto it = mTypeModules.begin(); it != mTypeModules.end();) {
      if (it->second.mType.expired()) {
        unref(it->second.mRef);
        it = mTypeModules.erase(it);
      } else {
        ++it;
      }
    }
  }

  bool CLuaEngine::call(int paNumArgs, int paNumResults, std::string &paError) {
    const int base = lua_gettop(mState) - paNumArgs;
    lua_pushcfunction(mState, traceback);
    lua_insert(mState, base);
    armBudget();
    const int status = lua_pcall(mState, paNumArgs, paNumResults, base);
    disarmBudget();
    if (status != 0) {
      const char *message = lua_tostring(mState, -1);
      paError = (message != nullptr) ? message : "error object is not a string";
      lua_settop(mState, base - 1);
      return false;
    }
    lua_remove(mState, base);
    return true;
  }

  int CLuaEngine::ref() {
    return luaL_ref(mState, LUA_REGISTRYINDEX);
  }

  void CLuaEngine::unref(int paRef) {
    luaL_unref(mState, LUA_REGISTRYINDEX, paRef);
  }

  void CLuaEngine::pushRef(int paRef) {
    lua_rawgeti(mState, LUA_REGISTRYINDEX, paRef);
  }

  void CLuaEngine::pushSTfunc() {
    pushRef(mSTfuncRef);
  }

  void CLuaEngine::armBudget() {
    if (mInstructionLimit > 0) {
      lua_sethook(mState, budgetHook, LUA_MASKCOUNT, static_cast<int>(mInstructionLimit));
    }
  }

  void CLuaEngine::disarmBudget() {
    if (mInstructionLimit > 0) {
      lua_sethook(mState, nullptr, 0, 0);
    }
  }

  int CLuaEngine::panic(lua_State *paState) {
    const char *message = lua_tostring(paState, -1);
    DEVLOG_ERROR("[Lua] unprotected error: %s\n", (message != nullptr) ? message : "unknown error");
    return 0;
  }

  int CLuaEngine::traceback(lua_State *paState) {
    const char *message = lua_tostring(paState, 1);
    luaL_traceback(paState, paState, (message != nullptr) ? message : "error object is not a string", 1);
    return 1;
  }

  void CLuaEngine::pushValue(lua_State *paState, const CIEC_ANY &paValue) {
    const CIEC_ANY &value = paValue.unwrap();
    switch (value.getDataTypeID()) {
      case CIEC_ANY::e_BOOL: lua_pushboolean(paState, static_cast<bool>(static_cast<const CIEC_BOOL &>(value))); break;
      case CIEC_ANY::e_SINT: pushNumber<CIEC_SINT>(paState, value); break;
      case CIEC_ANY::e_INT: pushNumber<CIEC_INT>(paState, value); break;
      case CIEC_ANY::e_DINT: pushNumber<CIEC_DINT>(paState, value); break;
      case CIEC_ANY::e_LINT: pushNumber<CIEC_LINT>(paState, value); break;
      case CIEC_ANY::e_USINT: pushNumber<CIEC_USINT>(paState, value); break;
      case CIEC_ANY::e_UINT: pushNumber<CIEC_UINT>(paState, value); break;
      case CIEC_ANY::e_UDINT: pushNumber<CIEC_UDINT>(paState, value); break;
      case CIEC_ANY::e_ULINT: pushNumber<CIEC_ULINT>(paState, value); break;
      case CIEC_ANY::e_BYTE: pushNumber<CIEC_BYTE>(paState, value); break;
      case CIEC_ANY::e_WORD: pushNumber<CIEC_WORD>(paState, value); break;
      case CIEC_ANY::e_DWORD: pushNumber<CIEC_DWORD>(paState, value); break;
      case CIEC_ANY::e_LWORD: pushNumber<CIEC_LWORD>(paState, value); break;
      case CIEC_ANY::e_DATE: pushNumber<CIEC_DATE>(paState, value); break;
      case CIEC_ANY::e_TIME_OF_DAY: pushNumber<CIEC_TIME_OF_DAY>(paState, value); break;
      case CIEC_ANY::e_DATE_AND_TIME: pushNumber<CIEC_DATE_AND_TIME>(paState, value); break;
      case CIEC_ANY::e_TIME: pushNumber<CIEC_TIME>(paState, value); break;
      case CIEC_ANY::e_LDATE: pushNumber<CIEC_LDATE>(paState, value); break;
      case CIEC_ANY::e_LTIME_OF_DAY: pushNumber<CIEC_LTIME_OF_DAY>(paState, value); break;
      case CIEC_ANY::e_LDATE_AND_TIME: pushNumber<CIEC_LDATE_AND_TIME>(paState, value); break;
      case CIEC_ANY::e_LTIME: pushNumber<CIEC_LTIME>(paState, value); break;
      case CIEC_ANY::e_REAL: pushNumber<CIEC_REAL>(paState, value); break;
      case CIEC_ANY::e_LREAL: pushNumber<CIEC_LREAL>(paState, value); break;
      case CIEC_ANY::e_CHAR: {
        const char character = static_cast<char>(static_cast<TForteChar>(static_cast<const CIEC_CHAR &>(value)));
        lua_pushlstring(paState, &character, 1);
        break;
      }
      case CIEC_ANY::e_WCHAR:
        pushWChar(paState, static_cast<TForteWChar>(static_cast<const CIEC_WCHAR &>(value)));
        break;
      case CIEC_ANY::e_STRING: {
        const auto &string = static_cast<const CIEC_STRING &>(value);
        lua_pushlstring(paState, string.c_str(), string.length());
        break;
      }
      case CIEC_ANY::e_WSTRING: {
        const auto &string = static_cast<const CIEC_WSTRING &>(value);
        lua_pushlstring(paState, string.getValue(), string.length());
        break;
      }
      case CIEC_ANY::e_ARRAY: {
        const auto &array = static_cast<const CIEC_ARRAY &>(value);
        lua_createtable(paState, static_cast<int>(array.size()), 2);
        for (intmax_t i = array.getLowerBound(), upper = array.getUpperBound(); i <= upper; ++i) {
          pushValue(paState, array[i]);
          lua_rawseti(paState, -2, static_cast<int>(i));
        }
        lua_pushnumber(paState, static_cast<lua_Number>(array.getLowerBound()));
        lua_setfield(paState, -2, "lo");
        lua_pushnumber(paState, static_cast<lua_Number>(array.getUpperBound()));
        lua_setfield(paState, -2, "up");
        break;
      }
      case CIEC_ANY::e_STRUCT: {
        const auto &structure = static_cast<const CIEC_STRUCT &>(value);
        const size_t size = structure.getStructSize();
        lua_createtable(paState, 0, static_cast<int>(size));
        for (size_t i = 0; i < size; ++i) {
          pushValue(paState, *structure.getMember(i));
          lua_setfield(paState, -2, structure.elementNames()[i].data());
        }
        break;
      }
      default: lua_pushnil(paState); break;
    }
  }

  const char *CLuaEngine::getValue(lua_State *paState, int paIndex, CIEC_ANY &paValue) {
    if (paIndex < 0 && paIndex > LUA_REGISTRYINDEX) {
      paIndex = lua_gettop(paState) + paIndex + 1;
    }
    CIEC_ANY &value = paValue.unwrap();
    switch (value.getDataTypeID()) {
      case CIEC_ANY::e_BOOL:
        // ANY_BIT functions of STfunc answer numbers, so a number is a BOOL as well
        if (lua_type(paState, paIndex) == LUA_TBOOLEAN) {
          static_cast<CIEC_BOOL &>(value) = CIEC_BOOL(lua_toboolean(paState, paIndex) != 0);
        } else if (lua_type(paState, paIndex) == LUA_TNUMBER) {
          static_cast<CIEC_BOOL &>(value) = CIEC_BOOL(lua_tonumber(paState, paIndex) != 0);
        } else {
          return "a boolean is expected";
        }
        return nullptr;
      case CIEC_ANY::e_SINT: return getInteger<CIEC_SINT>(paState, paIndex, value);
      case CIEC_ANY::e_INT: return getInteger<CIEC_INT>(paState, paIndex, value);
      case CIEC_ANY::e_DINT: return getInteger<CIEC_DINT>(paState, paIndex, value);
      case CIEC_ANY::e_LINT: return getInteger<CIEC_LINT>(paState, paIndex, value);
      case CIEC_ANY::e_USINT: return getInteger<CIEC_USINT>(paState, paIndex, value);
      case CIEC_ANY::e_UINT: return getInteger<CIEC_UINT>(paState, paIndex, value);
      case CIEC_ANY::e_UDINT: return getInteger<CIEC_UDINT>(paState, paIndex, value);
      case CIEC_ANY::e_ULINT: return getInteger<CIEC_ULINT>(paState, paIndex, value);
      case CIEC_ANY::e_BYTE: return getInteger<CIEC_BYTE>(paState, paIndex, value);
      case CIEC_ANY::e_WORD: return getInteger<CIEC_WORD>(paState, paIndex, value);
      case CIEC_ANY::e_DWORD: return getInteger<CIEC_DWORD>(paState, paIndex, value);
      case CIEC_ANY::e_LWORD: return getInteger<CIEC_LWORD>(paState, paIndex, value);
      case CIEC_ANY::e_DATE: return getInteger<CIEC_DATE>(paState, paIndex, value);
      case CIEC_ANY::e_TIME_OF_DAY: return getInteger<CIEC_TIME_OF_DAY>(paState, paIndex, value);
      case CIEC_ANY::e_DATE_AND_TIME: return getInteger<CIEC_DATE_AND_TIME>(paState, paIndex, value);
      case CIEC_ANY::e_TIME: return getInteger<CIEC_TIME>(paState, paIndex, value);
      case CIEC_ANY::e_LDATE: return getInteger<CIEC_LDATE>(paState, paIndex, value);
      case CIEC_ANY::e_LTIME_OF_DAY: return getInteger<CIEC_LTIME_OF_DAY>(paState, paIndex, value);
      case CIEC_ANY::e_LDATE_AND_TIME: return getInteger<CIEC_LDATE_AND_TIME>(paState, paIndex, value);
      case CIEC_ANY::e_LTIME: return getInteger<CIEC_LTIME>(paState, paIndex, value);
      case CIEC_ANY::e_REAL: return getReal<CIEC_REAL>(paState, paIndex, value);
      case CIEC_ANY::e_LREAL: return getReal<CIEC_LREAL>(paState, paIndex, value);
      case CIEC_ANY::e_CHAR: {
        const char *text = nullptr;
        size_t length = 0;
        if (const char *error = getString(paState, paIndex, text, length); error != nullptr) {
          return error;
        }
        if (length != 1) {
          return "a string of one character is expected";
        }
        static_cast<CIEC_CHAR &>(value) = CIEC_CHAR(static_cast<TForteChar>(text[0]));
        return nullptr;
      }
      case CIEC_ANY::e_WCHAR: {
        const char *text = nullptr;
        size_t length = 0;
        TForteWChar character = 0;
        if (const char *error = getString(paState, paIndex, text, length); error != nullptr) {
          return error;
        }
        if (length == 0 || decodeUtf8(text, length, character) != length) {
          return "a string of one character is expected";
        }
        static_cast<CIEC_WCHAR &>(value) = CIEC_WCHAR(character);
        return nullptr;
      }
      case CIEC_ANY::e_STRING: {
        const char *text = nullptr;
        size_t length = 0;
        if (const char *error = getString(paState, paIndex, text, length); error != nullptr) {
          return error;
        }
        static_cast<CIEC_STRING &>(value).assign(text, static_cast<TForteUInt16>(length));
        return nullptr;
      }
      case CIEC_ANY::e_WSTRING: {
        const char *text = nullptr;
        size_t length = 0;
        if (const char *error = getString(paState, paIndex, text, length); error != nullptr) {
          return error;
        }
        if (!isUtf8Bmp(text, length)) {
          return "the string is not UTF-8 of the basic multilingual plane";
        }
        static_cast<CIEC_WSTRING &>(value).assign(text, static_cast<TForteUInt16>(length));
        return nullptr;
      }
      case CIEC_ANY::e_ARRAY: {
        if (!lua_istable(paState, paIndex)) {
          return "a table is expected for an ARRAY";
        }
        auto &array = static_cast<CIEC_ARRAY &>(value);
        for (intmax_t i = array.getLowerBound(), upper = array.getUpperBound(); i <= upper; ++i) {
          lua_rawgeti(paState, paIndex, static_cast<int>(i));
          const char *error = lua_isnil(paState, -1) ? nullptr : getValue(paState, -1, array[i]);
          lua_pop(paState, 1);
          if (error != nullptr) {
            return error;
          }
        }
        return nullptr;
      }
      case CIEC_ANY::e_STRUCT: {
        if (!lua_istable(paState, paIndex)) {
          return "a table is expected for a STRUCT";
        }
        auto &structure = static_cast<CIEC_STRUCT &>(value);
        for (size_t i = 0, size = structure.getStructSize(); i < size; ++i) {
          lua_pushstring(paState, structure.elementNames()[i].data());
          lua_rawget(paState, paIndex);
          const char *error = lua_isnil(paState, -1) ? nullptr : getValue(paState, -1, *structure.getMember(i));
          lua_pop(paState, 1);
          if (error != nullptr) {
            return error;
          }
        }
        return nullptr;
      }
      default: return "the IEC data type is not supported in Lua";
    }
  }

} // namespace forte::lua
