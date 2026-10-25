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

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "forte/arch/forte_sync.h"

struct lua_State;

namespace forte {
  class CIEC_ANY;
}

namespace forte::lua {

  /*!\brief One LuaJIT state running the Lua modules of run-time types
   *
   * Every module runs in its own environment table holding a safe subset of the base library, copies of the
   * string, table, math and bit libraries, and a require that only knows STfunc. There is no io, os, debug, ffi,
   * package or jit access, and only source text is accepted (no precompiled byte code).
   *
   * Calls into Lua must hold getLock(), as instances of one resource are created from the management thread and
   * executed from the resource's event chain thread.
   *
   * Lua errors unwind through C++ frames. Functions called from Lua (lua_CFunctions) must therefore not hold
   * objects with destructors when they raise an error.
   */
  class CLuaEngine {
    public:
      //! The default VM instruction budget per call, 0 for none
      static const unsigned int scmDefaultInstructionLimit;

      explicit CLuaEngine(unsigned int paInstructionLimit = scmDefaultInstructionLimit);
      ~CLuaEngine();

      CLuaEngine(const CLuaEngine &) = delete;
      CLuaEngine &operator=(const CLuaEngine &) = delete;

      //! false if the state or the STfunc library could not be set up
      [[nodiscard]] bool isValid() const {
        return mValid;
      }

      [[nodiscard]] lua_State *getState() const {
        return mState;
      }

      arch::CSyncObject &getLock() {
        return mLock;
      }

      [[nodiscard]] unsigned int getInstructionLimit() const {
        return mInstructionLimit;
      }

      /*!\brief Runs a module chunk in a fresh sandbox
       *
       * On success the table returned by the chunk is pushed onto the stack.
       * @param paSource the Lua source text
       * @param paChunkName name used in error messages, e.g. the type name
       * @param paError receives the error message on failure
       */
      bool loadModule(std::string_view paSource, const char *paChunkName, std::string &paError);

      /*!\brief Calls the function below paNumArgs arguments on the stack in protected mode
       *
       * Applies the instruction budget. On failure nothing is left on the stack and paError holds the message
       * with a traceback.
       */
      bool call(int paNumArgs, int paNumResults, std::string &paError);

      //! Pops the top value and stores it in the registry, returns its reference
      int ref();
      void unref(int paRef);
      void pushRef(int paRef);

      //! Pushes a read only view of the STfunc library
      void pushSTfunc();

      /*!\brief Pushes an IEC value as a Lua value
       *
       * ARRAYs become tables keyed by their IEC index (lower to upper bound) with the fields lo and up,
       * STRUCTs become tables keyed by member name, CHAR and WCHAR one character strings, the time types
       * integers in nanoseconds.
       */
      static void pushValue(lua_State *paState, const CIEC_ANY &paValue);

      /*!\brief Stores the Lua value at paIndex into an IEC value
       *
       * Integers wrap to the width of the IEC type, as in a C cast. A Lua value of the wrong kind is rejected.
       * A missing (nil) array element or struct member leaves the IEC element unchanged.
       * @return nullptr on success, otherwise a static description of the error (nothing raised)
       */
      static const char *getValue(lua_State *paState, int paIndex, CIEC_ANY &paValue);

    private:
      bool setUp();
      void pushSandbox();
      void armBudget();
      void disarmBudget();

      static int panic(lua_State *paState);
      static int traceback(lua_State *paState);

      lua_State *mState;
      arch::CSyncObject mLock;
      unsigned int mInstructionLimit;
      int mSandboxFactoryRef;
      int mSTfuncRef;
      bool mValid;
  };

} // namespace forte::lua
