# Copied engine code (TbLitIdLib)

These files are copied verbatim from the D3-Sink engine (RBDOOM-3-BFG fork) at commit
`285ea4d18a0aaedf46b0803ea656e4850eba5a01`. The paths below this folder are the engine's
paths below `neo/`. SinkBroom's lit preview uses them to parse Doom 3 materials, tables,
skins and entityDefs with the engine's own code (plan `00-PLAN.md` §3.5, D12, C7), and to
build its geometry as the engine does (windings, patches, the vertex format and the
tangent code of `renderer/tr_trisurf.cpp`; D57, C24), and to load images as the engine
loads them (`renderer/Image_files.cpp`, the mip and cube functions of
`renderer/Image_process.cpp`, `renderer/Color/ColorSpace.*`; LP1c.2). LP2.0 adds two pure
functions its pipelines and uploads need (C24, P2.25): `PipelineCache::GetRenderState` with
`GetStencilOpState` (`renderer/PipelineCache.*`) and `GetRowPitch` with what it calls
(`renderer/Image_load.cpp`), compiled inside `../shim/src/ShimPipelineState.cpp`. The LP2
fix round (`31-LP2-FIXES.md` DX.2) adds the static interactions' cull to the light volume:
`R_CalcInteractionCullBits`' planes and cull bits and `R_CreateInteractionLightTris`'
triangle cull (`renderer/Interaction.cpp`, three statement ranges) with
`LIGHT_CLIP_EPSILON` and `srfCullInfo_t` (`renderer/Interaction.h`), compiled inside a
function of `../shim/src/EngineInteraction.cpp` that declares, with the engine's names,
what the ranges read and write.

`renderer/DXT/` (`DXTCodec.h`, `DXTCodec_local.h`, `DXTEncoder_SSE2.cpp`) is **test only**
(LP1c fix S3.2): the engine's fast DXT encoders, compiled into `TbLitIdDxtTestLib`, which
`TbLitLibTest` alone links, so that the image goldens compare the source fallback with the
engine's caches block for block. No product target links it: the preview never encodes BC
on the CPU (D24). `mirror.json` marks these entries `testOnly`.

`libs/` holds the libraries the engine itself vendors and the copied loaders call:
`stb/stb_image.h`, `tinyexr/tinyexr.h` and `mesa/format_r11g11b10f.h`, whole and at the
engine's versions, so that a source image decodes to the same bytes (D27's recorded
exception, R5.6). Their terms are in `LICENSE_EXCEPTIONS.md`, the engine's file, copied
whole next to `LICENSE.md`.

**Never edit these files.** A change the copied code needs goes into the shim
(`../shim/`). The shim headers `precompiled.h` and `RenderCommon.h` stand in for the
engine's headers of those names, so every whole-file copy here is byte-identical to the
engine's file and compares equal by its git blob id.

Two kinds of file:

- **Whole files** are the engine's file, byte for byte.
- **Partial files** hold only engine line ranges. Each range is preceded by one marker line
  `// ==== engine neo/<file> lines A-B (anchor) ====`; the markers are the only lines that
  are not the engine's.

## Third-party terms beyond `LICENSE_EXCEPTIONS.md` (LP1c fix S1.2)

`LICENSE_EXCEPTIONS.md` is the engine's own file, copied whole. Two terms the copied code
carries are not in it, and are recorded here:

- **cmft** (cube map utilities): the cube helpers of `renderer/Image_process.cpp` (engine
  lines 854-1167, "ripped from cmft utils by Dario Manesku") are under the BSD-2-Clause
  licence:

  > Copyright 2014-2015 Dario Manesku. All rights reserved.
  >
  > Redistribution and use in source and binary forms, with or without modification, are
  > permitted provided that the following conditions are met:
  >
  > 1. Redistributions of source code must retain the above copyright notice, this list of
  >    conditions and the following disclaimer.
  > 2. Redistributions in binary form must reproduce the above copyright notice, this list
  >    of conditions and the following disclaimer in the documentation and/or other
  >    materials provided with the distribution.
  >
  > THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
  > EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  > MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
  > THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  > SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
  > OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  > INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
  > STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
  > THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

- **OpenEXR**, inside TinyEXR (`libs/tinyexr/tinyexr.h`), under the BSD-3-Clause licence
  that `LICENSE_EXCEPTIONS.md` and `tinyexr.h` carry; miniz v1.15 inside it is public
  domain (the Unlicense).

TrenchBroom credits its other BSD and MIT libraries by name in the About dialog, and so do
these lines (`lib/TbUiLib/src/AboutDialog.cpp`). A packaged build that ships the lit preview
must also ship `LICENSE_EXCEPTIONS.md` and this note with the documentation, as the BSD
terms ask of binary redistributions (for packaging, D7).

`../../mirror.json` lists every file and range with its engine lines, anchor and sha256,
together with the compile definitions, the C++ standard, the shim's mirrored functions,
the virtuals the shim defines, the never-returning call sites and the statics. It is
checked against the engine before each landing (`TOOLS/lit_mirror_check.py`, LP-M).

This folder is excluded from clang-format (`.clang-format`) and from the clang-tidy
workflow. The code is GPL-3 with the Doom 3 BFG Edition additional terms; see `LICENSE.md`
and the notice at the top of each file.
