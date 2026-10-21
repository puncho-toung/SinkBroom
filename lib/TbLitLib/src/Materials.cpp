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

#include "lit/Materials.h"

#include <fmt/format.h>

namespace tb::lit
{
namespace
{

template <typename T>
Result<T> toResult(DeclResult<T> result, const std::string_view what)
{
  if (result.value)
  {
    return std::move(*result.value);
  }
  return Error{fmt::format("{}: {}", what, result.error)};
}

} // namespace

Result<std::unique_ptr<MaterialDecls>> makeMaterialDecls(const DeclSource& source)
{
  auto decls = std::make_unique<MaterialDecls>(source);
  if (!decls->constructionError().empty())
  {
    return Error{
      fmt::format("Cannot load the material decls: {}", decls->constructionError())};
  }
  return decls;
}

Result<std::shared_ptr<const MaterialEntry>> lookupMaterial(
  MaterialDecls& decls, const std::string_view name)
{
  return toResult(decls.lookup(name), fmt::format("Cannot parse material '{}'", name));
}

Result<EvaluatedRegisters> evaluateMaterial(
  MaterialDecls& decls,
  const MaterialEntry& material,
  const std::array<float, MaterialDecls::ShaderParmCount>& shaderParms,
  const float time)
{
  return toResult(
    decls.evaluate(material, shaderParms, time),
    fmt::format("Cannot evaluate material '{}'", material.name));
}

Result<std::vector<std::pair<std::string, std::string>>> lookupEntityDefKeys(
  MaterialDecls& decls, const std::string_view name)
{
  return toResult(
    decls.entityDefKeys(name), fmt::format("Cannot parse entityDef '{}'", name));
}

} // namespace tb::lit
