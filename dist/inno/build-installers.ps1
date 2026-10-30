# Builds the Leapdesk KVM installer for Windows.
#
#   dist\inno\build-installers.ps1 [-BuildDir build] [-OutputDir <folder>]
#                                  [-SeedConfig <server .sgc>] [-SkipBuild]
#
# The build directory must be configured with the GUI (INPUTLEAP_BUILD_GUI=ON) and Qt
# on CMAKE_PREFIX_PATH, since the installer includes the Leapdesk KVM window. Inno
# Setup 6 is needed too: winget install JRSoftware.InnoSetup --scope user
#
# -SeedConfig is the server configuration installed on a machine that has none yet.
# It holds nothing secret: pairing certificates are never built into the installer.
param(
    [string]$BuildDir,
    [string]$OutputDir,
    [string]$SeedConfig,
    [string]$Iscc,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build' }
$BuildDir = (Resolve-Path $BuildDir).Path
if (-not $OutputDir) { $OutputDir = Join-Path $BuildDir 'installers' }

function Invoke-Checked([string]$what, [scriptblock]$command) {
    & $command
    if ($LASTEXITCODE -ne 0) { throw "$what failed with exit code $LASTEXITCODE" }
}

if (-not $Iscc) {
    $candidates = @(
        (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source,
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'))
    $Iscc = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $Iscc) { throw 'Inno Setup 6 not found; install it with: winget install JRSoftware.InnoSetup --scope user' }
}

$cache = Get-Content (Join-Path $BuildDir 'CMakeCache.txt')
if (-not ($cache -match '^INPUTLEAP_BUILD_GUI:BOOL=ON$')) {
    throw "$BuildDir is configured without the GUI; reconfigure it with -DINPUTLEAP_BUILD_GUI=ON"
}
if ($SeedConfig) { $SeedConfig = (Resolve-Path $SeedConfig).Path }

# configure again so the installer script picks up changes to its template
Invoke-Checked 'CMake configure' { cmake -S $repo -B $BuildDir | Out-Null }
if (-not $SkipBuild) {
    Invoke-Checked 'Build' { cmake --build $BuildDir --config Release --parallel }
}

$staging = Join-Path $BuildDir 'installer-staging'
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
Invoke-Checked 'Staging' { cmake --install $BuildDir --config Release --prefix $staging | Out-Null }
foreach ($file in 'leapdesk-daemon.exe', 'leapdesk-server.exe', 'leapdesk-client.exe', 'leapdesk.exe',
                  'vcruntime140.dll', 'msvcp140.dll', 'platforms\qwindows.dll') {
    if (-not (Test-Path (Join-Path $staging $file))) { throw "Staging is missing $file" }
}

$revision = (git -C $repo rev-parse --short=8 HEAD).Trim()
if (git -C $repo status --porcelain --untracked-files=no) { $revision += '-modified' }

New-Item -ItemType Directory -Force $OutputDir | Out-Null
$script = Join-Path $BuildDir 'installer-inno\leapdesk-kvm.iss'

# check the installer's own code first: selftest.iss runs it without installing anything
$selfTestDir = Join-Path $BuildDir 'installer-selftest'
New-Item -ItemType Directory -Force $selfTestDir | Out-Null
Invoke-Checked 'Compiling the self-test' {
    & $Iscc /Q "/DStagingDir=$staging" "/DOutputDir=$selfTestDir" (Join-Path $BuildDir 'installer-inno\selftest.iss')
}
$log = Join-Path $selfTestDir 'selftest.log'
Start-Process (Join-Path $selfTestDir 'LeapdeskKVM-selftest.exe') -ArgumentList "/LOG=`"$log`"" -Wait
$results = Select-String -Path $log -Pattern ' (PASS|FAIL) ' | ForEach-Object { $_.Line -replace '^.*? (PASS|FAIL) ', '$1 ' }
$failures = @($results | Where-Object { $_ -like 'FAIL *' })
"Self-test: $(@($results).Count - $failures.Count) passed, $($failures.Count) failed"
if ($failures.Count -gt 0 -or @($results).Count -eq 0) {
    $failures | ForEach-Object { "  $_" }
    throw "The installer self-test failed; see $log"
}

$defines = @("/DStagingDir=$staging", "/DOutputDir=$OutputDir", "/DRevision=$revision")
if ($SeedConfig) { $defines += "/DSeedConfig=$SeedConfig" }
Invoke-Checked 'Compiling the installer' { & $Iscc /Q @defines $script }

Get-ChildItem $OutputDir -Filter "LeapdeskKVM-*-$revision.exe" |
    ForEach-Object { '{0}  {1:N1} MB' -f $_.FullName, ($_.Length / 1MB) }
