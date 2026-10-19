# Standalone packaging — v1.1.0

The release ZIP is a complete Windows x64 installation. Extract it in full and run `DESRU.exe`. It does not require the original utility or a separately installed .NET runtime. The source checkout and GitHub's automatic source archives are not runnable distributions.

## Build inputs

- This repository, tag `v1.1.0`; original DESRU base `f78f602e80c4cd1ceb7c6ac306cb4531f111f3ab` (1.6.17).
- Windows Visual Studio MSBuild with .NET desktop SDK and C++ desktop workload, CMake, Ninja, Windows SDK. The firewall COM reference requires full Visual Studio MSBuild. Vulkan headers are vendored.
- NuGet application dependencies pinned in the project; .NET and Windows Desktop runtime packs `6.0.36`, `win-x64`.
- Original [DESRU 1.6.17 release](https://github.com/bowsr/DESRU/releases/tag/1.6.17), `DESRU.zip`, SHA-256 `31556caae6e88d6b9bbd9d7079112f2b81a61c6bbE5E5cdd8e58e7b48344b080` (case-insensitive). Use only its four required fonts, `macro/DOOMEternalMacro.exe`, `macro/LICENSE` and `meath00k/XINPUT1_3.dll`. Do not copy an existing user's installation, logs or settings into the package.

## Publish

Run from the repository root in a Visual Studio developer shell. Use a new output folder. The legacy build events replace the `output/` directory, so keep `SkipBundleBuildEvents=true` throughout.

```powershell
./NativeVisuals/build.ps1
MSBuild.exe DESpeedrunUtil/DESpeedrunUtil.csproj /t:Restore,Publish /p:Configuration=Release /p:Platform=x64 /p:RuntimeIdentifier=win-x64 /p:SelfContained=true /p:RuntimeFrameworkVersion=6.0.36 /p:PublishSingleFile=false /p:DebugType=None /p:DebugSymbols=false /p:SkipBundleBuildEvents=true /p:SkipNativeVisualBuild=true /p:Version=1.1.0 /p:PublishDir="$PWD/artifacts/standalone-publish/"
MSBuild.exe Updater/Updater.csproj /t:Restore,Publish /p:Configuration=Release /p:Platform=x64 /p:RuntimeIdentifier=win-x64 /p:SelfContained=true /p:RuntimeFrameworkVersion=6.0.36 /p:DebugType=None /p:DebugSymbols=false /p:SkipBundleBuildEvents=true /p:Version=1.1.0 /p:PublishDir="$PWD/artifacts/updater-publish/"
```

Assemble a clean staging directory from the main publish output and the updater publish output. Their shared runtime files must match. Explicitly copy `artifacts/visuals-build/bin/DESRU.Visuals.dll` to `visuals/DESRU.Visuals.Runtime.dll`: the managed project's after-build copy is not a guarantee that this DLL reaches the publish directory. Preserve `visuals/licenses/`.

Add the original resource files listed above, all four READMEs, `docs/`, `CREDITS.md`, the GPL license and third-party license directory. Include the .NET runtime packs' license and third-party notices. The original components retain their own ownership/terms; they are not relicensed by the fork. The updater remains included for distribution completeness, but upstream automatic updating is disabled in `DESRU.dll.config` by default.

Exclude PDBs, logs, user profiles/config overrides, development notes, memory dumps, game executables, extracted geometry, test executables and build caches. Do not distribute a user's configured `output/` folder. The application retains upstream base-version reporting; the release and assembly version identify fork version 1.1.0.

## Verify and archive

Build/run `tests/DESRU.Tests` in Release x64 with the same skipped bundle events. Run `ctest --test-dir artifacts/visuals-build --output-on-failure`. Invoke the test executable with `--verify-package <absolute-staging-directory>` to exercise the real startup file check and configuration parser against the assembled files. Compare embedded approved Help resources in the packaged assembly as well.

Check the native DLL is x64 with the bridge entry points, and that both runtimeconfig files contain self-contained `includedFrameworks`, not an external framework requirement. Validate every ZIP entry and run the package file check again after extraction. Publish `DESRU-Enhanced-v1.1.0-standalone-win-x64.zip` with `SHA256SUMS.txt` and the exact source tag. See [release validation](RELEASE-VALIDATION.md) for the checks actually performed.
