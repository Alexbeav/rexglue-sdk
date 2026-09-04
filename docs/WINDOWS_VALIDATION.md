# Windows SDK validation with LLVM and the MSVC ABI

Use the Windows helper from this SDK checkout:

```powershell
pwsh -NoProfile -File tools/build-windows-msvc.ps1
ctest --test-dir out/build/win-amd64-msvc -C Release --output-on-failure
```

The helper selects `clang.exe` and `clang++.exe` from
`%ProgramFiles%/LLVM/bin`, checks the Windows MSVC target, and builds
`ppc_tests`, `unit_tests`, and `rexgpu-xenos`. LLVM, the Visual Studio C++
build tools, the Windows SDK, CMake, Ninja, and this checkout's PPC binutils
must be installed. Python is required by the deferred branch's headless
command-handler test.

Override the LLVM directory with `-LlvmDirectory`. Use `-ConfigureOnly`
to configure without compiling, or `-Jobs 8` to limit parallel compilation.
The default configuration is Release; `-Configuration` also accepts Debug
and RelWithDebInfo.

The build cache is under `out/build/win-amd64-msvc`. Test executables and
SDK binaries are under its `bin/Release` directory. The helper does not
install or deploy them into game projects.

A previous cache may point to a MinGW/Retcomm compiler even when another
LLVM installation is on PATH. The helper refuses to change a cache's pinned
compiler. Choose a fresh `-BuildDirectory` instead; its binaries remain
under that directory's `bin` subdirectory. Existing build caches and
validated game binaries are preserved.

SDK migration checks recognize source includes with CRLF line endings and
manifest filenames after either directory separator. Source-include rewrites
preserve the existing line endings. Template-drift checks ignore differences
between LF and CRLF, so newline style alone does not trigger regeneration.
Template and migration test fixtures use separate temporary directories for
parallel runs.

A successful build proves compilation and linking. Run the tests above for
instruction and unit validation. Gameplay and image correctness still need
an explicitly requested, hash-bound game run.
