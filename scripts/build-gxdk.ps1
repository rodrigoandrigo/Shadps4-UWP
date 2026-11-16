param(
    [string]$ProjectPath = '',
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$LayoutDirectory,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('Scarlett', 'XboxOne')][string]$Target = 'Scarlett',
    [string]$GdkRoot = "${env:ProgramFiles(x86)}\Microsoft GDK",
    [string]$GdkEdition = '200600',
    [string]$MSBuildPath = '',
    [string]$GameOSPath = '',
    [string]$OutputDirectory = '',
    [ValidateRange(1, 256)][int]$Jobs = 4,
    [switch]$SkipBuild,
    [switch]$SkipSubmissionValidation
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = Split-Path $PSScriptRoot -Parent
$platform = "Gaming.Xbox.$Target.x64"
$gdk = Join-Path $GdkRoot $GdkEdition
$gameKit = Join-Path $gdk 'GXDK\gameKit'
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $repository "Build\GXDK-$Target-$Configuration\Packages"
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$LayoutDirectory = [IO.Path]::GetFullPath($LayoutDirectory)
$layoutPrefix = $LayoutDirectory.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if ($OutputDirectory.Equals($LayoutDirectory, [StringComparison]::OrdinalIgnoreCase) -or
    $OutputDirectory.StartsWith($layoutPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The package output must be outside the content layout.'
}

function Invoke-GdkTool([string]$Tool, [string[]]$Arguments, [string]$LogPath) {
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Tool @Arguments 2>&1 | Tee-Object -FilePath $LogPath | Out-Host
        $toolExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($toolExitCode) {
        throw "$([IO.Path]::GetFileName($Tool)) failed with exit code $toolExitCode. See $LogPath"
    }
}

function Assert-GameCoreImage([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5a4d) { throw "Not a PE image: $Path" }
        $stream.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 64 -or $peOffset -gt $stream.Length - 96) {
            throw "Invalid PE header: $Path"
        }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw "The GXDK layout requires x64 PE images: $Path"
        }
        $stream.Position = $peOffset + 24
        if ($reader.ReadUInt16() -ne 0x20b) { throw "Expected a PE32+ image: $Path" }
        $stream.Position = $peOffset + 24 + 70
        if ($reader.ReadUInt16() -band 0x1000) {
            throw "AppContainer/UWP image is not a native Game Core binary: $Path"
        }
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

if (!(Test-Path -LiteralPath (Join-Path $gameKit "Lib\amd64\$Target") -PathType Container)) {
    throw "GXDK $GdkEdition does not provide the $Target target at $gameKit"
}
if (!$GameOSPath) { $GameOSPath = Join-Path $gdk 'GXDK\Sideload\gameos.xvd' }
if (!(Test-Path -LiteralPath $GameOSPath -PathType Leaf)) {
    throw "The selected GXDK GameOS image was not found: $GameOSPath"
}
$GameOSPath = (Resolve-Path -LiteralPath $GameOSPath).Path
$toolDirectory = Join-Path $GdkRoot 'bin'
$tools = @{}
foreach ($name in @('makepkg', 'packageutil')) {
    $tool = Join-Path $toolDirectory "$name.exe"
    if (!(Test-Path -LiteralPath $tool -PathType Leaf)) { throw "GDK tool not found: $tool" }
    $tools[$name] = $tool
}

if (!$SkipBuild) {
    if (!$ProjectPath -or !(Test-Path -LiteralPath $ProjectPath -PathType Leaf)) {
        throw 'Pass -ProjectPath with a native Game Core .vcxproj. The shadPS4 XAML/UWP project cannot be converted to GXDK by packaging it as XVC.'
    }
    $ProjectPath = (Resolve-Path -LiteralPath $ProjectPath).Path
    [xml]$project = Get-Content -LiteralPath $ProjectPath -Raw
    $projectConfigurations = @($project.SelectNodes("//*[local-name()='ProjectConfiguration']") |
        ForEach-Object { $_.GetAttribute('Include') })
    if ($projectConfigurations -notcontains "$Configuration|$platform") {
        throw "$ProjectPath does not define $Configuration|$platform"
    }
    if (!$MSBuildPath) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $visualStudio = & $vswhere -latest -products '*' -version '[16.0,17.0)' `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (!$visualStudio) {
            throw 'GXDK 200600 requires a compatible Visual Studio toolchain. Install VS 2019 with GXDK integration, or pass -MSBuildPath for an explicitly configured toolchain.'
        }
        $MSBuildPath = Join-Path $visualStudio 'MSBuild\Current\Bin\MSBuild.exe'
    }
    if (!(Test-Path -LiteralPath $MSBuildPath -PathType Leaf)) {
        throw "MSBuild was not found: $MSBuildPath"
    }
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    Invoke-GdkTool $MSBuildPath @(
        $ProjectPath, '/t:Build', "/m:$Jobs", "/p:Configuration=$Configuration",
        "/p:Platform=$platform", "/p:LayoutDir=$($LayoutDirectory.TrimEnd('\'))\",
        "/p:XdkEditionTarget=$GdkEdition", '/v:minimal', '/nologo'
    ) (Join-Path $OutputDirectory 'build.log')
}

$configPath = Join-Path $LayoutDirectory 'MicrosoftGame.config'
if (!(Test-Path -LiteralPath $configPath -PathType Leaf)) {
    throw "The native Game Core content layout is missing MicrosoftGame.config: $LayoutDirectory"
}
if (Test-Path -LiteralPath (Join-Path $LayoutDirectory 'AppxManifest.xml')) {
    throw 'An APPX/UWP layout cannot be packaged as a native GXDK title.'
}
[xml]$config = Get-Content -LiteralPath $configPath -Raw
$executables = @($config.SelectNodes('/Game/ExecutableList/Executable'))
if (!$executables.Count) { throw 'MicrosoftGame.config does not declare an executable.' }
foreach ($executable in $executables) {
    if ($executable.GetAttribute('TargetDeviceFamily') -ne $Target) {
        throw "Every executable must target $Target in MicrosoftGame.config."
    }
    $image = [IO.Path]::GetFullPath((Join-Path $LayoutDirectory $executable.GetAttribute('Name')))
    if (!$image.StartsWith($layoutPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        !(Test-Path -LiteralPath $image -PathType Leaf)) {
        throw "Executable is missing or outside the content layout: $image"
    }
}
foreach ($image in Get-ChildItem -LiteralPath $LayoutDirectory -Recurse -File |
    Where-Object { $_.Extension -in @('.exe', '.dll') }) {
    Assert-GameCoreImage $image.FullName
}

$runDirectory = Join-Path $OutputDirectory ([guid]::NewGuid().ToString('N'))
$packDirectory = Join-Path $runDirectory 'packed'
New-Item -ItemType Directory -Force -Path $packDirectory | Out-Null
$mapFile = Join-Path $runDirectory 'PackageLayout.xml'
Invoke-GdkTool $tools.makepkg @('genmap', '/f', $mapFile, '/d', $LayoutDirectory) `
    (Join-Path $runDirectory 'genmap.log')
$packageArguments = @('/f', $mapFile, '/d', $LayoutDirectory, '/pd', $packDirectory,
    '/gameos', $GameOSPath, '/loggable')
if ($SkipSubmissionValidation) {
    Write-Warning 'Submission validation is disabled. The resulting development XVC is not submission-validated.'
    $packageArguments += '/skipvalidation'
} else {
    $packageArguments += '/validationcritical'
    Invoke-GdkTool $tools.makepkg (@('validate') + $packageArguments) `
        (Join-Path $runDirectory 'validate.log')
}
Invoke-GdkTool $tools.makepkg (@('pack') + $packageArguments) `
    (Join-Path $runDirectory 'pack.log')
$packages = @(Get-ChildItem -LiteralPath $packDirectory -Filter '*.xvc' -File)
if ($packages.Count -ne 1) { throw "Expected one XVC package; found $($packages.Count)." }
$signedPackage = $packages[0].FullName
Invoke-GdkTool $tools.packageutil @('license', $signedPackage) (Join-Path $runDirectory 'license.log')
if ((Get-Content -LiteralPath (Join-Path $runDirectory 'license.log') -Raw) -notmatch 'Test signed') {
    throw 'PackageUtil did not identify the package as development test-signed.'
}
Invoke-GdkTool $tools.packageutil @('info', $signedPackage) (Join-Path $runDirectory 'info.log')
$artifact = Get-Item -LiteralPath $signedPackage
$result = [ordered]@{
    package = $artifact.FullName
    bytes = $artifact.Length
    sha256 = (Get-FileHash -LiteralPath $signedPackage -Algorithm SHA256).Hash
    target = $platform
    configuration = $Configuration
    gdk_edition = $GdkEdition
    signing = 'MakePkg development test signing; not a production signature'
    submission_validation = $(if ($SkipSubmissionValidation) { 'skipped' } else { 'passed' })
    gameos_sha256 = (Get-FileHash -LiteralPath $GameOSPath -Algorithm SHA256).Hash
    config_sha256 = (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
}
$result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDirectory 'artifact.json') -Encoding UTF8
Write-Host "Development-signed GXDK package: $signedPackage"
