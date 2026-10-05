param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",

    [string]$BuildDir = "build-run",

    [switch]$Clean,
    [switch]$NoRun,
    [switch]$RunTests
)

$ErrorActionPreference = "Stop"

function Find-VsDevCmd {
    $candidates = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installPaths = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        foreach ($installPath in $installPaths) {
            if ($installPath) {
                $candidates += Join-Path $installPath "Common7\Tools\VsDevCmd.bat"
            }
        }
    }

    $knownRoots = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Professional",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Enterprise",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\BuildTools",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Community"
    )
    foreach ($root in $knownRoots) {
        $candidates += Join-Path $root "Common7\Tools\VsDevCmd.bat"
    }

    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $candidate)) {
            continue
        }
        $checkCommand = "`"$candidate`" -arch=x64 -host_arch=x64 >NUL && where cl.exe 2>NUL"
        $check = & cmd.exe /c $checkCommand
        if ($LASTEXITCODE -eq 0 -and $check) {
            return $candidate
        }
    }

    return $null
}

function Import-VsDevEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        return
    }

    $vsDevCmd = Find-VsDevCmd
    if (-not $vsDevCmd) {
        throw "MSVC tools were not found. Install Visual Studio 2022 Build Tools with the C++ workload, or run this script from a Developer PowerShell/Command Prompt."
    }

    Write-Host "Loading Visual Studio build environment..."
    $envCommand = "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >NUL && set"
    $envDump = & cmd.exe /c $envCommand
    foreach ($line in $envDump) {
        $idx = $line.IndexOf("=")
        if ($idx -gt 0) {
            $name = $line.Substring(0, $idx)
            $value = $line.Substring($idx + 1)
            if ($name -ieq "Path" -and $env:Path -match "Microsoft Visual Studio" -and $value -notmatch "Microsoft Visual Studio") {
                continue
            }
            Set-Item -Path "Env:$name" -Value $value
        }
    }
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repoRoot

Import-VsDevEnvironment

if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    throw "cmake.exe was not found on PATH."
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "cl.exe was not found on PATH after loading the Visual Studio build environment."
}

$generator = $null
if (Get-Command nmake.exe -ErrorAction SilentlyContinue) {
    $generator = "NMake Makefiles"
} elseif (Get-Command ninja.exe -ErrorAction SilentlyContinue) {
    $generator = "Ninja"
} else {
    throw "Neither nmake.exe nor ninja.exe was found on PATH after loading the Visual Studio build environment."
}

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    $resolvedBuild = Resolve-Path -LiteralPath $BuildDir
    if (-not $resolvedBuild.Path.StartsWith($repoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove build directory outside the repository: $($resolvedBuild.Path)"
    }
    Remove-Item -LiteralPath $resolvedBuild.Path -Recurse -Force
}

if (-not (Test-Path -LiteralPath $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

Write-Host "Configuring Wisp ($Configuration, $generator)..."
# The build type must be quoted: PowerShell does not expand a variable inside
# a bare argument that starts with a dash, so an unquoted
# -DCMAKE_BUILD_TYPE=$Configuration reaches CMake literally. CMake then sees a
# build type it knows nothing about, applies no per-configuration flags, and
# Debug and Release produce the same unoptimized binary with no debug checks.
cmake.exe -S . -B $BuildDir -G $generator "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE."
}

Write-Host "Building Wisp..."
cmake.exe --build $BuildDir
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

if ($RunTests) {
    Write-Host "Running tests..."
    ctest.exe --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "Tests failed with exit code $LASTEXITCODE."
    }
}

$exe = Join-Path $BuildDir "dist\Wisp.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "Build completed, but $exe was not found."
}

if (-not $NoRun) {
    Write-Host "Starting $exe"
    Start-Process -FilePath (Resolve-Path -LiteralPath $exe).Path -WorkingDirectory (Join-Path $BuildDir "dist")
}
