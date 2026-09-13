#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Install-STR.ps1"

function Assert([bool]$Condition, [string]$Message) { if (-not $Condition) { throw "TEST FAILED: $Message" } }
function Assert-Throws([scriptblock]$Code, [string]$Message) {
    $thrown = $false
    try { & $Code | Out-Null } catch { $thrown = $true }
    Assert $thrown $Message
}
function Put([string]$Path, [string]$Text) {
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force
    [IO.File]::WriteAllText($Path, $Text)
}

# All install operations stay in fresh fixtures. Do not use registry discovery, actual games or process launch.
$sandbox = Join-Path ([IO.Path]::GetTempPath()) ('str-installer-tests-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $sandbox
$game = Join-Path $sandbox 'Steam library\Skyrim Special Edition'
$package = Join-Path $sandbox 'Paquete con espacios'
$userRoot = Join-Path $sandbox 'Usuario'
$session = Join-Path $userRoot 'STR-Playtest\active-session.json'
function Get-ExecutableVersion([string]$Path) { return $script:fixtureVersion }
$script:fixtureVersion = '1.7.104.0'
function Assert-GameStopped { }

try {
    foreach ($relative in @('SkyrimTogetherReborn\SkyrimTogether.exe', 'SkyrimTogetherReborn\SkyrimTogetherServer.exe',
        'SkyrimTogetherReborn\STServer.dll', 'SkyrimTogetherReborn\TPProcess.exe', 'SkyrimTogetherReborn\UI\index.html',
        'SkyrimTogether.esp', 'SkyrimTogetherQuestPatches.esp', 'scripts\test.pex')) {
        Put (Join-Path $package $relative) "package:$relative"
    }
    Put (Join-Path $game 'SkyrimSE.exe') 'fixture-not-executable'
    Assert ((Get-GameVersion $game $package) -eq '1-7-104-0') 'accept current 1.7.104.0 with correct address library filename'
    $script:fixtureVersion = '1.6.1170.0'
    Assert-Throws { Get-GameVersion $game $package } 'reject version incompatible with current client'
    Put (Join-Path $package 'build-info.json') '{"SupportedGameVersions":["1.6.1170.0"]}'
    Assert ((Get-GameVersion $game $package) -eq '1-6-1170-0') 'honor explicit package compatibility instead of installer release'
    Put (Join-Path $package 'build-info.json') '{"SupportedGameVersions":[]}'
    Assert-Throws { Get-GameVersion $game $package } 'reject invalid compatibility metadata'
    Remove-Item -LiteralPath (Join-Path $package 'build-info.json')
    $script:fixtureVersion = '1.7.104.0'
    Put (Join-Path $game 'Data\SKSE\Plugins\versionlib-1-7-104-0.bin') 'existing library'
    $original = Join-Path $game 'Data\scripts\test.pex'
    Put $original 'previous script'
    $plugins = Join-Path $userRoot 'Skyrim Special Edition\plugins.txt'
    Put $plugins "# custom comment`r`n*Other.esp`r`nSkyrimTogether.esp`r`n*SkyrimTogether.esp`r`n"
    $pluginsBefore = (Get-FileHash $plugins).Hash
    $exeBefore = (Get-FileHash (Join-Path $game 'SkyrimSE.exe')).Hash
    Assert-Throws { Get-SafeChild $game '..\escape' } 'reject traversal'
    Assert-Throws { Get-SafeChild $game 'C:\escape' } 'reject absolute path'
    Assert-Throws { Get-SafeChild $game 'test:stream' } 'reject alternate data stream'

    $null = Install-Package $game $package '' $userRoot $session
    Assert ((Get-Content $original -Raw) -eq 'package:scripts\test.pex') 'install file'
    $active = Get-Content $plugins -Raw
    Assert ($active.Contains('*Other.esp')) 'preserve other plugins'
    Assert (([regex]::Matches($active, '(?m)^\*SkyrimTogether\.esp\r?$')).Count -eq 1) 'deduplicate and activate'
    Assert ($active.IndexOf('*SkyrimTogether.esp') -lt $active.IndexOf('*SkyrimTogetherQuestPatches.esp')) 'plugin dependency order'
    $markerBefore = Get-Content $session -Raw
    $null = Install-Package $game $package '' $userRoot $session
    Assert ((Get-Content $session -Raw) -ceq $markerBefore) 'second install is idempotent'
    Restore-Session $userRoot
    Assert ((Get-Content $original -Raw) -eq 'previous script') 'restore overwritten file'
    Assert ((Get-FileHash $plugins).Hash -eq $pluginsBefore) 'restore exact plugin bytes'
    Assert (-not (Test-Path (Join-Path $game 'Data\SkyrimTogether.esp'))) 'remove newly installed plugin'
    Assert ((Get-FileHash (Join-Path $game 'SkyrimSE.exe')).Hash -eq $exeBefore) 'never change game exe'
    Assert (-not (Test-Path $session)) 'clear recovery marker'
    Assert (-not (Test-Path (Join-Path $game 'steam_appid.txt'))) 'restore launcher Steam side effect'

    # Simulate process interruption: restoring from disk must work without in-memory install state.
    $null = Install-Package $game $package '' $userRoot $session
    Put $original 'external edit'
    Assert-Throws { Restore-Session $userRoot } 'do not overwrite unrelated edits during recovery'
    Assert (Test-Path $session) 'retain recovery after conflict'
    Put $original 'package:scripts\test.pex'
    Restore-Session $userRoot
    Assert ((Get-Content $original -Raw) -eq 'previous script') 'recover interrupted session'

    # Force an IO failure after the first write; rollback must restore it byte-for-byte.
    $tx = Join-Path $sandbox 'rollback'
    $hash = (Get-FileHash $original).Hash
    $changes = @(
        [pscustomobject]@{ Kind='Text'; Destination=$original; Text='temporary' },
        [pscustomobject]@{ Kind='Copy'; Destination=(Join-Path $game 'Data\new.txt'); Source=(Join-Path $sandbox 'missing'); Hash='none' }
    )
    Assert-Throws { Invoke-FileTransaction $changes $tx '' $game } 'copy failure propagates'
    Assert ((Get-FileHash $original).Hash -eq $hash) 'rollback first write after later failure'
    Assert (-not (Test-Path (Join-Path $game 'Data\new.txt'))) 'rollback new file'

    # Exercise Main lifecycle without starting any executable or accessing the real user profile.
    $savedLocalAppData = $env:LOCALAPPDATA
    try {
        $env:LOCALAPPDATA = $userRoot
        $GamePath = $game
        $PackagePath = $package
        $script:launchCode = 0
        function Start-Process {
            param($FilePath, $WorkingDirectory, $ArgumentList, [switch]$PassThru)
            Assert ($FilePath -eq (Join-Path $game 'Data\SkyrimTogetherReborn\SkyrimTogether.exe')) 'launch STR rather than vanilla'
            Assert ($ArgumentList[1] -eq ('"' + (Join-Path $game 'SkyrimSE.exe') + '"')) 'quote paths with spaces'
            Assert (Test-Path $session) 'backup journal exists before launch'
            $fake = [pscustomobject]@{ ExitCode=$script:launchCode }
            $fake | Add-Member ScriptMethod WaitForExit { }
            $fake | Add-Member ScriptMethod Refresh { }
            return $fake
        }
        Main
        Assert ((Get-FileHash $plugins).Hash -eq $pluginsBefore) 'normal game exit restores profile'
        $script:launchCode = 3
        Assert-Throws { Main } 'report nonzero launcher exit'
        Assert (-not (Test-Path $session)) 'failed game exit restores profile too'
        $script:launchCode = 0
        $installedLibrary = Join-Path $game 'Data\SKSE\Plugins\versionlib-1-7-104-0.bin'
        Remove-Item -LiteralPath $installedLibrary
        Assert-Throws { Install-Package $game $package '' $userRoot $session } 'missing dependency fails before installation'
        Assert (-not (Test-Path $session)) 'missing dependency creates no active session'
        $AddressLibraryPath = Join-Path $sandbox 'Address Library extracted'
        Put (Join-Path $AddressLibraryPath 'SKSE\Plugins\versionlib-1-7-104-0.bin') 'library fixture'
        Main
        Assert (-not (Test-Path $installedLibrary)) 'temporary dependency removed from game'
        Assert (Test-Path (Join-Path $userRoot 'STR-Playtest\AddressLibrary\versionlib-1-7-104-0.bin')) 'dependency cached outside game'
        $AddressLibraryPath = ''
        Main
        Assert (-not (Test-Path $installedLibrary)) 'cached dependency installs and restores without prompting'
        Remove-Item Function:\Start-Process
    } finally { $env:LOCALAPPDATA = $savedLocalAppData }

    $manifest = @(Get-PackageFiles $package | ForEach-Object { @{ Path=$_.Relative; SHA256=$_.Hash } })
    $manifest | ConvertTo-Json | Set-Content (Join-Path $package 'package-files.json') -Encoding UTF8
    $null = Get-PackageFiles $package
    Put (Join-Path $package 'SkyrimTogether.esp') 'tampered'
    Assert-Throws { Get-PackageFiles $package } 'reject tampered package before writes'
    Assert ((Get-FileHash $plugins).Hash -eq $pluginsBefore) 'failed validation leaves plugins untouched'
    # Invoke the REAL -File entry point with no PackagePath and an unrelated CWD.
    # Copy only a host executable to supply authentic Windows version resources;
    # CheckOnly must never execute it or change this fixture installation.
    Put (Join-Path $package 'SkyrimTogether.esp') 'package:SkyrimTogether.esp'
    Remove-Item -LiteralPath (Join-Path $package 'package-files.json')
    Copy-Item -LiteralPath "$PSScriptRoot\Install-STR.ps1" -Destination (Join-Path $package 'Install-STR.ps1')
    $hostExe = (Get-Process -Id $PID).Path
    Copy-Item -LiteralPath $hostExe -Destination (Join-Path $game 'SkyrimSE.exe') -Force
    $v = (Get-Item -LiteralPath $hostExe).VersionInfo
    $hostVersion = '{0}.{1}.{2}.{3}' -f $v.FileMajorPart, $v.FileMinorPart, $v.FileBuildPart, $v.FilePrivatePart
    @{ SupportedGameVersions=@($hostVersion) } | ConvertTo-Json | Set-Content (Join-Path $package 'build-info.json') -Encoding UTF8
    Put (Join-Path $game ('Data\SKSE\Plugins\versionlib-' + $hostVersion.Replace('.', '-') + '.bin')) 'fixture library'
    Push-Location $sandbox
    try {
        $entryOutput = & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $package 'Install-STR.ps1') -GamePath $game -CheckOnly 2>&1
        Assert ($LASTEXITCODE -eq 0) "real script entry point: $entryOutput"
        Assert (($entryOutput -join ' ').Contains('Comprobacion correcta')) 'CheckOnly reaches end of real preflight'
        Assert (($entryOutput -join ' ').Contains($package)) 'default package path comes from script location'
        Assert ((Get-FileHash $plugins).Hash -eq $pluginsBefore) 'CheckOnly does not activate plugins'
        Assert (-not (Test-Path (Join-Path $game 'Data\SkyrimTogether.esp'))) 'CheckOnly does not install files'
    } finally { Pop-Location }
    Write-Host 'PASS: install, idempotency, exact restore, crash recovery, external edits, rollback, package hashes and unsafe paths.'
} finally {
    # Verify exact fixture root before recursive cleanup. Never cross into a game installation.
    $full = [IO.Path]::GetFullPath($sandbox)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($full.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Leaf $full) -match '^str-installer-tests-[a-f0-9]{32}$') {
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}
