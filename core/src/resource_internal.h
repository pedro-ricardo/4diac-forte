/*******************************************************************************
 * Copyright (c) 2025 Martin Erich Jobst
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0
 *
 * Contributors:
 *    Martin Erich Jobst - initial implementation
 *******************************************************************************/

#pragma once

#include "monitoring.h"

#ifdef FORTE_DYNAMIC_TYPE_LOAD
#include <memory>

#include "lua/luaengine.h"
#endif

#ifdef FORTE_TRACE_CTF
#include "trace/tracerConfig.h"
#endif

namespace forte {
  class ResourceInternal {
    public:
      explicit ResourceInternal(CResource &paResource) :
          mMonitoringHandler(paResource)
#ifdef FORTE_TRACE_CTF
          ,
          mTracer(paResource.getInstanceNameId(), FORTE_TRACE_CTF_BUFFER_SIZE)
#endif
      {
      }

      CMonitoringHandler &getMonitoringHandler() {
        return mMonitoringHandler;
      }

#ifdef FORTE_DYNAMIC_TYPE_LOAD
      /*!\brief The Lua state of this resource's Lua FB instances, created on first use
       *
       * Instances keep a reference, so the state outlives them however the resource is torn down.
       * Only called from the management thread, which creates the instances.
       */
      const std::shared_ptr<lua::CLuaEngine> &getLuaEngine() {
        if (!mLuaEngine) {
          mLuaEngine = std::make_shared<lua::CLuaEngine>();
        }
        return mLuaEngine;
      }

      /*!\brief The Lua state that reads the modules of new Lua types, created on first use
       *
       * Types are created on the device from the management thread, so only the device's is used. It is kept
       * between types instead of setting up a state for each, and set up again if that failed, e.g. for lack
       * of memory.
       */
      lua::CLuaEngine &getLuaDefinitionEngine() {
        if (!mLuaDefinitionEngine || !mLuaDefinitionEngine->isValid()) {
          mLuaDefinitionEngine.reset(); // a failed state goes before a new one needs memory
          mLuaDefinitionEngine = std::make_unique<lua::CLuaEngine>(lua::CLuaEngine::scmDefinitionInstructionLimit);
        }
        return *mLuaDefinitionEngine;
      }
#endif

#ifdef FORTE_TRACE_CTF
      trace::CForteTracer &getTracer() {
        return mTracer;
      }
#endif

    private:
      CMonitoringHandler mMonitoringHandler;

#ifdef FORTE_TRACE_CTF
      trace::CForteTracer mTracer;
#endif // FORTE_TRACE_CTF

#ifdef FORTE_DYNAMIC_TYPE_LOAD
      std::shared_ptr<lua::CLuaEngine> mLuaEngine;
      std::unique_ptr<lua::CLuaEngine> mLuaDefinitionEngine;
#endif
  };
} // namespace forte
