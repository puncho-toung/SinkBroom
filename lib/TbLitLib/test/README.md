# TbLitLibTest

The lit preview's tests (plan `plans/lit-preview`). Run the executable from its build folder,
`build-lit/lib/TbLitLib/test/<Debug|Release>/TbLitLibTest.exe`.

**A short build path is needed (LP1c fix S3.16).** Many tests make a game folder below the
working folder with `fs::TestEnvironment`: a 32-character folder, a non-ASCII folder, the test
case's name and then the game's own paths. From a working folder deeper than about 75
characters these paths pass Windows' `MAX_PATH` (260): folders cannot be made, and game files
read as missing, as the engine's `fopen` reads them too. Keep the build folder short (the main
checkout's `build-lit`), or run from an 8.3 path.

Hidden tests (F1.21, 2026-11-05), each run on its own, serially, never under ctest and never
while a build or another suite runs, with `--durations yes`; the bound is the wall clock a run must
stay under:

- `"[.][litgpu]"` (LP0-LP2's GPU cases), `"[.][litgpu2]"` (LP4's, LPF's and LP3's shadow cases;
  their atlas and picture PNGs go to the folder `TB_LIT_PNG_OUT` names, if set), `"[.][litgpu3]"`
  (LP5's, LP7's and LP3's) and `"[.][litgpu4]"` (LP6's, the four LP6 cases of `[litgpu]` among them
  since the final fix round, DQ-4, and that round's own cases) and `"[.][litgpu5]"` (the eviction
  case and V-F.2's case, split from `[litgpu4]` for its clock in the re-check's fix round): a Vulkan
  device and the game folder `TB_LIT_GAME_PATH` names (read only); 150 s each.
- `"[.][parity]"`: the parity rows over the preview root of a `parity.py` run
  (`TB_LIT_PARITY_ROOT`), their read-backs and meta written to `TB_LIT_PARITY_OUT`; 180 s.
  `"[.][parity-moc]"` (F3.3): the same rows with the masked occlusion culling on, run after
  `[parity]` with the same two variables: it reads `[parity]`'s read-backs in `TB_LIT_PARITY_OUT`
  and writes its own under `<TB_LIT_PARITY_OUT>/moc`; 180 s. Both read the root's `parity/rows.json`,
  or (F3.15) the file `TB_LIT_PARITY_ROWS` names (a `refresh-meta --out` into a scratch folder).
- `"[.][retail]"`: the user's game folder `TB_LIT_GAME_PATH` (read only); 600 s. `"[.][retail-dump]"` writes the engine capture's dump
  to `TB_LIT_DUMP_PATH` (`TOOLS/lit-preview/capture_goldens.py` runs it).
- `"[.][feed]"`: the Qt-free host API's cases (LPF.2), a Vulkan device and `TB_LIT_GAME_PATH`;
  180 s.
- `"[.][stagesort]"` and `"[.][numbers]"`: timings (the stage sort over a few thousand chunks,
  lexerFloat on a million texts), CPU only, in no gate.

TbUiLibTest's hidden lit tests: `"[.][litgpu]"` (the panel on a GPU, in a map window too: a
Vulkan device, the desktop and `TB_LIT_GAME_PATH`; 150 s) and `"[.][measure]"` (the measurement on
the retail map admin, Release: `TB_LIT_GAME_PATH`, its outputs in `TB_LIT_MEASURE_OUT`, a temp
folder when unset; 300 s).
