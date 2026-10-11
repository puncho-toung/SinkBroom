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

#include "mdl/ModelConvention.h"

#include <ostream>

namespace tb::mdl
{

std::ostream& operator<<(std::ostream& lhs, const ModelConvention rhs)
{
  switch (rhs)
  {
  case ModelConvention::Default:
    lhs << "Default";
    break;
  case ModelConvention::IdTech4:
    lhs << "IdTech4";
    break;
  }
  return lhs;
}

} // namespace tb::mdl
