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

#pragma once

#include "base/Result.h"
#include "lit/MaterialDecls.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tb::lit
{

// TbLitLib's side of the copied material parser: the facade's results as Result<T>
// (§3.12). Like the facade, every function here is single-threaded and belongs on the
// thread that owns the MaterialDecls (the parse thread, D32).

// the registry over source; fails when another registry exists (C17)
Result<std::unique_ptr<MaterialDecls>> makeMaterialDecls(const DeclSource& source);

// the material of this name, or the engine's implicit material for a name without a decl
Result<std::shared_ptr<const MaterialEntry>> lookupMaterial(
  MaterialDecls& decls, std::string_view name);

// the material's registers for these shaderParms at this time (seconds)
Result<EvaluatedRegisters> evaluateMaterial(
  MaterialDecls& decls,
  const MaterialEntry& material,
  const std::array<float, MaterialDecls::ShaderParmCount>& shaderParms,
  float time);

// the merged keys of an entityDef, inherited keys included
Result<std::vector<std::pair<std::string, std::string>>> lookupEntityDefKeys(
  MaterialDecls& decls, std::string_view name);

} // namespace tb::lit
