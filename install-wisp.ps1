param(
    [Parameter(Mandatory = $false)]
    [string]$InstallDir = "$env:LOCALAPPDATA\Wisp",

    [Parameter(Mandatory = $false)]
    [string]$BuildDir = "build-run",

    [Parameter(Mandatory = $false)]
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",

    [Parameter(Mandatory = $false)]
    [switch]$NoBuild,

    [Parameter(Mandatory = $false)]
    [switch]$NoContextMenu,

    [Parameter(Mandatory = $false)]
    [switch]$NoPath,

    [Parameter(Mandatory = $false)]
    [switch]$NoShortcut,

    [Parameter(Mandatory = $false)]
    [switch]$Clean,

    [Parameter(Mandatory = $false)]
    [switch]$Help
)

function Write-Banner {
    Write-Host "================================================================" -ForegroundColor Cyan
    Write-Host "  Wisp Terminal Emulator - Windows Installer" -ForegroundColor Cyan
    Write-Host "================================================================" -ForegroundColor Cyan
    Write-Host ""
}

function Write-Step {
    param([string]$Message)
    Write-Host "  >> $Message" -ForegroundColor Yellow
}

function Write-Success {
    param([string]$Message)
    Write-Host "  [OK] $Message" -ForegroundColor Green
}

function Write-Warning {
    param([string]$Message)
    Write-Host "  [WARN] $Message" -ForegroundColor Magenta
}

function Write-ErrorMsg {
    param([string]$Message)
    Write-Host "  [ERROR] $Message" -ForegroundColor Red
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Find-VisualStudio {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installPath = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath -latest
        if ($installPath) { return $installPath }
    }
    $roots = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2026\Professional",
        "${env:ProgramFiles}\Microsoft Visual Studio\2026\Enterprise",
        "${env:ProgramFiles}\Microsoft Visual Studio\2026\Community",
        "${env:ProgramFiles}\Microsoft Visual Studio\2026\BuildTools",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools"
    )
    foreach ($root in $roots) {
        if (Test-Path -LiteralPath "$root\Common7\Tools\VsDevCmd.bat") { return $root }
    }
    return $null
}

function Invoke-Build {
    param(
        [string]$ScriptRoot,
        [string]$BuildDirName
    )

    Write-Step "Checking build environment..."

    $vsInstallPath = Find-VisualStudio
    if (-not $vsInstallPath) {
        throw "Visual Studio 2022 not found. Install it or use -NoBuild with a pre-built distribution."
    }

    $vsDevCmd = "$vsInstallPath\Common7\Tools\VsDevCmd.bat"
    Write-Step "Loading Visual Studio environment from: $vsDevCmd"

    $envDump = & cmd.exe /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >NUL && set"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to load Visual Studio environment."
    }
    foreach ($line in $envDump) {
        $idx = $line.IndexOf("=")
        if ($idx -gt 0) {
            $name = $line.Substring(0, $idx)
            $value = $line.Substring($idx + 1)
            Set-Item -Path "Env:$name" -Value $value
        }
    }

    $generator = if (Get-Command nmake.exe -ErrorAction SilentlyContinue) { "NMake Makefiles" }
                elseif (Get-Command ninja.exe -ErrorAction SilentlyContinue) { "Ninja" }
                else { throw "No supported build tool found (nmake or ninja)." }

    $buildDir = Join-Path $ScriptRoot $BuildDirName
    $cmakeCache = Join-Path $buildDir "CMakeCache.txt"

    $needConfigure = $Clean -or (-not (Test-Path -LiteralPath $cmakeCache))

    if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
        Write-Step "Clean build: removing $BuildDirName"
        Remove-Item -LiteralPath $buildDir -Recurse -Force
        $needConfigure = $true
    }

    if ($needConfigure) {
        Write-Step "Configuring Wisp ($Configuration, $generator)..."
        & cmake.exe -S $ScriptRoot -B $buildDir -G $generator "-DCMAKE_BUILD_TYPE=$Configuration" | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
    } else {
        Write-Step "Using existing build configuration in $BuildDirName"
    }

    Write-Step "Building Wisp..."
    & cmake.exe --build $buildDir | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }

    $distDir = Join-Path $buildDir "dist"
    if (-not (Test-Path -LiteralPath (Join-Path $distDir "Wisp.exe"))) {
        throw "Build completed but Wisp.exe not found in dist directory."
    }

    Write-Success "Build completed successfully."
    return $distDir
}

function Find-DistDirectory {
    param([string]$ScriptRoot)

    $candidates = @(
        (Join-Path $ScriptRoot "build-release\dist"),
        (Join-Path $ScriptRoot "build-run\dist"),
        (Join-Path $ScriptRoot "build\dist"),
        (Join-Path $ScriptRoot "dist")
    )

    $checkDist = Join-Path $ScriptRoot "Wisp.exe"
    if (Test-Path -LiteralPath $checkDist) {
        return $ScriptRoot
    }

    foreach ($dir in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $dir "Wisp.exe")) {
            return $dir
        }
    }

    return $null
}

function Install-Files {
    param(
        [string]$SourceDir,
        [string]$DestinationDir
    )

    Write-Step "Installing to: $DestinationDir"

    if (-not (Test-Path -LiteralPath $DestinationDir)) {
        New-Item -ItemType Directory -Path $DestinationDir -Force | Out-Null
    }

    $binDir = Join-Path $DestinationDir "bin"
    if (-not (Test-Path -LiteralPath $binDir)) {
        New-Item -ItemType Directory -Path $binDir -Force | Out-Null
    }

    $exes = Get-ChildItem -LiteralPath $SourceDir -Filter "*.exe"
    $count = 0
    $companionCount = 0
    foreach ($exe in $exes) {
        if ($exe.Name -eq "Wisp.exe") {
            Copy-Item -LiteralPath $exe.FullName -Destination (Join-Path $DestinationDir "Wisp.exe") -Force
            $count++
            Write-Success "Copied Wisp.exe"
        } else {
            Copy-Item -LiteralPath $exe.FullName -Destination (Join-Path $binDir $exe.Name) -Force
            $companionCount++
        }
    }

    if ($companionCount -gt 0) {
        Write-Success "Copied $companionCount companion utilities to bin\"
    }

    $resourceDirs = @("themes")
    foreach ($dir in $resourceDirs) {
        $srcDir = Join-Path $SourceDir $dir
        if (Test-Path -LiteralPath $srcDir) {
            $destDir = Join-Path $DestinationDir $dir
            if (-not (Test-Path -LiteralPath $destDir)) {
                New-Item -ItemType Directory -Path $destDir -Force | Out-Null
            }
            robocopy $srcDir $destDir /E /NP /NFL /NDL /NJH /NJS > $null
            Write-Success "Copied $dir\"
        }
    }

    return $count -gt 0
}

function Register-AppPaths {
    param([string]$WispPath)

    $appPaths = "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Wisp.exe"
    if (-not (Test-Path -LiteralPath $appPaths)) {
        New-Item -Path $appPaths -Force | Out-Null
    }
    Set-ItemProperty -LiteralPath $appPaths -Name "(default)" -Value $WispPath
    Write-Success "Registered App Paths entry."
}

function Add-ContextMenu {
    param([string]$WispPath)

    $menuName = "Wisp"
    $menuText = "Open in WISP"
    $command = '"' + $WispPath + '" --cwd "%V"'
    $folderCommand = '"' + $WispPath + '" --cwd "%1"'

    $backgroundKey = "HKCU:\SOFTWARE\Classes\Directory\Background\shell\$menuName"
    if (-not (Test-Path -LiteralPath $backgroundKey)) {
        New-Item -Path $backgroundKey -Force | Out-Null
    }
    Set-ItemProperty -LiteralPath $backgroundKey -Name "(default)" -Value $menuText
    Set-ItemProperty -LiteralPath $backgroundKey -Name "Icon" -Value "$WispPath,0"

    $bgCmdKey = "$backgroundKey\command"
    if (-not (Test-Path -LiteralPath $bgCmdKey)) {
        New-Item -Path $bgCmdKey -Force | Out-Null
    }
    Set-ItemProperty -LiteralPath $bgCmdKey -Name "(default)" -Value $command
    Write-Success "Added context menu: '$menuText' (folder background)"

    $folderKey = "HKCU:\SOFTWARE\Classes\Directory\shell\$menuName"
    if (-not (Test-Path -LiteralPath $folderKey)) {
        New-Item -Path $folderKey -Force | Out-Null
    }
    Set-ItemProperty -LiteralPath $folderKey -Name "(default)" -Value $menuText
    Set-ItemProperty -LiteralPath $folderKey -Name "Icon" -Value "$WispPath,0"

    $folderCmdKey = "$folderKey\command"
    if (-not (Test-Path -LiteralPath $folderCmdKey)) {
        New-Item -Path $folderCmdKey -Force | Out-Null
    }
    Set-ItemProperty -LiteralPath $folderCmdKey -Name "(default)" -Value $folderCommand
    Write-Success "Added context menu: '$menuText' (folder)"

    $driveKey = "HKCU:\SOFTWARE\Classes\Drive\shell\$menuName"
    if (-not (Test-Path -LiteralPath $driveKey)) {
        New-Item -Path $driveKey -Force | Out-Null
    }
    Set-ItemProperty -LiteralPath $driveKey -Name "(default)" -Value $menuText
    Set-ItemProperty -LiteralPath $driveKey -Name "Icon" -Value "$WispPath,0"

    $driveCmdKey = "$driveKey\command"
    if (-not (Test-Path -LiteralPath $driveCmdKey)) {
        New-Item -Path $driveCmdKey -Force | Out-Null
    }
    Set-ItemProperty -LiteralPath $driveCmdKey -Name "(default)" -Value $folderCommand
    Write-Success "Added context menu: '$menuText' (drives)"
}

function Add-ToPath {
    param([string]$InstallDir)

    $pathReg = "HKCU:\Environment"
    $currentPath = (Get-ItemProperty -LiteralPath $pathReg -Name "PATH" -ErrorAction SilentlyContinue).PATH

    $binDir = Join-Path $InstallDir "bin"
    $pathsToAdd = @($InstallDir)
    if (Test-Path -LiteralPath $binDir) {
        $pathsToAdd += $binDir
    }

    $newEntries = @()
    foreach ($p in $pathsToAdd) {
        if ($currentPath -and $currentPath.Split(";") -contains $p) {
            Write-Warning "$p already in PATH"
        } else {
            $newEntries += $p
        }
    }

    if ($newEntries.Count -gt 0) {
        $separator = if ($currentPath) { ";" } else { "" }
        $newPath = $currentPath + $separator + ($newEntries -join ";")
        Set-ItemProperty -LiteralPath $pathReg -Name "PATH" -Type ExpandString -Value $newPath
        foreach ($p in $newEntries) {
            Write-Success "Added to user PATH: $p"
        }

        $env:Path = [Environment]::GetEnvironmentVariable("Path", "User") + ";" + [Environment]::GetEnvironmentVariable("Path", "Machine")
        Write-Warning "PATH updated. Restart applications to pick up changes."
    }
}

function Create-DesktopShortcut {
    param([string]$WispPath)

    $desktop = [Environment]::GetFolderPath("Desktop")
    $shortcutPath = Join-Path $desktop "Wisp.lnk"

    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = $WispPath
    $shortcut.WorkingDirectory = Split-Path -Parent $WispPath
    $shortcut.Description = "Wisp Terminal Emulator"
    $shortcut.Save()

    Write-Success "Created desktop shortcut."
}

function Remove-OldContextMenu {
    $menuName = "Wisp"
    $keys = @(
        "HKCU:\SOFTWARE\Classes\Directory\Background\shell\$menuName",
        "HKCU:\SOFTWARE\Classes\Directory\shell\$menuName",
        "HKCU:\SOFTWARE\Classes\Drive\shell\$menuName"
    )
    foreach ($key in $keys) {
        if (Test-Path -LiteralPath $key) {
            Remove-Item -LiteralPath $key -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Show-Help {
    Write-Host @"
Usage: .\install-wisp.ps1 [Options]

Options:
  -InstallDir <path>   Installation directory (default: %%LOCALAPPDATA%%\Wisp)
  -BuildDir <path>     CMake build directory name (default: build-run)
  -Configuration <cfg> Build configuration: Debug, Release, RelWithDebInfo, MinSizeRel (default: Release)
  -NoBuild             Skip build step; find existing distribution in repo
  -NoContextMenu       Skip Explorer context menu registration
  -NoPath              Skip adding to user PATH
  -NoShortcut          Skip desktop shortcut creation
  -Clean               Force clean rebuild (removes build directory)
  -Help                Show this help

Examples:
  .\install-wisp.ps1                        # Build + install
  .\install-wisp.ps1 -NoBuild               # Install pre-built binaries
  .\install-wisp.ps1 -Configuration Debug   # Debug build + install
  .\install-wisp.ps1 -Clean                 # Clean rebuild + install
  .\install-wisp.ps1 -NoContextMenu -NoPath # Minimal install
"@
    exit 0
}

# --- Main ------------------------------------------------------------------

Clear-Host
Write-Banner

if ($Help) { Show-Help }

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$isUpdate = Test-Path -LiteralPath (Join-Path $InstallDir "Wisp.exe")

if ($isUpdate) {
    Write-Host "  Existing installation detected at: $InstallDir" -ForegroundColor Yellow
    Write-Host "  Script will update it with latest build." -ForegroundColor Yellow
    Write-Host ""
}

# -- Step 1: Build or locate distribution ------------------------------------
if ($NoBuild) {
    Write-Step "Skipping build (-NoBuild). Looking for pre-built distribution..."
    $distDir = Find-DistDirectory -ScriptRoot $scriptRoot
    if (-not $distDir) {
        Write-ErrorMsg "No pre-built distribution found. Run without -NoBuild or build manually first."
        exit 1
    }
    Write-Success "Found distribution at: $distDir"
} else {
    Write-Step "Building Wisp from source..."
    try {
        $distDir = Invoke-Build -ScriptRoot $scriptRoot -BuildDirName $BuildDir
    } catch {
        Write-ErrorMsg "Build failed: $_"
        Write-Host "  Tip: Run with -NoBuild if you have a pre-built distribution." -ForegroundColor Yellow
        exit 1
    }
}

# -- Step 2: Copy files ------------------------------------------------------
Write-Step "Installing files..."
$installed = Install-Files -SourceDir $distDir -DestinationDir $InstallDir
if (-not $installed) {
    Write-ErrorMsg "No files were installed."
    exit 1
}

$wshPath = Join-Path $InstallDir "Wisp.exe"

# -- Step 3: (Wisp writes its own default config on first run) ---------------

# -- Step 4: Register App Paths ----------------------------------------------
Write-Step "Registering application paths..."
Register-AppPaths -WispPath $wshPath

# -- Step 5: Context menu ----------------------------------------------------
if (-not $NoContextMenu) {
    Write-Step "Adding Explorer context menu entries..."
    Remove-OldContextMenu
    Add-ContextMenu -WispPath $wshPath
} else {
    Write-Step "Skipping context menu registration (-NoContextMenu)."
}

# -- Step 6: PATH ------------------------------------------------------------
if (-not $NoPath) {
    Write-Step "Adding to user PATH..."
    Add-ToPath -InstallDir $InstallDir
} else {
    Write-Step "Skipping PATH modification (-NoPath)."
}

# -- Step 7: Desktop shortcut ------------------------------------------------
if (-not $NoShortcut) {
    $desktopShortcut = Join-Path ([Environment]::GetFolderPath("Desktop")) "Wisp.lnk"
    if (-not (Test-Path -LiteralPath $desktopShortcut)) {
        Write-Step "Creating desktop shortcut..."
        Create-DesktopShortcut -WispPath $wshPath
    } else {
        Write-Step "Desktop shortcut already exists, skipping."
    }
} else {
    Write-Step "Skipping desktop shortcut (-NoShortcut)."
}

# -- Done --------------------------------------------------------------------
Write-Host ""
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "  Installation complete!" -ForegroundColor Green
if ($isUpdate) {
    Write-Host "  Wisp has been updated to the latest version." -ForegroundColor Green
} else {
    Write-Host "  Wisp has been installed successfully." -ForegroundColor Green
}
Write-Host ""
Write-Host "  What's next:"
Write-Host "    * Run Wisp from Start menu (search 'Wisp')"
Write-Host "    * Right-click any folder -> 'Open in WISP'"
Write-Host "    * Type 'wisp' in any terminal (if added to PATH)"
if (-not $NoPath) {
    Write-Host "    * Companion utilities available in: $InstallDir\bin\"
}
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host ""

if (-not $isUpdate) {
    $launchNow = Read-Host "Launch Wisp now? [Y/N]"
    if ($launchNow -eq "Y" -or $launchNow -eq "y") {
        Start-Process -FilePath $wshPath
    }
}
