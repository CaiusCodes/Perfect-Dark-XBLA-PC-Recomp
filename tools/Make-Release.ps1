# Builds the Perfect Dark portable release in out\release. The ZIP holds one folder:
#
#   Perfect Dark\
#     Perfect Dark.exe            the launcher: the only program to open
#     README.txt
#     licenses\
#     resources\
#       runtime\                  the game (Perfect Dark.exe, rexruntime.dll,
#                                 rexgpu-xenos.dll, VC++ runtime DLLs)
#
# The first time the launcher runs it imports the game data from the player's
# own Xbox 360 package into resources\assets; saves and settings go to
# resources\userdata. The release carries no original game content; the audit
# below refuses to package anything that looks like game data, debug output,
# scripts or user state, and checks the folder root holds only the four items
# above.
[CmdletBinding()]
param(
    [string]$BuildPreset = "win-amd64-release",
    # Defaults to the VERSION file; a different value is refused.
    [string]$Version,
    # Folder holding the Visual C++ runtime DLLs to ship; found automatically.
    [string]$VcRedistDir
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$versionFile = Join-Path (Join-Path $PSScriptRoot "..") "VERSION"
$builtVersion = ([System.IO.File]::ReadAllText($versionFile)).Trim()
if (-not $Version) { $Version = $builtVersion }
elseif ($Version -ne $builtVersion) {
    throw "Version $Version differs from the game's own ($builtVersion, VERSION): change it there and rebuild."
}

if ($Version -notmatch '^[A-Za-z0-9._-]+$') {
    throw "Version may contain only letters, numbers, dots, underscores and hyphens."
}

$releaseName = "Perfect Dark"
$exeName = "Perfect Dark.exe"          # the launcher in the root, and the game's name inside resources\runtime
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path -Path $PSScriptRoot -ChildPath ".."))
$buildRoot = Join-Path $projectRoot "out\build\$BuildPreset"
$packagingRoot = Join-Path $projectRoot "packaging"
$releaseRoot = Join-Path $projectRoot "out\release"
# Staged under out\release\staging so a copy someone plays from out\release is never touched.
$stageRoot = Join-Path $releaseRoot "staging\$releaseName"
$zipPath = Join-Path $releaseRoot "Perfect-Dark-XBLA-PC-Recomp-v$Version.zip"
$launcherBuilt = Join-Path $projectRoot "out\launcher\$exeName"

$runtimeFiles = @("perfect_dark.exe", "rexruntime.dll", "rexgpu-xenos.dll")
$vcRuntimeFiles = @("msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll")

# --- 1. Inputs ---------------------------------------------------------------
foreach ($file in $runtimeFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $buildRoot $file) -PathType Leaf)) {
        throw "Build output is missing: $file. Build $BuildPreset first."
    }
}

# A release must never ship a binary older than the source it claims to be.
$exeTime = (Get-Item -LiteralPath (Join-Path $buildRoot "perfect_dark.exe")).LastWriteTimeUtc
$newestSource = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot "src") -File -Recurse) + @(Get-Item -LiteralPath $versionFile) |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if ($newestSource.LastWriteTimeUtc -gt $exeTime) {
    throw "perfect_dark.exe is older than src\$($newestSource.Name). Rebuild before packaging."
}

if (-not $VcRedistDir) {
    $candidates = @(Get-Item -Path "C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT" -ErrorAction SilentlyContinue |
        Where-Object { $dir = $_.FullName; @($vcRuntimeFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $dir $_)) }).Count -eq 0 } |
        Sort-Object { [version]($_.FullName -replace '^.*\\MSVC\\([0-9.]+)\\.*$', '$1') } -Descending)
    if ($candidates.Count -eq 0) {
        throw "Visual C++ redistributable DLLs not found. Pass -VcRedistDir."
    }
    $VcRedistDir = $candidates[0].FullName
}
foreach ($file in $vcRuntimeFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $VcRedistDir $file) -PathType Leaf)) {
        throw "Missing $file in $VcRedistDir"
    }
}

# ReXGlue 0.9.0 uses GPU plugin ABI 2. Validate the exported ABI function
# before packaging so a stale plugin cannot produce a broken release.
if (-not ("GpuPluginInspector" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class GpuPluginInspector {
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate UInt32 GpuAbiVersionDelegate();

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryExW(
        string fileName, IntPtr file, UInt32 flags);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string symbolName);

    [DllImport("kernel32.dll")]
    private static extern bool FreeLibrary(IntPtr module);

    public static UInt32 GetAbiVersion(string pluginPath) {
        const UInt32 LoadLibrarySearchDllLoadDir = 0x00000100;
        const UInt32 LoadLibrarySearchDefaultDirs = 0x00001000;
        IntPtr module = LoadLibraryExW(
            pluginPath, IntPtr.Zero,
            LoadLibrarySearchDllLoadDir | LoadLibrarySearchDefaultDirs);
        if (module == IntPtr.Zero) {
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Could not load GPU plugin for ABI validation");
        }

        try {
            IntPtr symbol = GetProcAddress(module, "rex_gpu_abi_version");
            if (symbol == IntPtr.Zero) {
                throw new InvalidOperationException(
                    "GPU plugin does not export rex_gpu_abi_version");
            }

            var function = (GpuAbiVersionDelegate)Marshal.GetDelegateForFunctionPointer(
                symbol, typeof(GpuAbiVersionDelegate));
            return function();
        }
        finally {
            FreeLibrary(module);
        }
    }
}
"@
}

$gpuPluginAbi = [GpuPluginInspector]::GetAbiVersion((Join-Path $buildRoot "rexgpu-xenos.dll"))
if ($gpuPluginAbi -ne 2) {
    throw "GPU plugin ABI mismatch: found $gpuPluginAbi, expected 2. Rebuild with GPU_PLUGINS xenos."
}

# --- 2. Launcher ----------------------------------------------------------------
& (Join-Path $packagingRoot "Build-Launcher.ps1") -Version $Version -Output $launcherBuilt
if (-not (Test-Path -LiteralPath $launcherBuilt -PathType Leaf)) {
    throw "The launcher was not built: $exeName"
}

# --- 3. Stage -----------------------------------------------------------------
foreach ($played in @("resources\assets", "resources\userdata")) {
    if (Test-Path -LiteralPath (Join-Path $stageRoot $played)) {
        throw "The staging folder has been played from ($played; saves?). Move it away first: $stageRoot"
    }
}
foreach ($old in @($stageRoot, $zipPath)) {
    if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Recurse -Force }
}

$licensesStage = Join-Path $stageRoot "licenses"
$runtimeStage = Join-Path $stageRoot "resources\runtime"
foreach ($dir in @($stageRoot, $licensesStage, $runtimeStage)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

# Release root: the launcher, README and licences.
Copy-Item -LiteralPath $launcherBuilt -Destination $stageRoot
$readme = [System.IO.File]::ReadAllText((Join-Path $packagingRoot "README.txt")).Replace("@VERSION@", $Version)
$readme = $readme -replace "`r?`n", "`r`n"  # Notepad-friendly line endings
[System.IO.File]::WriteAllText((Join-Path $stageRoot "README.txt"), $readme, (New-Object System.Text.UTF8Encoding($false)))
Get-ChildItem -LiteralPath (Join-Path $packagingRoot "licenses") -File |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $licensesStage }

# resources\runtime: the game and its DLLs. The game reads its data from
# resources\assets and resources\userdata, beside this folder.
Copy-Item -LiteralPath (Join-Path $buildRoot "perfect_dark.exe") -Destination (Join-Path $runtimeStage $exeName)
foreach ($file in @($runtimeFiles | Where-Object { $_ -ne "perfect_dark.exe" })) {
    Copy-Item -LiteralPath (Join-Path $buildRoot $file) -Destination $runtimeStage
}
foreach ($file in $vcRuntimeFiles) { Copy-Item -LiteralPath (Join-Path $VcRedistDir $file) -Destination $runtimeStage }
[System.IO.File]::WriteAllText((Join-Path $runtimeStage "version.txt"),
    "Perfect Dark XBLA PC Recomp v$Version`r`n", (New-Object System.Text.UTF8Encoding($false)))

# --- 4. Audit: nothing from the game package, no debug output, no user state -
$staged = @(Get-ChildItem -LiteralPath $stageRoot -Recurse -Force)
$rootNames = @(Get-ChildItem -LiteralPath $stageRoot -Force | ForEach-Object { $_.Name } | Sort-Object)
$expectedRoot = @($exeName, "README.txt", "licenses", "resources" | Sort-Object)
if (($rootNames -join "|") -ne ($expectedRoot -join "|")) {
    throw "The release folder must hold only $($expectedRoot -join ', '); found: $($rootNames -join ', ')"
}
foreach ($item in @(Get-ChildItem -LiteralPath (Join-Path $stageRoot "resources") -Force)) {
    if ($item.Name -ne "runtime") { throw "Unexpected item in resources: $($item.Name)" }
}
$forbiddenNames = @("assets", "assets_import_tmp", "userdata", "logs", "cache", "Game", "perfect_dark.toml",
    "release-manifest.json", "default.xex", "ArcadeInfo.xml", "PackedSegFile", "guest_image.bin",
    "xstr_decompressed.bin", "memdump.bin")
$forbiddenPatterns = @("*.pdb", "*.xex", "*.bin", "*.log", "*.ilk", "*.exp", "*.lib", "*.ps1", "*.bat", "*.cmd",
    "Setup*", "*.xwb", "*.xsb", "*.str")
foreach ($item in $staged) {
    if ($forbiddenNames -contains $item.Name) {
        throw "Forbidden item entered the release: $($item.FullName)"
    }
    foreach ($pattern in $forbiddenPatterns) {
        if ($item.Name -like $pattern) { throw "Forbidden file type entered the release: $($item.FullName)" }
    }
    if (-not $item.PSIsContainer -and $item.Length -gt 64MB) {
        throw "Unexpectedly large file in the release: $($item.FullName)"
    }
}
# The launcher is a plain program: it carries no payload, so it stays small.
if ((Get-Item -LiteralPath (Join-Path $stageRoot $exeName)).Length -gt 4MB) {
    throw "The launcher is unexpectedly large; it must not carry a payload."
}

# Byte-level check against the developer's extracted game data, when present:
# no shipped file may be identical to any file from the package.
$stagedFiles = @($staged | Where-Object { -not $_.PSIsContainer })
$assetsRoot = Join-Path $projectRoot "game"
if (-not (Test-Path -LiteralPath (Join-Path $assetsRoot "default.xex"))) { $assetsRoot = Join-Path $buildRoot "assets" }
if (-not (Test-Path -LiteralPath (Join-Path $assetsRoot "default.xex"))) {
    throw "The extracted game is needed for the byte check (game\default.xex): see README.md."
}
$assetComparisons = 0
if (Test-Path -LiteralPath $assetsRoot) {
    $stagedSizes = @{}
    foreach ($file in $stagedFiles) { $stagedSizes[$file.Length] = $true }
    $stagedHashes = @{}
    foreach ($file in $stagedFiles) {
        $stagedHashes[(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash] = $file.FullName
    }
    foreach ($asset in @(Get-ChildItem -LiteralPath $assetsRoot -File -Recurse)) {
        if (-not $stagedSizes.ContainsKey($asset.Length)) { continue }
        $assetComparisons++
        $hash = (Get-FileHash -LiteralPath $asset.FullName -Algorithm SHA256).Hash
        if ($stagedHashes.ContainsKey($hash)) {
            throw "Release file $($stagedHashes[$hash]) is identical to game data $($asset.FullName)"
        }
    }
}

# No personal data in any shipped file: the builder's Windows user name, home
# folder or project folder. Compilers record source paths; the build maps them
# to neutral names with -ffile-prefix-map, and this checks that it worked.
$personal = @(
    $env:USERNAME,
    $env:USERPROFILE, ($env:USERPROFILE -replace '\\', '/'),
    $projectRoot, ($projectRoot -replace '\\', '/'),
    "OneDrive"
) | Where-Object { $_ -and $_.Length -ge 4 } | Select-Object -Unique
$latin1 = [System.Text.Encoding]::GetEncoding(28591)
foreach ($file in $stagedFiles) {
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    $ascii = $latin1.GetString($bytes)
    $wide = [System.Text.Encoding]::Unicode.GetString($bytes)
    foreach ($text in $personal) {
        if ($ascii.IndexOf($text, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or
            $wide.IndexOf($text, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw "Personal data ('$text') found in $($file.FullName). Rebuild so -ffile-prefix-map applies."
        }
    }
}

# --- 5. The release ZIP ---------------------------------------------------------
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stageRoot, $zipPath,
    [System.IO.Compression.CompressionLevel]::Optimal, $true)

$releaseSize = ($stagedFiles | Measure-Object -Property Length -Sum).Sum
$zip = Get-Item -LiteralPath $zipPath
Write-Host "Portable release created."
Write-Host "Folder:   $stageRoot"
Write-Host "ZIP:      $zipPath"
Write-Host "Root:     $(@(Get-ChildItem -LiteralPath $stageRoot | ForEach-Object Name) -join ', ')"
Write-Host "Runtime:  $(@(Get-ChildItem -LiteralPath $runtimeStage -File -Recurse).Count) files in resources\runtime"
Write-Host "GPU ABI:  $gpuPluginAbi"
Write-Host "VC++ CRT: $VcRedistDir"
Write-Host "Audit:    root holds only the 4 expected items; no forbidden items; $assetComparisons same-size game files compared, none identical; no personal data in $($stagedFiles.Count) files"
Write-Host ("Size:     {0:N2} MB uncompressed; {1:N2} MB ZIP" -f ($releaseSize / 1MB), ($zip.Length / 1MB))
Write-Host "SHA256:   $((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash)"
