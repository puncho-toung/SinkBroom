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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderWorld_load.cpp and
// renderer/Model.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2025 Robert Beckebans
//   Copyright (C) 2014-2016 Kot in Action Creative Artel
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/BprocFile.h"

#include "lit/BprocModel.h"
#include "lit/EngineLight.h"

#include "kd/result.h"

#include <fmt/format.h>

namespace tb::lit
{
namespace
{

// the magic's low byte is its version (RenderWorld_load.cpp:808-816)
int versionOf(const std::uint32_t magic)
{
  return int(magic & 0xFFu);
}

// LoadBinaryModel's time test, with the .bproc's time as the source time
bool modelIsStale(const std::int64_t mapTimeStamp, const std::int64_t modelTime)
{
  return mapTimeStamp != -1 && mapTimeStamp != 0 && mapTimeStamp != modelTime;
}

Error bprocError(const std::string_view what)
{
  return Error{fmt::format("The .bproc cannot be used: {}", what)};
}

} // namespace

Result<BprocTree> readBprocTree(
  const std::span<const std::byte> data, const bool procExists)
{
  return readBprocFile(data) | kdl::and_then([&](BprocFile file) -> Result<BprocTree> {
           auto result = BprocTree{};
           result.version = versionOf(file.magic);
           // :829-832: an old cache beside a .proc is stale (not production mode)
           if (result.version != 3 && procExists)
           {
             return bprocError(
               fmt::format("version {} is used only without a .proc", result.version));
           }
           result.mapName = std::move(file.mapName);
           result.timeStamp = file.timeStamp;
           result.tree = std::move(file.areas);

           for (auto& model : file.models)
           {
             // ReadBinaryModel: LoadBinaryModel( file, mapTimeStamp, 0 )
             if (modelIsStale(result.timeStamp, model.timeStamp))
             {
               return bprocError(
                 fmt::format("model {}'s time differs from the file's", model.name));
             }
             auto procModel = ProcModel{};
             procModel.area = procModelArea(model.name);
             // C31: the stored bounds are FinishSurfaces' own (deforming surfaces grown)
             procModel.bounds = model.bounds;
             procModel.boundsKnown = true;
             // LP7.0 (D138, P7.12): finished surfaces, kept as readBinaryModel read them
             procModel.finished = true;
             for (auto& surface : model.surfaces)
             {
               auto procSurface = ProcSurface{};
               procSurface.material = std::move(surface.material);
               procSurface.numVerts = surface.numVerts;
               procSurface.numIndexes = int(surface.indexes.size());
               // the binary keeps no vertex format
               procSurface.vertexFormat = -1;
               // a surface without geometry stores no bounds
               procSurface.bounds =
                 surface.geometry ? surface.bounds : engineClearedBounds();
               procSurface.finishedVerts = std::move(surface.verts);
               procSurface.indexes.assign(surface.indexes.begin(), surface.indexes.end());
               procSurface.tangentsCalculated = surface.tangentsCalculated;
               procModel.surfaces.push_back(std::move(procSurface));
             }
             procModel.name = std::move(model.name);
             result.tree.models.push_back(std::move(procModel));
           }

           // the .proc reader's bounds checks (ParseInterAreaPortals, ParseNodes)
           const auto& tree = result.tree;
           for (const auto& portal : tree.portals)
           {
             if (
               portal.positiveArea < 0 || portal.positiveArea >= tree.numAreas
               || portal.negativeArea < 0 || portal.negativeArea >= tree.numAreas)
             {
               return bprocError("a portal names an area past the area count");
             }
           }
           for (const auto& node : tree.nodes)
           {
             for (const auto child : node.children)
             {
               if (child >= int(tree.nodes.size()))
               {
                 return bprocError(fmt::format("node child {} out of range", child));
               }
             }
           }
           return result;
         });
}

} // namespace tb::lit
