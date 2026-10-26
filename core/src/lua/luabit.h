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

#pragma once

struct lua_State;

namespace forte::lua {

  //! The global name of the bit library
  inline constexpr char cBitLibraryName[] = "bit";

  /*!\brief Opens a bit library with the API of LuaJIT's and Lua BitOp's as the global table bit
   *
   * The functions tobit, tohex, bnot, band, bor, bxor, lshift, rshift, arshift, rol, ror and bswap work on 32 bit
   * integers: arguments are taken modulo 2^32 (non-integral numbers are rounded) and results are signed. Shift
   * counts use their low 5 bits.
   */
  int openBitLibrary(lua_State *paState);

} // namespace forte::lua
