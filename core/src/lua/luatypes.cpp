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

#include "luatypes.h"

extern "C" {
#include <lua.h>
}

#include "forte/util/devlog.h"
#include "luaadapter.h"
#include "luabfb.h"
#include "luacfb.h"
#include "luaengine.h"

namespace forte::lua {
  namespace {
    enum class EFBKind { Basic, Composite, Unknown };

    //! Loads the module to see which kind of FB type it defines
    EMGMResponse getFBKind(StringId paTypeNameId, const std::string &paSource, EFBKind &paKind, std::string &paError) {
      CLuaEngine engine;
      if (!engine.isValid()) {
        paError = "could not set up a Lua state";
        return EMGMResponse::Overflow;
      }
      if (!engine.loadModule(paSource, paTypeNameId.data(), paError)) {
        return EMGMResponse::InvalidObject;
      }
      lua_State *const state = engine.getState();
      lua_pushliteral(state, "ECC");
      lua_rawget(state, -2);
      const bool hasECC = !lua_isnil(state, -1);
      lua_pushliteral(state, "fbnSpec");
      lua_rawget(state, -3);
      const bool hasNetwork = !lua_isnil(state, -1);
      lua_pop(state, 3);
      paKind = hasECC ? EFBKind::Basic : (hasNetwork ? EFBKind::Composite : EFBKind::Unknown);
      return EMGMResponse::Ready;
    }

    template<typename TEntry, typename TRegister>
    EMGMResponse
    createAndRegister(StringId paTypeNameId, std::string paSource, TRegister paRegister, std::string &paError) {
      std::shared_ptr<TEntry> entry;
      EMGMResponse response = TEntry::create(paTypeNameId, std::move(paSource), entry, paError);
      if (response == EMGMResponse::Ready) {
        response = paRegister(std::move(entry));
        if (response == EMGMResponse::InvalidOperation) {
          paError = "a compiled type has this name";
        } else if (response == EMGMResponse::InvalidState) {
          paError = "the type exists with another definition and has instances";
        }
      }
      return response;
    }

    EMGMResponse report(StringId paTypeNameId, EMGMResponse paResponse, const std::string &paError) {
      if (paResponse == EMGMResponse::Ready) {
        DEVLOG_INFO("[Lua] Created type %s\n", paTypeNameId.data());
      } else {
        DEVLOG_ERROR("[Lua] Could not create type %s: %s\n", paTypeNameId.data(), paError.c_str());
      }
      return paResponse;
    }
  } // namespace

  EMGMResponse createFBType(StringId paTypeNameId, std::string paSource) {
    std::string error;
    if (!isValidTypeName(paTypeNameId.data())) {
      return report(paTypeNameId, EMGMResponse::BadParams, "invalid type name");
    }
    EFBKind kind = EFBKind::Unknown;
    EMGMResponse response = getFBKind(paTypeNameId, paSource, kind, error);
    if (response != EMGMResponse::Ready) {
      return report(paTypeNameId, response, error);
    }
    const auto registerFB = [](std::shared_ptr<const CRuntimeFBTypeEntry> paEntry) {
      return registerRuntimeFBType(std::move(paEntry));
    };
    switch (kind) {
      case EFBKind::Basic:
        response = createAndRegister<CLuaBFBTypeEntry>(paTypeNameId, std::move(paSource), registerFB, error);
        break;
      case EFBKind::Composite:
        response = createAndRegister<CLuaCFBTypeEntry>(paTypeNameId, std::move(paSource), registerFB, error);
        break;
      case EFBKind::Unknown:
        response = EMGMResponse::BadParams;
        error = "the module has neither an ECC function nor an fbnSpec table";
        break;
    }
    return report(paTypeNameId, response, error);
  }

  EMGMResponse createAdapterType(StringId paTypeNameId, std::string paSource) {
    std::string error;
    if (!isValidTypeName(paTypeNameId.data())) {
      return report(paTypeNameId, EMGMResponse::BadParams, "invalid type name");
    }
    const auto registerAdapter = [](std::shared_ptr<const CRuntimeAdapterTypeEntry> paEntry) {
      return registerRuntimeAdapterType(std::move(paEntry));
    };
    return report(paTypeNameId,
                  createAndRegister<CLuaAdapterTypeEntry>(paTypeNameId, std::move(paSource), registerAdapter, error),
                  error);
  }

} // namespace forte::lua
