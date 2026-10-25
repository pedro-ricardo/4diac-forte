/*******************************************************************************
 * Copyright (c) 2026 Aimirim STI
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0
 *
 * Contributors:
 *   Pedro Ricardo C. Souza - initial implementation
 *******************************************************************************/

#include "luainterface.h"

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

#include "forte/datatypes/forte_array_dynamic.h"
#include "../typelib_internal.h"

namespace forte::lua {
  namespace {
    int toAbsIndex(lua_State *paState, int paIndex) {
      return (paIndex > 0 || paIndex <= LUA_REGISTRYINDEX) ? paIndex : lua_gettop(paState) + paIndex + 1;
    }

    //! Pushes paTable[paKey] without invoking metamethods and returns its Lua type
    int pushField(lua_State *paState, int paTable, const char *paKey) {
      lua_pushstring(paState, paKey);
      lua_rawget(paState, paTable);
      return lua_type(paState, -1);
    }

    bool toInteger(lua_State *paState, int paIndex, intmax_t paMin, intmax_t paMax, intmax_t &paValue) {
      if (lua_type(paState, paIndex) != LUA_TNUMBER) {
        return false;
      }
      const lua_Number number = lua_tonumber(paState, paIndex);
      if (number != std::floor(number) || number < static_cast<lua_Number>(paMin) ||
          number > static_cast<lua_Number>(paMax)) {
        return false;
      }
      paValue = static_cast<intmax_t>(number);
      return true;
    }

    bool isIdentifier(std::string_view paName) {
      if (paName.empty() || std::isdigit(static_cast<unsigned char>(paName.front()))) {
        return false;
      }
      return std::all_of(paName.begin(), paName.end(),
                         [](char paChar) { return std::isalnum(static_cast<unsigned char>(paChar)) || paChar == '_'; });
    }

    //! A type name, optionally qualified with packages (pkg::sub::Name)
    bool isTypeName(std::string_view paName) {
      for (std::size_t start = 0;;) {
        const std::size_t end = paName.find("::", start);
        if (!isIdentifier(paName.substr(start, end - start))) {
          return false;
        }
        if (end == std::string_view::npos) {
          return true;
        }
        start = end + 2;
      }
    }

    EMGMResponse fail(std::string &paError, std::string paMessage, EMGMResponse paResponse = EMGMResponse::BadParams) {
      paError = std::move(paMessage);
      return paResponse;
    }

    //! Reads the count field paKey of the table at paTable
    EMGMResponse readCount(lua_State *paState,
                           int paTable,
                           const char *paKey,
                           std::size_t paMax,
                           std::size_t &paCount,
                           std::string &paError) {
      pushField(paState, paTable, paKey);
      intmax_t value;
      const bool valid = toInteger(paState, -1, 0, static_cast<intmax_t>(paMax), value);
      lua_pop(paState, 1);
      if (!valid) {
        return fail(paError, std::string(paKey) + " must be an integer from 0 to " + std::to_string(paMax));
      }
      paCount = static_cast<std::size_t>(value);
      return EMGMResponse::Ready;
    }

    //! Pushes the list field paKey of the table at paTable, checking that it is a table
    EMGMResponse pushList(lua_State *paState, int paTable, const char *paKey, std::string &paError) {
      if (pushField(paState, paTable, paKey) != LUA_TTABLE) {
        lua_pop(paState, 1);
        return fail(paError, std::string(paKey) + " must be a table");
      }
      return EMGMResponse::Ready;
    }

    //! Reads paCount names from the list field paKey
    EMGMResponse readNames(lua_State *paState,
                           int paTable,
                           const char *paKey,
                           std::size_t paCount,
                           std::vector<StringId> &paNames,
                           std::string &paError) {
      if (EMGMResponse response = pushList(paState, paTable, paKey, paError); response != EMGMResponse::Ready) {
        return response;
      }
      EMGMResponse response = EMGMResponse::Ready;
      if (lua_objlen(paState, -1) != paCount) {
        response = fail(paError, std::string(paKey) + " must have " + std::to_string(paCount) + " entries");
      }
      paNames.clear();
      paNames.reserve(paCount);
      for (std::size_t i = 1; response == EMGMResponse::Ready && i <= paCount; ++i) {
        lua_rawgeti(paState, -1, static_cast<int>(i));
        std::size_t length = 0;
        const char *name = (lua_type(paState, -1) == LUA_TSTRING) ? lua_tolstring(paState, -1, &length) : nullptr;
        if (name == nullptr || !isIdentifier(std::string_view(name, length))) {
          response = fail(paError, std::string(paKey) + " entry " + std::to_string(i) + " is not a valid name");
        } else {
          paNames.push_back(StringId::insert(std::string_view(name, length)));
        }
        lua_pop(paState, 1);
      }
      lua_pop(paState, 1);
      return response;
    }

    //! Reads all entries of the integer list field paKey
    EMGMResponse readIntegers(lua_State *paState,
                              int paTable,
                              const char *paKey,
                              intmax_t paMin,
                              intmax_t paMax,
                              std::vector<intmax_t> &paValues,
                              std::string &paError) {
      if (EMGMResponse response = pushList(paState, paTable, paKey, paError); response != EMGMResponse::Ready) {
        return response;
      }
      EMGMResponse response = EMGMResponse::Ready;
      const std::size_t length = lua_objlen(paState, -1);
      paValues.clear();
      paValues.reserve(length);
      for (std::size_t i = 1; response == EMGMResponse::Ready && i <= length; ++i) {
        lua_rawgeti(paState, -1, static_cast<int>(i));
        intmax_t value;
        if (!toInteger(paState, -1, paMin, paMax, value)) {
          response = fail(paError, std::string(paKey) + " entry " + std::to_string(i) + " is out of range");
        } else {
          paValues.push_back(value);
        }
        lua_pop(paState, 1);
      }
      lua_pop(paState, 1);
      return response;
    }

    std::string_view trim(std::string_view paText) {
      const std::size_t start = paText.find_first_not_of(' ');
      if (start == std::string_view::npos) {
        return {};
      }
      return paText.substr(start, paText.find_last_not_of(' ') - start + 1);
    }

    bool parseBound(std::string_view paText, intmax_t &paValue) {
      paText = trim(paText);
      const auto result = std::from_chars(paText.data(), paText.data() + paText.size(), paValue);
      return result.ec == std::errc() && result.ptr == paText.data() + paText.size();
    }

    //! Parses "ARRAY [lo..hi] OF T", returns the element type name or an empty view
    std::string_view parseArrayDeclaration(std::string_view paText, intmax_t &paLowerBound, intmax_t &paUpperBound) {
      constexpr std::string_view keyword = "ARRAY";
      if (!paText.starts_with(keyword)) {
        return {};
      }
      paText = trim(paText.substr(keyword.size()));
      const std::size_t close = paText.find(']');
      const std::size_t range = paText.find("..");
      if (!paText.starts_with('[') || close == std::string_view::npos || range == std::string_view::npos ||
          range > close) {
        return {};
      }
      if (!parseBound(paText.substr(1, range - 1), paLowerBound) ||
          !parseBound(paText.substr(range + 2, close - range - 2), paUpperBound)) {
        return {};
      }
      paText = trim(paText.substr(close + 1));
      if (!paText.starts_with("OF ")) {
        return {};
      }
      return trim(paText.substr(3));
    }

    //! Resolves an element type name to a compiled data type
    EMGMResponse resolveDataType(std::string_view paName, StringId &paTypeName, std::string &paError) {
      if (!isTypeName(paName)) {
        return fail(paError, "invalid data type name '" + std::string(paName) + "'");
      }
      paTypeName = StringId::lookup(paName);
      if (!paTypeName || getDataTypeEntry(paTypeName) == nullptr) {
        return fail(paError, "unknown data type '" + std::string(paName) + "'", EMGMResponse::UnsupportedType);
      }
      return EMGMResponse::Ready;
    }

    //! Reads the data type list at the top of the stack, arrays are either "ARRAY [lo..hi] OF T" or "ARRAY", n, "T"
    EMGMResponse
    readDataTypes(lua_State *paState, const char *paKey, std::vector<SLuaDataType> &paTypes, std::string &paError) {
      const int length = static_cast<int>(lua_objlen(paState, -1));
      paTypes.clear();
      for (int i = 1; i <= length; ++i) {
        lua_rawgeti(paState, -1, i);
        std::size_t textLength = 0;
        const char *text = (lua_type(paState, -1) == LUA_TSTRING) ? lua_tolstring(paState, -1, &textLength) : nullptr;
        const std::string name = (text != nullptr) ? std::string(text, textLength) : std::string();
        lua_pop(paState, 1);
        if (text == nullptr) {
          return fail(paError, std::string(paKey) + " entry " + std::to_string(i) + " is not a string");
        }

        SLuaDataType type;
        std::string_view elementName = name;
        if (name == "ARRAY") {
          // legacy form: "ARRAY", <number of elements>, "<element type>"
          intmax_t size = 0;
          lua_rawgeti(paState, -1, i + 1);
          const bool validSize = toInteger(paState, -1, 1, CLuaInterface::scmMaxDataPoints, size);
          lua_pop(paState, 1);
          lua_rawgeti(paState, -1, i + 2);
          text = (lua_type(paState, -1) == LUA_TSTRING) ? lua_tolstring(paState, -1, &textLength) : nullptr;
          const std::string legacyElement = (text != nullptr) ? std::string(text, textLength) : std::string();
          lua_pop(paState, 1);
          if (!validSize || text == nullptr) {
            return fail(paError, std::string(paKey) + " entry " + std::to_string(i) + " is an incomplete ARRAY");
          }
          type.mIsArray = true;
          type.mUpperBound = size - 1;
          if (EMGMResponse response = resolveDataType(legacyElement, type.mTypeName, paError);
              response != EMGMResponse::Ready) {
            return response;
          }
          i += 2;
        } else {
          if (name.starts_with("ARRAY ") || name.starts_with("ARRAY[")) {
            elementName = parseArrayDeclaration(name, type.mLowerBound, type.mUpperBound);
            if (elementName.empty() || type.mLowerBound > type.mUpperBound ||
                type.mUpperBound - type.mLowerBound >= static_cast<intmax_t>(CLuaInterface::scmMaxDataPoints)) {
              return fail(paError, "invalid array declaration '" + name + "'");
            }
            type.mIsArray = true;
          }
          if (EMGMResponse response = resolveDataType(elementName, type.mTypeName, paError);
              response != EMGMResponse::Ready) {
            return response;
          }
        }
        paTypes.push_back(type);
      }
      return EMGMResponse::Ready;
    }

    bool hasDuplicates(std::vector<StringId> paNames) {
      std::sort(paNames.begin(), paNames.end());
      return std::adjacent_find(paNames.begin(), paNames.end()) != paNames.end();
    }

    void append(std::vector<StringId> &paTarget, const std::vector<StringId> &paSource) {
      paTarget.insert(paTarget.end(), paSource.begin(), paSource.end());
    }

    bool adapterTypeExists(StringId paTypeName) {
      return getAdapterTypeEntry(paTypeName) != nullptr || getRuntimeAdapterTypeEntry(paTypeName) != nullptr;
    }
  } // namespace

  std::unique_ptr<CIEC_ANY> SLuaDataType::createValue() const {
    if (mIsArray) {
      return std::make_unique<CIEC_ARRAY_DYNAMIC>(mLowerBound, mUpperBound, mTypeName);
    }
    return std::unique_ptr<CIEC_ANY>(createDataTypeInstance(mTypeName, nullptr));
  }

  std::vector<std::unique_ptr<CIEC_ANY>> CLuaInterface::createValues(std::span<const SLuaDataType> paTypes) {
    std::vector<std::unique_ptr<CIEC_ANY>> values;
    values.reserve(paTypes.size());
    for (const SLuaDataType &type : paTypes) {
      values.push_back(type.createValue());
    }
    return values;
  }

  EMGMResponse CLuaInterface::parse(lua_State *paState, int paIndex, bool paWithAdapters, std::string &paError) {
    const int table = toAbsIndex(paState, paIndex);
    if (lua_type(paState, table) != LUA_TTABLE) {
      return fail(paError, "interfaceSpec must be a table");
    }

    EMGMResponse response = parseEvents(paState, table, "EI", mEINames, mEIWith, paError);
    if (response == EMGMResponse::Ready) {
      response = parseEvents(paState, table, "EO", mEONames, mEOWith, paError);
    }
    if (response == EMGMResponse::Ready) {
      response = parseData(paState, table, "numDIs", "DINames", "DIDataTypeNames", scmMaxDataPoints, mDINames, mDITypes,
                           paError);
    }
    if (response == EMGMResponse::Ready) {
      response = parseData(paState, table, "numDOs", "DONames", "DODataTypeNames", scmMaxDataPoints, mDONames, mDOTypes,
                           paError);
    }
    if (response == EMGMResponse::Ready) {
      response = parseWith(paState, table, "EI", mDINames.size(), mEIWith, paError);
    }
    if (response == EMGMResponse::Ready) {
      response = parseWith(paState, table, "EO", mDONames.size(), mEOWith, paError);
    }
    if (response == EMGMResponse::Ready && paWithAdapters) {
      response = parseAdapters(paState, table, paError);
    }
    if (response != EMGMResponse::Ready) {
      return response;
    }

    std::vector<StringId> variables = mDINames;
    append(variables, mDONames);
    append(variables, mPlugNames);
    append(variables, mSocketNames);
    if (hasDuplicates(mEINames) || hasDuplicates(mEONames) || hasDuplicates(variables)) {
      return fail(paError, "interface names must be unique");
    }

    buildSpecs();
    return EMGMResponse::Ready;
  }

  EMGMResponse CLuaInterface::parseInternals(lua_State *paState, int paIndex, std::string &paError) {
    const int table = toAbsIndex(paState, paIndex);
    if (lua_type(paState, table) != LUA_TTABLE) {
      return fail(paError, "internalVarsInformation must be a table");
    }
    EMGMResponse response = parseData(paState, table, "numIntVars", "intVarsNames", "intVarsDataTypeNames",
                                      scmMaxDataPoints, mInternalNames, mInternalTypes, paError);
    if (response != EMGMResponse::Ready) {
      return response;
    }

    std::vector<StringId> variables = mDINames;
    append(variables, mDONames);
    append(variables, mPlugNames);
    append(variables, mSocketNames);
    append(variables, mInternalNames);
    if (hasDuplicates(variables)) {
      return fail(paError, "internal variable names must be unique");
    }
    return EMGMResponse::Ready;
  }

  EMGMResponse CLuaInterface::parseEvents(lua_State *paState,
                                          int paIndex,
                                          const char *paPrefix,
                                          std::vector<StringId> &paNames,
                                          std::vector<std::vector<TPortId>> &paWith,
                                          std::string &paError) {
    const std::string prefix(paPrefix);
    std::size_t count = 0;
    EMGMResponse response = readCount(paState, paIndex, ("num" + prefix + "s").c_str(), scmMaxEvents, count, paError);
    if (response == EMGMResponse::Ready) {
      response = readNames(paState, paIndex, (prefix + "Names").c_str(), count, paNames, paError);
    }
    paWith.assign(paNames.size(), {});
    return response;
  }

  EMGMResponse CLuaInterface::parseWith(lua_State *paState,
                                        int paIndex,
                                        const char *paPrefix,
                                        std::size_t paNumDataPoints,
                                        std::vector<std::vector<TPortId>> &paWith,
                                        std::string &paError) {
    const std::string withKey = std::string(paPrefix) + "With";
    const std::string indexesKey = withKey + "Indexes";
    std::vector<intmax_t> with;
    std::vector<intmax_t> indexes;
    EMGMResponse response =
        readIntegers(paState, paIndex, withKey.c_str(), 0, static_cast<intmax_t>(scmWithDelimiter), with, paError);
    if (response == EMGMResponse::Ready) {
      response = readIntegers(paState, paIndex, indexesKey.c_str(), -1, static_cast<intmax_t>(with.size()) - 1, indexes,
                              paError);
    }
    if (response != EMGMResponse::Ready) {
      return response;
    }
    if (indexes.size() != paWith.size()) {
      return fail(paError, indexesKey + " must have " + std::to_string(paWith.size()) + " entries");
    }

    for (std::size_t event = 0; event < paWith.size(); ++event) {
      if (indexes[event] < 0) {
        continue;
      }
      std::size_t pos = static_cast<std::size_t>(indexes[event]);
      for (; pos < with.size() && with[pos] != static_cast<intmax_t>(scmWithDelimiter); ++pos) {
        if (static_cast<std::size_t>(with[pos]) >= paNumDataPoints) {
          return fail(paError, withKey + " refers to a missing data point");
        }
        paWith[event].push_back(static_cast<TPortId>(with[pos]));
      }
      if (pos == with.size()) {
        return fail(paError, withKey + " misses a delimiter");
      }
    }
    return EMGMResponse::Ready;
  }

  EMGMResponse CLuaInterface::parseData(lua_State *paState,
                                        int paIndex,
                                        const char *paNumKey,
                                        const char *paNamesKey,
                                        const char *paTypesKey,
                                        std::size_t paMaxNum,
                                        std::vector<StringId> &paNames,
                                        std::vector<SLuaDataType> &paTypes,
                                        std::string &paError) {
    std::size_t count = 0;
    EMGMResponse response = readCount(paState, paIndex, paNumKey, paMaxNum, count, paError);
    if (response == EMGMResponse::Ready) {
      response = readNames(paState, paIndex, paNamesKey, count, paNames, paError);
    }
    if (response == EMGMResponse::Ready) {
      response = pushList(paState, paIndex, paTypesKey, paError);
    }
    if (response != EMGMResponse::Ready) {
      return response;
    }
    response = readDataTypes(paState, paTypesKey, paTypes, paError);
    lua_pop(paState, 1);
    if (response == EMGMResponse::Ready && paTypes.size() != count) {
      response = fail(paError, std::string(paTypesKey) + " must have " + std::to_string(count) + " types");
    }
    return response;
  }

  EMGMResponse CLuaInterface::parseAdapters(lua_State *paState, int paIndex, std::string &paError) {
    std::size_t count = 0;
    EMGMResponse response = readCount(paState, paIndex, "numAdapters", scmMaxAdapters, count, paError);
    if (response == EMGMResponse::Ready) {
      response = pushList(paState, paIndex, "adapterInstanceDefinition", paError);
    }
    if (response != EMGMResponse::Ready) {
      return response;
    }
    if (lua_objlen(paState, -1) != count) {
      lua_pop(paState, 1);
      return fail(paError, "adapterInstanceDefinition must have " + std::to_string(count) + " entries");
    }

    std::vector<SLuaAdapterDefinition> plugs;
    std::vector<SLuaAdapterDefinition> sockets;
    const int list = lua_gettop(paState);
    for (std::size_t i = 1; response == EMGMResponse::Ready && i <= count; ++i) {
      lua_rawgeti(paState, list, static_cast<int>(i));
      const int entry = lua_gettop(paState);
      if (lua_type(paState, entry) != LUA_TTABLE) {
        response = fail(paError, "adapterInstanceDefinition entry " + std::to_string(i) + " must be a table");
      } else {
        const bool hasName = pushField(paState, entry, "adapterNameID") == LUA_TSTRING;
        const bool hasType = pushField(paState, entry, "adapterTypeNameID") == LUA_TSTRING;
        const bool hasKind = pushField(paState, entry, "isPlug") == LUA_TBOOLEAN;
        std::size_t nameLength = 0;
        std::size_t typeLength = 0;
        const char *name = hasName ? lua_tolstring(paState, -3, &nameLength) : nullptr;
        const char *type = hasType ? lua_tolstring(paState, -2, &typeLength) : nullptr;
        if (!hasKind || name == nullptr || !isIdentifier(std::string_view(name, nameLength)) || type == nullptr ||
            !isTypeName(std::string_view(type, typeLength))) {
          response = fail(paError, "adapterInstanceDefinition entry " + std::to_string(i) + " is invalid");
        } else {
          const StringId typeName = StringId::lookup(std::string_view(type, typeLength));
          if (!typeName || !adapterTypeExists(typeName)) {
            response = fail(paError, "unknown adapter type '" + std::string(type, typeLength) + "'",
                            EMGMResponse::UnsupportedType);
          } else {
            const SLuaAdapterDefinition definition{.mName = StringId::insert(std::string_view(name, nameLength)),
                                                   .mTypeName = typeName,
                                                   .mIsPlug = lua_toboolean(paState, -1) != 0};
            (definition.mIsPlug ? plugs : sockets).push_back(definition);
          }
        }
        lua_pop(paState, 3);
      }
      lua_pop(paState, 1);
    }
    lua_pop(paState, 1);
    if (response != EMGMResponse::Ready) {
      return response;
    }

    mAdapters = std::move(plugs);
    mAdapters.insert(mAdapters.end(), sockets.begin(), sockets.end());
    for (const SLuaAdapterDefinition &adapter : mAdapters) {
      (adapter.mIsPlug ? mPlugNames : mSocketNames).push_back(adapter.mName);
    }
    return EMGMResponse::Ready;
  }

  void CLuaInterface::buildSpecs() {
    mSpec = {
        .mEINames = mEINames,
        .mEITypeNames = {},
        .mEONames = mEONames,
        .mEOTypeNames = {},
        .mDINames = mDINames,
        .mDONames = mDONames,
        .mDIONames = {},
        .mSocketNames = mSocketNames,
        .mPlugNames = mPlugNames,
    };
    mMirroredSpec = {
        .mEINames = mEONames,
        .mEITypeNames = {},
        .mEONames = mEINames,
        .mEOTypeNames = {},
        .mDINames = mDONames,
        .mDONames = mDINames,
        .mDIONames = {},
        .mSocketNames = {},
        .mPlugNames = {},
    };
  }

} // namespace forte::lua
