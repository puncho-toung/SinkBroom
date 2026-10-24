/*
 Copyright (C) 2026 TrenchBroom contributors

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

// ---- engine notice ----
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/ImageManager.cpp and
// idlib/Str.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2024 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/ImageOverride.h"

#include <fmt/format.h>

#include <algorithm>
#include <string_view>

namespace tb::lit
{
namespace
{

// ImageManager.cpp:420 (and the shim's ShimRenderer.cpp ImageFormats)
constexpr std::string_view ImageFormats[] = {".tga", ".jpg", ".png", ".exr", ".hdr"};

// Str.cpp:937-981 idStr::Replace( old, "" ): every occurrence, left to right, without
// overlap, compared with case (idStr::Cmpn)
std::string removeAll(const std::string& text, const std::string_view old)
{
  auto out = std::string{};
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();)
  {
    if (text.compare(i, old.size(), old) == 0)
    {
      i += old.size();
    }
    else
    {
      out.push_back(text[i]);
      ++i;
    }
  }
  return out;
}

// idStr::Icmp's case folding: ASCII letters only
std::string lowercased(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(), [](const char c) {
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
  });
  return text;
}

} // namespace

std::string feedKeyName(const std::string& name)
{
  // ImageManager.cpp:417-423: strip any .tga file extensions from anywhere in the _name,
  // including image program parameters
  auto folded = name;
  for (const auto extension : ImageFormats)
  {
    folded = removeAll(folded, extension);
  }
  std::replace(folded.begin(), folded.end(), '\\', '/');
  // :432: the records' names are compared without case
  return lowercased(std::move(folded));
}

std::string feedMatchName(const MaterialImageKey& key)
{
  return lowercased(key.name);
}

std::string feedNameRefusal(const std::string& name)
{
  if (name.empty() || name == "default")
  {
    return fmt::format(
      "an override of \"{}\" is refused: the name is the engine's default image "
      "(ImageManager.cpp:403-407)",
      name);
  }
  if (name.front() == '_')
  {
    return fmt::format(
      "an override of \"{}\" is refused: a name that starts with '_' is one of the "
      "engine's built-in images, which match any options and are never read from files "
      "(ImageManager.cpp:436-439)",
      name);
  }
  return {};
}

} // namespace tb::lit
