/*
 Copyright (C) 2020 Kristian Duske

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

#pragma once


#include "kd/reflection_decl.h"

#include "vm/mat.h"

#include <optional>
#include <string>

namespace tb::mdl
{

class Group
{
private:
  std::string m_name;

  vm::mat4x4d m_transformation;

  std::optional<std::string> m_prefabPath;
  std::optional<std::string> m_prefabHash;

  kdl_reflect_decl(Group, m_name, m_transformation, m_prefabPath, m_prefabHash);

public:
  explicit Group(std::string name);

  const std::string& name() const;
  void setName(std::string name);

  const vm::mat4x4d& transformation() const;
  void setTransformation(const vm::mat4x4d& transformation);
  void transform(const vm::mat4x4d& transformation);

  /**
   * The prefab file this group was placed from, as the drop payload names it without its
   * "prefab:" prefix: the library root ("user" or "mod"), a slash and the path relative
   * to that root, with forward slashes, for example "mod/props/crate.map". Serialised as
   * _tb_prefab.
   */
  const std::optional<std::string>& prefabPath() const;
  void setPrefabPath(std::optional<std::string> prefabPath);

  /**
   * The content hash (lower-case hex of a 64-bit FNV-1a, see fnv1a64Hex) of the prefab
   * file at the time this group was placed or refreshed. Serialised as _tb_prefab_hash.
   */
  const std::optional<std::string>& prefabHash() const;
  void setPrefabHash(std::optional<std::string> prefabHash);
};

} // namespace tb::mdl
