param(
    [string]$WorkspaceRoot = "",
    [string]$Output = ""
)

$ErrorActionPreference = 'Stop'
$SourceRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($WorkspaceRoot -eq "") {
    $WorkspaceRoot = $SourceRoot
}
$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
if ($Output -eq "") {
    $Output = Join-Path $SourceRoot 'output\rt_shim.zip'
}
$Output = [System.IO.Path]::GetFullPath($Output)
$CMake = (Get-Command cmake -CommandType Application -ErrorAction Stop).Source

$Ndk = Join-Path $WorkspaceRoot 'tools\downloads\android-ndk-r27c'
$NdkBuild = Join-Path $Ndk 'ndk-build.cmd'
$Make = Join-Path $Ndk 'prebuilt\windows-x86_64\bin\make.exe'
$Toolchain = Join-Path $Ndk 'build\cmake\android.toolchain.cmake'
$Strip = Join-Path $Ndk `
    'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe'
$NativeSource = Join-Path $SourceRoot 'native'
$LoaderRoot = Join-Path $SourceRoot 'loader'
$ModuleSource = Join-Path $SourceRoot 'module'
$BuildRoot = Join-Path $SourceRoot 'build'
$NativeBuild = Join-Path $BuildRoot 'readonly_native'
$Stage = Join-Path $BuildRoot 'readonly_module_stage'
$Obj = Join-Path $BuildRoot 'loader_obj'
$Libs = Join-Path $BuildRoot 'loader_libs'

$Required = @(
    $NdkBuild, $Make, $Toolchain, $Strip,
    (Join-Path $NativeSource 'CMakeLists.txt'),
    (Join-Path $NativeSource 'readonly_runtime.c'),
    (Join-Path $NativeSource 'readonly_memory.c'),
    (Join-Path $NativeSource 'readonly_module.c'),
    (Join-Path $NativeSource 'readonly_exports.c'),
    (Join-Path $NativeSource 'il2cpp_metadata.c'),
    (Join-Path $NativeSource 'aim_math.c'),
    (Join-Path $NativeSource 'aim_policy.c'),
    (Join-Path $NativeSource 'gyro_controller.c'),
    (Join-Path $NativeSource 'scene_snapshot.c'),
    (Join-Path $NativeSource 'android_input.c'),
    (Join-Path $NativeSource 'sensor_probe.c'),
    (Join-Path $SourceRoot 'bridge\src\rt\internal\Bridge.java'),
    (Join-Path $LoaderRoot 'jni\Android.mk'),
    (Join-Path $LoaderRoot 'jni\Application.mk'),
    (Join-Path $LoaderRoot 'jni\loader.cpp'),
    (Join-Path $LoaderRoot 'jni\input_companion.c'),
    (Join-Path $LoaderRoot 'jni\zygisk.hpp'),
    (Join-Path $ModuleSource 'module.prop'),
    (Join-Path $ModuleSource 'README.md'),
    (Join-Path $ModuleSource 'customize.sh'),
    (Join-Path $ModuleSource 'META-INF\com\google\android\update-binary'),
    (Join-Path $ModuleSource 'META-INF\com\google\android\updater-script')
)
foreach ($Path in $Required) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Required file not found: $Path"
    }
}

New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null

Write-Host '[1/4] Building Android input bridge and ARM64 runtime payload'
& python (Join-Path $SourceRoot 'tools\build_bridge.py')
if ($LASTEXITCODE -ne 0) { throw 'Android input bridge build failed' }
& $CMake -S $NativeSource -B $NativeBuild `
    -G 'Unix Makefiles' `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    "-DCMAKE_MAKE_PROGRAM=$Make" `
    -DANDROID_ABI=arm64-v8a `
    -DANDROID_PLATFORM=android-23 `
    -DANDROID_STL=c++_static `
    -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'Native CMake configure failed' }
& $CMake --build $NativeBuild --target cf_readonly --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Native payload build failed' }
$Payload = Join-Path $NativeBuild 'libgcloudsync.so'

Write-Host '[2/4] Building Zygisk API v5 loader'
& $NdkBuild `
    ("NDK_PROJECT_PATH=" + $LoaderRoot) `
    ("APP_BUILD_SCRIPT=" + (Join-Path $LoaderRoot 'jni\Android.mk')) `
    ("NDK_APPLICATION_MK=" + (Join-Path $LoaderRoot 'jni\Application.mk')) `
    ("NDK_OUT=" + $Obj) `
    ("NDK_LIBS_OUT=" + $Libs)
if ($LASTEXITCODE -ne 0) { throw 'Zygisk loader build failed' }
$Loader = Join-Path $Libs 'arm64-v8a\librt_shim.so'

Write-Host '[3/4] Staging module'
$BuildPrefix = $BuildRoot.TrimEnd('\') + '\'
if (-not $Stage.StartsWith(
        $BuildPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe stage path: $Stage"
}
if (Test-Path -LiteralPath $Stage) {
    Remove-Item -LiteralPath $Stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path `
    (Join-Path $Stage 'zygisk'), `
    (Join-Path $Stage 'payload'), `
    (Join-Path $Stage 'META-INF\com\google\android') | Out-Null

Copy-Item -LiteralPath (Join-Path $ModuleSource 'module.prop') -Destination $Stage
Copy-Item -LiteralPath (Join-Path $ModuleSource 'customize.sh') -Destination $Stage
Copy-Item -LiteralPath (Join-Path $ModuleSource 'README.md') -Destination $Stage
Copy-Item -LiteralPath (Join-Path $ModuleSource 'sepolicy.rule') -Destination $Stage
Copy-Item -LiteralPath $Loader -Destination (Join-Path $Stage 'zygisk\arm64-v8a.so')
Copy-Item -LiteralPath $Payload -Destination (Join-Path $Stage 'payload\libgcloudsync.so')
Copy-Item -LiteralPath `
    (Join-Path $ModuleSource 'META-INF\com\google\android\update-binary') `
    -Destination (Join-Path $Stage 'META-INF\com\google\android\update-binary')
Copy-Item -LiteralPath `
    (Join-Path $ModuleSource 'META-INF\com\google\android\updater-script') `
    -Destination (Join-Path $Stage 'META-INF\com\google\android\updater-script')

& $Strip --strip-unneeded (Join-Path $Stage 'zygisk\arm64-v8a.so')
if ($LASTEXITCODE -ne 0) { throw 'Zygisk loader strip failed' }
& $Strip --strip-unneeded (Join-Path $Stage 'payload\libgcloudsync.so')
if ($LASTEXITCODE -ne 0) { throw 'Payload strip failed' }

Write-Host '[4/4] Creating Magisk module ZIP'
$OutputDir = Split-Path -Parent $Output
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
if (Test-Path -LiteralPath $Output) {
    Remove-Item -LiteralPath $Output -Force
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$Zip = [System.IO.Compression.ZipFile]::Open(
    $Output, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $StagePrefix = $Stage.TrimEnd('\') + '\'
    Get-ChildItem -LiteralPath $Stage -Recurse -File | ForEach-Object {
        if (-not $_.FullName.StartsWith(
                $StagePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "File escaped staging directory: $($_.FullName)"
        }
        $EntryName = $_.FullName.Substring($StagePrefix.Length).Replace('\', '/')
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $Zip, $_.FullName, $EntryName,
            [System.IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    $Zip.Dispose()
}

Get-FileHash -LiteralPath $Output -Algorithm SHA256
