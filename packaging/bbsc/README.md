# BBSC for the package

`out/bbsc` holds BBSC 0.0.2 (github.com/CLSAMG22/BBSC, GPL-2.0) as `package.sh` bundles it. Its `bbsc.dll` is built
from BBSC's source at commit 4f11faf with `supermedo-bbsc.patch` applied: BBSC's bells are given in the Hunter's
Dream, not at the very start; while dead and watching a hunter your unseen body goes with them (their area
stays loaded); and a guest that dies while the host's boss bar is up counts as dying in the boss fight
(it spectates instead of waking at its lamp). The rest of `out/bbsc` is BBSC-0.0.2-BBport.zip unchanged.

To build the DLL: clone CLSAMG22/BBSC at 4f11faf with the submodules `emulator/externals/fmt` and
`emulator/externals/xbyak`, `git apply supermedo-bbsc.patch`, then run `emulator/mod/build-mod.cmd`
(Visual Studio 2022 Build Tools and clang-cl; set its OUT folder). Copy `bbsc.dll` and the patch into `out/bbsc`.
Built that way without the patch, all of the DLL's texts match the released one (only the build date and folder
differ).
