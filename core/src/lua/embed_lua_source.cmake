# *******************************************************************************
# Copyright (c) 2026 Aimirim STI
# This program and the accompanying materials are made available under the
# terms of the Eclipse Public License 2.0 which is available at
# http://www.eclipse.org/legal/epl-2.0.
#
# SPDX-License-Identifier: EPL-2.0
#
# Contributors:
#   Pedro Ricardo C. Souza - initial implementation
# *******************************************************************************/
#
# Writes a Lua source file as a null terminated byte array into a C++ header.
#   cmake -DINPUT=<file.lua> -DOUTPUT=<header.h> -DVARIABLE=<name> -P embed_lua_source.cmake

file(READ ${INPUT} content HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," content "${content}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n    "
       content "${content}")
get_filename_component(name ${INPUT} NAME)
file(WRITE ${OUTPUT}
     "// Generated from ${name} by embed_lua_source.cmake, do not edit\n"
     "#pragma once\n\n"
     "namespace forte::lua {\n"
     "  constexpr unsigned char ${VARIABLE}[] = {\n    ${content}0x00};\n"
     "} // namespace forte::lua\n"
)
