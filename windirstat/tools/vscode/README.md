# VS Code Workflow

Intent: VS Code is the day-to-day development environment for this repo. The build still uses the supported Windows MSVC toolchain, Windows SDK, MSBuild, and Rust tooling where the project already requires them.

## Requirements

- VS Code with the `llvm-vs-code-extensions.vscode-clangd` extension.
- Visual Studio Build Tools or Visual Studio with the MSVC x64 tools and Windows SDK installed.
- Rust/Cargo for `windirstat-scan-host` because the scan host links the Rust scan engine.

The Microsoft C/C++ IntelliSense engine is disabled for this workspace. clangd owns C/C++ navigation, completion, diagnostics, and go-to-definition.

## Open

Open the `windirstat` folder in VS Code, or open `windirstat.code-workspace` from the repository parent.

## IntelliSense

Run the VS Code task `clangd: refresh compile_commands` after cloning, after changing `.vcxproj` files, or after changing project include/define settings.

That task writes a local `compile_commands.json` from the MSBuild projects and the discovered MSVC developer environment. The file is intentionally machine-local and ignored by git.

clangd reads that file as the source of truth for per-file flags, including MSVC-style options, Windows SDK include paths, MSVC STL include paths, project include paths, defines, and the forced `pch.h` include used by most translation units.

To verify clangd is using the right flags:

- Open `RemoteStub/RemoteScanHost.cpp`, `RemoteStub/ScanHostMain.cpp`, or `DirStatDoc.cpp`.
- Use `clangd: Restart language server` from the command palette after refreshing `compile_commands.json`.
- Check clangd's output channel for `compile_commands.json` and the matching source file path.

## Build

Use `Terminal > Run Task...`:

- `Build Debug x64`
- `Build Scan Host Debug x64`
- `build main app debug x64 (compile only)`
- `build scan host debug x64 (compile only)`

The normal build tasks run the project post-build events and sign the binaries. The compile-only tasks skip post-build signing/copy steps and are useful only for validating compilation.

If you ran a compile-only task and Windows blocks the EXE as unsigned, run:

- `sign debug x64 outputs`

## Debug

Press F5, or use `Run and Debug`:

- `F5: WinDirStat Debug x64`
- `Debug scan host x64`

The default F5 configuration runs `Build Debug x64`, then launches `build\WinDirStat_x64.exe` with the matching PDB. That path comes from `windirstat.vcxproj`: `OutDir` is `build\`, `TargetName` is `WinDirStat_$(PlatformShortName.ToLower())`, and Debug x64 resolves to `WinDirStat_x64.exe`.

To debug a different target, change the launch config's `program` and `preLaunchTask` together. The scan-host config already uses `Build Scan Host Debug x64` and `build\windirstat-scan-host_x64.exe`.

## Refreshing Project Metadata

When project files, generated headers, include paths, or defines change, rerun `clangd: refresh compile_commands`. If headers still look stale, restart clangd or reload the VS Code window so it reopens the new compilation database.
