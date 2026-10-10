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

#include "mdl/IdTech4MaterialName.h"

#include "kd/path_utils.h"

#include <algorithm>
#include <string>

namespace tb::mdl
{
namespace
{

bool isAscii(const char c)
{
  return static_cast<unsigned char>(c) < 0x80;
}

} // namespace

std::filesystem::path idTech4Path(const std::string_view text)
{
  // ASCII is the same in every native path encoding, so the native string is built
  // directly, which skips the platform's narrow-to-native conversion and its cost
  if (text.empty())
  {
    return {};
  }

  using Char = std::filesystem::path::value_type;
  auto native = std::filesystem::path::string_type{};
  native.reserve(text.size());
  for (const auto c : text)
  {
    native.push_back(static_cast<Char>(isAscii(c) ? c : '_'));
  }
  return kdl::parse_path(std::move(native));
}

std::string canonicalIdTech4DeclName(const std::string_view name)
{
  // see idDeclManagerLocal::MakeNameCanonical in RBDOOM-3-BFG's
  // neo/framework/DeclManager.cpp: only ASCII letters are lowercased (idStr::ToLower)
  auto result = std::string{};
  result.reserve(name.size());

  auto lastDot = std::string::npos;
  for (const auto c : name)
  {
    if (c == '\\')
    {
      result.push_back('/');
    }
    else if (c == '.')
    {
      lastDot = result.size();
      result.push_back(c);
    }
    else if (c >= 'A' && c <= 'Z')
    {
      result.push_back(static_cast<char>(c - 'A' + 'a'));
    }
    else
    {
      result.push_back(isAscii(c) ? c : '_');
    }
  }

  if (lastDot != std::string::npos)
  {
    result.resize(lastDot);
  }

  return result;
}

std::filesystem::path canonicalIdTech4MaterialName(const std::string_view name)
{
  return idTech4Path(canonicalIdTech4DeclName(name));
}

} // namespace tb::mdl
