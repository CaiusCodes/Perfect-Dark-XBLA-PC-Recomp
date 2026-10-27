# Builds the launcher, the release's only program at the top level:
#   out\launcher\Perfect Dark.exe   (sources: packaging\launcher\*.cpp)
# It starts the game in resources\runtime, and shows the setup window first when
# the game data has not been imported yet. A plain native Win32 program: static
# CRT (it needs nothing installed to start), no packing, no embedded payload,
# no scripts. It carries the default perfect_dark.toml as a resource, plus the
# game icon and version information.
#
# Because it compiles in the STFS package reader (packaging\launcher\stfs.cpp,
# derived from Velocity), the launcher is licensed under the GNU GPL version 3.
[CmdletBinding()]
param(
    # Defaults to the VERSION file, the single source of truth.
    [string]$Version,
    [string]$Output
)

$ErrorActionPreference = 'Stop'
$packaging = $PSScriptRoot
$projectRoot = Split-Path -Parent $packaging
if (-not $Version) {
    $Version = ([System.IO.File]::ReadAllText((Join-Path $projectRoot 'VERSION'))).Trim()
}
if ($Version -notmatch '^[0-9][0-9A-Za-z.-]*$') { throw "Unexpected version text: $Version" }
if (-not $Output) { $Output = Join-Path $projectRoot 'out\launcher\Perfect Dark.exe' }
$Output = [System.IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path (Split-Path -Parent $Output) -Force | Out-Null

$llvm = 'C:\Program Files\LLVM\bin'
$clang = Join-Path $llvm 'clang-cl.exe'
$rc = Join-Path $llvm 'llvm-rc.exe'
foreach ($tool in @($clang, $rc)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Missing build tool: $tool" }
}

$icon = Join-Path $packaging 'perfect_dark.ico'
$defaultConfig = Join-Path $packaging 'perfect_dark.toml'
foreach ($required in @($icon, $defaultConfig)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing launcher input: $required" }
}

# "0.9.9.2" -> 0,9,9,2 for the binary version fields.
$parts = @(($Version -replace '[^0-9.].*$', '').Split('.') | Where-Object { $_ -ne '' })
while ($parts.Count -lt 4) { $parts += '0' }
$fileVersion = ($parts[0..3]) -join ','

$sourceDir = Join-Path $packaging 'launcher'
$work = Join-Path ([System.IO.Path]::GetTempPath()) ('pd-launcher-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    Copy-Item -LiteralPath $icon -Destination (Join-Path $work 'app.ico')
    Copy-Item -LiteralPath $defaultConfig -Destination (Join-Path $work 'perfect_dark.toml')
    # asInvoker: never asks for administrator rights. PerMonitorV2: the setup
    # window scales itself for each monitor's DPI instead of being stretched.
    Set-Content -LiteralPath (Join-Path $work 'app.manifest') -Encoding ASCII -Value @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="PerfectDark.Launcher" version="1.0.0.0" processorArchitecture="amd64"/>
  <description>Perfect Dark XBLA PC Recomp</description>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security><requestedPrivileges><requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security>
  </trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application><supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/></application>
  </compatibility>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
      <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
    </windowsSettings>
  </application>
</assembly>
'@
    $rcPath = Join-Path $work 'launcher.rc'
    Set-Content -LiteralPath $rcPath -Encoding ASCII -Value @"
1 ICON "app.ico"
1 24 "app.manifest"
101 RCDATA "perfect_dark.toml"
1 VERSIONINFO
 FILEVERSION $fileVersion
 PRODUCTVERSION $fileVersion
 FILEOS 0x40004
 FILETYPE 0x1
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "Perfect Dark XBLA PC Recomp project"
      VALUE "FileDescription", "Perfect Dark"
      VALUE "ProductName", "Perfect Dark XBLA PC Recomp"
      VALUE "ProductVersion", "$Version"
      VALUE "FileVersion", "$Version"
      VALUE "InternalName", "Perfect Dark launcher"
      VALUE "OriginalFilename", "Perfect Dark.exe"
      VALUE "LegalCopyright", "Launcher: GPL-3.0-only (see the licenses folder)"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"@
    $resPath = Join-Path $work 'launcher.res'
    Push-Location $work
    try {
        & $rc /nologo /FO $resPath $rcPath
        if ($LASTEXITCODE -ne 0) { throw 'llvm-rc failed for the launcher' }
    }
    finally {
        Pop-Location
    }

    $sources = @(Get-ChildItem -LiteralPath $sourceDir -Filter *.cpp | ForEach-Object FullName)
    # No build paths in the binary: the source and work folders get neutral names.
    $mapSource = '/clang:-ffile-prefix-map=' + ($projectRoot -replace '\\', '/') + '=perfect-dark'
    $mapWork = '/clang:-ffile-prefix-map=' + ($work -replace '\\', '/') + '=launcher'
    & $clang /nologo /std:c++17 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE /DNOMINMAX "/DPD_LAUNCHER_VERSION=$Version" `
        $mapSource $mapWork `
        "/I$sourceDir" "/Fo$work\" @sources "/Fe$Output" /link /SUBSYSTEM:WINDOWS `
        user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib uuid.lib bcrypt.lib $resPath
    if ($LASTEXITCODE -ne 0) { throw 'clang-cl failed for the launcher' }
    Write-Host ('Built {0} ({1:N0} bytes)' -f $Output, (Get-Item -LiteralPath $Output).Length)
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
