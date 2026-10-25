# TbLitLibTest

The lit preview's tests (plan `plans/lit-preview`). Run the executable from its build folder,
`build-lit/lib/TbLitLib/test/<Debug|Release>/TbLitLibTest.exe`.

**A short build path is needed (LP1c fix S3.16).** Many tests make a game folder below the
working folder with `fs::TestEnvironment`: a 32-character folder, a non-ASCII folder, the test
case's name and then the game's own paths. From a working folder deeper than about 75
characters these paths pass Windows' `MAX_PATH` (260): folders cannot be made, and game files
read as missing, as the engine's `fopen` reads them too. Keep the build folder short (the main
checkout's `build-lit`), or run from an 8.3 path.

Hidden tests: `"[.][retail]"` reads the game folder `TB_LIT_GAME_PATH` names (read only);
`"[.][litgpu]"` (LP0-LP2's cases), `"[.][litgpu2]"` (LP4's and LPF's) and `"[.][litgpu3]"`
(LP5's, and LP7's until it passes 150 s) need a Vulkan device and `TB_LIT_GAME_PATH`, each run
on its own and under 180 s (LP5.0's split, LP7.0's third tag; a fourth once one passes 150 s); `"[.][feed]"` the host API's cases (LPF.2); `"[.][parity]"` the
parity rows (`TB_LIT_PARITY_ROOT`, `TB_LIT_PARITY_OUT`); `"[.][retail-dump]"` writes the
engine-capture dump.
