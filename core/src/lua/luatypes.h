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

#include <string>

#include "forte/mgmcmd.h"
#include "forte/stringid.h"

namespace forte::lua {

  /*!\brief Creates an FB type from a Lua module and registers it
   *
   * A module with an ECC function defines a basic FB type, a module with an fbnSpec table a composite FB type.
   *
   * @return Ready, also when the type exists with the same definition;
   *         InvalidObject if the module does not load; BadParams for a malformed module or type name;
   *         UnsupportedType for an unknown data or adapter type; InvalidOperation for the name of a compiled type;
   *         InvalidState if the type exists with another definition and has instances;
   *         Overflow if no Lua state could be created
   */
  EMGMResponse createFBType(StringId paTypeNameId, std::string paSource);

  //! Creates an adapter type from a Lua module and registers it, with the results of createFBType
  EMGMResponse createAdapterType(StringId paTypeNameId, std::string paSource);

} // namespace forte::lua
