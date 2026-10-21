# Copied engine code (TbLitIdLib)

These files are copied verbatim from the D3-Sink engine (RBDOOM-3-BFG fork) at commit
`285ea4d18a0aaedf46b0803ea656e4850eba5a01`. The paths below this folder are the engine's
paths below `neo/`. SinkBroom's lit preview uses them to parse Doom 3 materials, tables,
skins and entityDefs with the engine's own code (plan `00-PLAN.md` §3.5, D12, C7), and to
build its geometry as the engine does (windings, patches, the vertex format and the
tangent code of `renderer/tr_trisurf.cpp`; D57, C24).

**Never edit these files.** A change the copied code needs goes into the shim
(`../shim/`). The shim headers `precompiled.h` and `RenderCommon.h` stand in for the
engine's headers of those names, so every whole-file copy here is byte-identical to the
engine's file and compares equal by its git blob id.

Two kinds of file:

- **Whole files** are the engine's file, byte for byte.
- **Partial files** hold only engine line ranges. Each range is preceded by one marker line
  `// ==== engine neo/<file> lines A-B (anchor) ====`; the markers are the only lines that
  are not the engine's.

`../../mirror.json` lists every file and range with its engine lines, anchor and sha256,
together with the compile definitions, the C++ standard, the shim's mirrored functions,
the virtuals the shim defines, the never-returning call sites and the statics. It is
checked against the engine before each landing (`TOOLS/lit_mirror_check.py`, LP-M).

This folder is excluded from clang-format (`.clang-format`) and from the clang-tidy
workflow. The code is GPL-3 with the Doom 3 BFG Edition additional terms; see `LICENSE.md`
and the notice at the top of each file.
