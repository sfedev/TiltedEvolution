#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$GamePath,
    [string]$PackagePath,
    [string]$AddressLibraryPath,
    [switch]$Restore,
    [switch]$CheckOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($PackagePath)) {
    $PackagePath = [IO.Path]::GetDirectoryName($PSCommandPath)
}

function Get-SafeChild([string]$Root, [string]$Relative) {
    if ([string]::IsNullOrWhiteSpace($Root) -or [string]::IsNullOrWhiteSpace($Relative)) { throw 'La raiz o la ruta relativa esta vacia.' }
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    if ([IO.Path]::IsPathRooted($Relative) -or $Relative.Contains(':')) { throw "Ruta no permitida: $Relative" }
    $full = [IO.Path]::GetFullPath((Join-Path $rootFull $Relative))
    if (-not $full.StartsWith($rootFull + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Ruta fuera del destino: $Relative" }
    # Do not follow junctions/symlinks in installation or backup paths.
    $cursor = $full
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "No se admiten enlaces o junctions en esta ruta: $cursor"
            }
        }
        $parent = Split-Path -Parent $cursor
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
    return $full
}

function Find-SteamGame {
    $roots = @()
    foreach ($key in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam')) {
        if (Test-Path $key) {
            $props = Get-ItemProperty $key
            foreach ($name in @('SteamPath', 'InstallPath')) {
                if ($props.PSObject.Properties[$name]) { $roots += $props.$name }
            }
        }
    }
    $libraries = @($roots)
    foreach ($root in $roots) {
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($match in [regex]::Matches([IO.File]::ReadAllText($vdf), '"path"\s+"([^"]+)"')) {
                $libraries += $match.Groups[1].Value.Replace('\\', '\')
            }
        }
    }
    $games = @(foreach ($library in ($libraries | Select-Object -Unique)) {
        $manifest = Join-Path $library 'steamapps\appmanifest_489830.acf'
        if (Test-Path -LiteralPath $manifest) {
            $match = [regex]::Match([IO.File]::ReadAllText($manifest), '"installdir"\s+"([^"]+)"')
            if ($match.Success) {
                $candidate = Get-SafeChild (Join-Path $library 'steamapps\common') $match.Groups[1].Value
                if (Test-Path -LiteralPath (Join-Path $candidate 'SkyrimSE.exe')) { $candidate }
            }
        }
    })
    return @($games | Sort-Object -Unique)
}

function Get-ExecutableVersion([string]$Path) {
    $exe = Get-Item -LiteralPath (Join-Path $Path 'SkyrimSE.exe')
    $v = $exe.VersionInfo
    return '{0}.{1}.{2}.{3}' -f $v.FileMajorPart, $v.FileMinorPart, $v.FileBuildPart, $v.FilePrivatePart
}

function Get-GameVersion([string]$Path, [string]$Package) {
    $detected = Get-ExecutableVersion $Path
    # Legacy playtest ZIPs have no metadata. Match their actual client/main.cpp,
    # not the older public wiki. New packages carry the versions extracted by CI.
    $supported = @('1.7.104.0')
    if ($Package -and (Test-Path -LiteralPath (Join-Path $Package 'build-info.json'))) {
        $info = Get-Content -LiteralPath (Join-Path $Package 'build-info.json') -Raw | ConvertFrom-Json
        if ($info.PSObject.Properties['SupportedGameVersions']) {
            $supported = @($info.SupportedGameVersions)
            if (-not $supported.Count -or @($supported | Where-Object { $_ -notmatch '^\d+\.\d+\.\d+\.\d+$' }).Count) {
                throw 'Metadatos de compatibilidad de Skyrim no validos.'
            }
        }
    }
    if ($supported -notcontains $detected) {
        throw "Skyrim detectado: $detected. Este paquete admite: $($supported -join ', '). Usa una version del juego compatible con el paquete."
    }
    return $detected.Replace('.', '-')
}

function Assert-GameStopped {
    $running = @(Get-Process -Name SkyrimSE, SkyrimTogether -ErrorAction SilentlyContinue)
    if ($running.Count) { throw 'Cierra Skyrim antes de instalar, restaurar o iniciar otra instancia.' }
}

function Assert-RuntimeStopped([string]$Runtime, [switch]$WaitForHelpers) {
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $running = @(Get-Process -Name SkyrimTogetherServer, TPProcess -ErrorAction SilentlyContinue | Where-Object {
            $_.Path -and $_.Path.StartsWith($Runtime.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)
        })
        if (-not $running.Count) { return }
        if (-not $WaitForHelpers -or @($running | Where-Object { $_.ProcessName -eq 'SkyrimTogetherServer' }).Count) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Quedan procesos usando el runtime de Data. Cierralos y ejecuta Restaurar-Skyrim.cmd. Aloja el servidor desde el paquete extraido.'
}

function Get-PackageFiles([string]$Root) {
    if ([string]::IsNullOrWhiteSpace($Root)) { throw 'Falta la carpeta del paquete. Extrae el instalador junto a SkyrimTogetherReborn.' }
    $rootFull = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
    $required = @('SkyrimTogetherReborn\SkyrimTogether.exe', 'SkyrimTogetherReborn\SkyrimTogetherServer.exe',
        'SkyrimTogetherReborn\STServer.dll', 'SkyrimTogetherReborn\TPProcess.exe',
        'SkyrimTogetherReborn\UI\index.html', 'SkyrimTogether.esp', 'SkyrimTogetherQuestPatches.esp')
    foreach ($relative in $required) {
        if (-not (Test-Path -LiteralPath (Get-SafeChild $rootFull $relative) -PathType Leaf)) {
            throw "Paquete incompleto: falta $relative. Extrae el ZIP jugable, no el de simbolos ni el codigo fuente."
        }
    }
    $files = @(foreach ($name in @('SkyrimTogetherReborn', 'meshes', 'scripts', 'SkyrimTogetherRebornBehaviors', 'SkyrimTogether.esp', 'SkyrimTogetherQuestPatches.esp')) {
        $path = Get-SafeChild $rootFull $name
        if (Test-Path -LiteralPath $path) {
            $items = if (Test-Path -LiteralPath $path -PathType Container) { Get-ChildItem -LiteralPath $path -File -Recurse } else { Get-Item -LiteralPath $path }
            foreach ($item in $items) {
                $relative = $item.FullName.Substring($rootFull.Length + 1)
                $null = Get-SafeChild $rootFull $relative
                # A distributed runtime must not import somebody else's settings/logs.
                if ($relative -match '^SkyrimTogetherReborn\\(config|logs|cache)\\') { continue }
                [pscustomobject]@{ Relative = $relative; Source = $item.FullName; Hash = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash }
            }
        }
    })
    $manifest = Join-Path $rootFull 'package-files.json'
    if (Test-Path -LiteralPath $manifest) {
        $expected = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
        $map = @{}
        foreach ($entry in $expected) {
            $relative = $entry.Path.Replace('/', '\')
            $null = Get-SafeChild $rootFull $relative
            if ($map.ContainsKey($relative)) { throw "Manifest duplicado: $relative" }
            $map[$relative] = $entry.SHA256
        }
        foreach ($file in $files) {
            if (-not $map.ContainsKey($file.Relative) -or $map[$file.Relative] -ne $file.Hash) {
                throw "El paquete no coincide con su manifiesto: $($file.Relative)"
            }
            $map.Remove($file.Relative)
        }
        if ($map.Count) { throw 'Faltan archivos indicados por el manifiesto del paquete.' }
    } else {
        Write-Host 'Paquete anterior sin manifiesto: se registraran sus hashes locales.'
    }
    return $files
}

function Get-PluginText([string]$Path) {
    $lines = @()
    if (Test-Path -LiteralPath $Path) { $lines = @([IO.File]::ReadAllLines($Path)) }
    # Keep other entries/comments and their relative order. Add our plugins once, in dependency order.
    $lines = @($lines | Where-Object { $_.Trim() -notmatch '^\*?(SkyrimTogether|SkyrimTogetherQuestPatches)\.esp$' })
    $lines += '*SkyrimTogether.esp', '*SkyrimTogetherQuestPatches.esp'
    return ($lines -join "`r`n") + "`r`n"
}

function Invoke-FileTransaction([object[]]$Changes, [string]$BackupRoot, [string]$SessionPath, [string]$GameRoot) {
    $null = New-Item -ItemType Directory -Path $BackupRoot
    $journal = @()
    # Back up ALL destinations before the first write. Journal also supports manual recovery.
    foreach ($change in $Changes) {
        $backup = Get-SafeChild $BackupRoot ('{0:D5}.bak' -f $journal.Count)
        $exists = Test-Path -LiteralPath $change.Destination -PathType Leaf
        if (Test-Path -LiteralPath $change.Destination -PathType Container) { throw "Se esperaba un archivo: $($change.Destination)" }
        if ($exists) { Copy-Item -LiteralPath $change.Destination -Destination $backup }
        $afterHash = $null
        if ($change.Kind -eq 'Copy') { $afterHash = $change.Hash }
        elseif ($change.Kind -eq 'Text') {
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $afterHash = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($change.Text))).Replace('-', '') } finally { $sha.Dispose() }
        }
        $originalHash = if ($exists) { (Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash } else { $null }
        $journal += [pscustomobject]@{ Destination = $change.Destination; Existed = $exists; Backup = $backup; OriginalHash = $originalHash; InstalledHash = $afterHash }
    }
    $journal | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $BackupRoot 'recovery.json') -Encoding UTF8
    if ($SessionPath) {
        [pscustomobject]@{ Game = $GameRoot; Backup = $BackupRoot } | ConvertTo-Json | Set-Content -LiteralPath $SessionPath -Encoding UTF8
    }
    $applied = 0
    try {
        foreach ($change in $Changes) {
            $applied++
            $parent = Split-Path -Parent $change.Destination
            if (-not (Test-Path -LiteralPath $parent)) { $null = New-Item -ItemType Directory -Path $parent -Force }
            if ($change.Kind -eq 'Copy') {
                Copy-Item -LiteralPath $change.Source -Destination $change.Destination -Force
                if ((Get-FileHash -LiteralPath $change.Destination -Algorithm SHA256).Hash -ne $change.Hash) { throw 'Fallo de verificacion tras copiar.' }
            } elseif ($change.Kind -eq 'Text') {
                [IO.File]::WriteAllText($change.Destination, $change.Text, (New-Object Text.UTF8Encoding($false)))
            } elseif ($change.Kind -eq 'Delete') {
                Remove-Item -LiteralPath $change.Destination -Force
            } else { throw "Operacion desconocida: $($change.Kind)" }
        }
    } catch {
        $failure = $_
        $rollbackErrors = @()
        for ($i = $applied - 1; $i -ge 0; $i--) {
            $entry = $journal[$i]
            try {
                if ($entry.Existed) { Copy-Item -LiteralPath $entry.Backup -Destination $entry.Destination -Force }
                elseif (Test-Path -LiteralPath $entry.Destination -PathType Leaf) { Remove-Item -LiteralPath $entry.Destination -Force }
            } catch { $rollbackErrors += $_.Exception.Message }
        }
        if ($rollbackErrors.Count) { Write-Warning "Restauracion incompleta. Conserva $BackupRoot : $($rollbackErrors -join '; ')" }
        elseif ($SessionPath -and (Test-Path -LiteralPath $SessionPath)) { Remove-Item -LiteralPath $SessionPath }
        throw $failure
    }
}

function Install-Package([string]$Game, [string]$Package, [string]$Library, [string]$UserRoot, [string]$SessionPath, [switch]$ValidateOnly) {
    $files = @(Get-PackageFiles $Package)
    $gameFull = (Resolve-Path -LiteralPath $Game).Path.TrimEnd('\')
    $version = Get-GameVersion $gameFull $Package
    $dataRoot = Get-SafeChild $gameFull 'Data'
    $libraryName = "versionlib-$version.bin"
    $libraryDest = Get-SafeChild $dataRoot "SKSE\Plugins\$libraryName"
    if (-not (Test-Path -LiteralPath $libraryDest -PathType Leaf)) {
        if (-not $Library) { throw "Falta Address Library ($libraryName). Descarga All in one (Anniversary Edition) desde https://www.nexusmods.com/skyrimspecialedition/mods/32444 , extraelo y vuelve a ejecutar indicando su carpeta." }
        $candidates = @(Get-ChildItem -LiteralPath $Library -Recurse -File -Filter $libraryName)
        if ($candidates.Count -ne 1) { throw "La carpeta de Address Library debe contener exactamente un $libraryName." }
        $safeSource = Get-SafeChild (Resolve-Path -LiteralPath $Library).Path $candidates[0].FullName.Substring((Resolve-Path -LiteralPath $Library).Path.TrimEnd('\').Length + 1)
        $files += [pscustomobject]@{ Relative = "SKSE\Plugins\$libraryName"; Source = $safeSource; Hash = (Get-FileHash -LiteralPath $safeSource -Algorithm SHA256).Hash }
    }
    $statePath = Get-SafeChild $dataRoot 'STR-Playtest-install.json'
    $old = $null
    if (Test-Path -LiteralPath $statePath) { $old = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json }
    $changes = @()
    $managed = @()
    foreach ($file in $files) {
        $destination = Get-SafeChild $dataRoot $file.Relative
        if ([IO.Path]::GetFullPath($file.Source) -ieq $destination) { throw 'Extrae el paquete fuera de la carpeta Data del juego.' }
        $managed += [pscustomobject]@{ Path = $file.Relative; SHA256 = $file.Hash }
        if (-not (Test-Path -LiteralPath $destination -PathType Leaf) -or (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $file.Hash) {
            $changes += [pscustomobject]@{ Kind = 'Copy'; Source = $file.Source; Destination = $destination; Hash = $file.Hash }
        }
    }
    if ($old) {
        foreach ($previous in $old.Files) {
            if ($previous.Path -like 'SKSE\Plugins\versionlib-*.bin') { continue }
            if ($managed.Path -contains $previous.Path) { continue }
            # Only remove obsolete files from the runtime owned by this installer.
            if ($previous.Path -notlike 'SkyrimTogetherReborn\*' -or $previous.Path -match '^SkyrimTogetherReborn\\(config|logs|cache)\\') { continue }
            $destination = Get-SafeChild $dataRoot $previous.Path
            if (Test-Path -LiteralPath $destination -PathType Leaf) {
                if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $previous.SHA256) { throw "Archivo antiguo modificado por otra herramienta: $destination. No se elimina automaticamente." }
                $changes += [pscustomobject]@{ Kind = 'Delete'; Destination = $destination }
            }
        }
    }
    $plugins = Get-SafeChild $UserRoot 'Skyrim Special Edition\plugins.txt'
    $pluginText = Get-PluginText $plugins
    if (-not (Test-Path -LiteralPath $plugins) -or [IO.File]::ReadAllText($plugins) -cne $pluginText) {
        $changes += [pscustomobject]@{ Kind = 'Text'; Destination = $plugins; Text = $pluginText }
    }
    # SteamLoader writes this file when loading Skyrim in the launcher process.
    $steamAppId = Get-SafeChild $gameFull 'steam_appid.txt'
    if (-not (Test-Path -LiteralPath $steamAppId) -or [IO.File]::ReadAllText($steamAppId) -cne '489830') {
        $changes += [pscustomobject]@{ Kind = 'Text'; Destination = $steamAppId; Text = '489830' }
    }
    $provenancePath = Join-Path $Package 'build-info.json'
    $provenance = if (Test-Path -LiteralPath $provenancePath) { Get-Content -LiteralPath $provenancePath -Raw | ConvertFrom-Json } else { 'Paquete sin metadatos CI; identificado por SHA-256' }
    if ($ValidateOnly) {
        Write-Host "Comprobacion correcta. Juego: $gameFull. Paquete: $Package. Cambios previstos: $($changes.Count). No se ha instalado ni arrancado nada."
        return
    }
    if ($changes.Count) {
        $backup = Get-SafeChild $UserRoot ('STR-Playtest\Backups\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
        $state = [ordered]@{ Game = $gameFull; Version = $version; InstalledAt = (Get-Date -Format o); Build = $provenance; Backup = $backup; Files = $managed }
        $changes += [pscustomobject]@{ Kind = 'Text'; Destination = $statePath; Text = ($state | ConvertTo-Json -Depth 8) }
        Invoke-FileTransaction $changes $backup $SessionPath $gameFull
        Write-Host "Instalacion verificada. Copia de seguridad: $backup"
    } else { Write-Host 'Instalacion comprobada: ya esta actualizada.' }
    Write-Host ('Build: ' + ($provenance | ConvertTo-Json -Compress))
    return Get-SafeChild $dataRoot 'SkyrimTogetherReborn\SkyrimTogether.exe'
}

function Restore-Session([string]$UserRoot) {
    $sessionPath = Get-SafeChild $UserRoot 'STR-Playtest\active-session.json'
    if (-not (Test-Path -LiteralPath $sessionPath)) { return }
    Assert-GameStopped
    $session = Get-Content -LiteralPath $sessionPath -Raw | ConvertFrom-Json
    $backupRoot = Get-SafeChild $UserRoot 'STR-Playtest\Backups'
    $backup = [IO.Path]::GetFullPath($session.Backup)
    if (-not $backup.StartsWith($backupRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Ruta de recuperacion no valida.' }
    $backup = Get-SafeChild $backupRoot $backup.Substring($backupRoot.Length + 1)
    $dataRoot = Get-SafeChild $session.Game 'Data'
    Assert-RuntimeStopped (Join-Path $dataRoot 'SkyrimTogetherReborn') -WaitForHelpers
    $plugins = Get-SafeChild $UserRoot 'Skyrim Special Edition\plugins.txt'
    $steamAppId = Get-SafeChild $session.Game 'steam_appid.txt'
    $entries = Get-Content -LiteralPath (Join-Path $backup 'recovery.json') -Raw | ConvertFrom-Json
    $entries = @($entries)
    # Validate all paths before restoring anything. Never execute commands from the journal.
    foreach ($entry in $entries) {
        $dest = [IO.Path]::GetFullPath($entry.Destination)
        if ($dest -ine $plugins -and $dest -ine $steamAppId) {
            if (-not $dest.StartsWith($dataRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Destino de recuperacion fuera de Data.' }
            $null = Get-SafeChild $dataRoot $dest.Substring($dataRoot.Length + 1)
        }
        $source = [IO.Path]::GetFullPath($entry.Backup)
        if (-not $source.StartsWith($backup + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Backup fuera de la sesion.' }
        $null = Get-SafeChild $backup $source.Substring($backup.Length + 1)
        if ($entry.Existed -and -not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Falta backup: $source" }
        if ($entry.Existed -and (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $entry.OriginalHash) { throw "Backup alterado: $source" }
        if (Test-Path -LiteralPath $dest -PathType Leaf) {
            $currentHash = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
            if ($currentHash -ne $entry.InstalledHash -and $currentHash -ne $entry.OriginalHash) {
                throw "Otro programa ha modificado $dest. No se sobrescribe; recuperacion disponible en $backup."
            }
        }
    }
    for ($i = $entries.Count - 1; $i -ge 0; $i--) {
        $entry = $entries[$i]
        if ($entry.Existed) { Copy-Item -LiteralPath $entry.Backup -Destination $entry.Destination -Force }
        elseif (Test-Path -LiteralPath $entry.Destination -PathType Leaf) { Remove-Item -LiteralPath $entry.Destination -Force }
    }
    Remove-Item -LiteralPath $sessionPath
    Write-Host 'Archivos y plugins anteriores restaurados. Ya puedes iniciar Skyrim desde Steam.'
}

function Main {
    if (-not $CheckOnly) { Assert-GameStopped }
    $sessionPath = Get-SafeChild $env:LOCALAPPDATA 'STR-Playtest\active-session.json'
    if (-not $CheckOnly) { Restore-Session $env:LOCALAPPDATA }
    if ($Restore) { return }
    $game = $GamePath
    if (-not $game) {
        $detected = @(Find-SteamGame)
        if ($detected.Count -eq 1) { $game = $detected[0] }
        else { $game = (Read-Host 'Carpeta de Skyrim que contiene SkyrimSE.exe').Trim('"') }
    }
    if ([string]::IsNullOrWhiteSpace($game)) { throw 'No se ha indicado ninguna carpeta de Skyrim.' }
    $game = (Resolve-Path -LiteralPath $game).Path
    $PackagePath = (Resolve-Path -LiteralPath $PackagePath).Path
    $version = Get-GameVersion $game $PackagePath
    $library = $AddressLibraryPath
    $cachedLibrary = Get-SafeChild $env:LOCALAPPDATA 'STR-Playtest\AddressLibrary'
    if (-not $library -and (Test-Path -LiteralPath (Join-Path $cachedLibrary "versionlib-$version.bin"))) { $library = $cachedLibrary }
    if (-not (Test-Path -LiteralPath (Join-Path $game "Data\SKSE\Plugins\versionlib-$version.bin")) -and -not $library) {
        Write-Host 'Necesitas Address Library: All in one (Anniversary Edition).'
        Write-Host 'https://www.nexusmods.com/skyrimspecialedition/mods/32444'
        $library = (Read-Host 'Carpeta de Address Library ya extraida (Enter para cancelar)').Trim('"')
        if (-not $library) { throw 'Instalacion cancelada antes de modificar archivos.' }
    }
    $runtimePath = Join-Path $game 'Data\SkyrimTogetherReborn'
    if ($CheckOnly) {
        Install-Package $game $PackagePath $library $env:LOCALAPPDATA $sessionPath -ValidateOnly
        return
    }
    Assert-RuntimeStopped $runtimePath
    $launcher = Install-Package $game $PackagePath $library $env:LOCALAPPDATA $sessionPath
    try {
        # Cache the dependency outside Skyrim so subsequent temporary sessions need no new prompt.
        if ($library) {
            $cachedFile = Get-SafeChild $cachedLibrary "versionlib-$version.bin"
            $installedFile = Join-Path $game "Data\SKSE\Plugins\versionlib-$version.bin"
            if (-not (Test-Path -LiteralPath $cachedFile)) {
                $null = New-Item -ItemType Directory -Path $cachedLibrary -Force
                Copy-Item -LiteralPath $installedFile -Destination $cachedFile
            }
        }
        Write-Host 'Iniciando Skyrim Together. Conecta con F2 despues de Helgen.'
        Write-Host 'Deja esta ventana abierta: restaurara la instalacion al cerrar el juego.'
        $process = Start-Process -FilePath $launcher -WorkingDirectory (Split-Path -Parent $launcher) -ArgumentList @('--exePath', ('"' + (Join-Path $game 'SkyrimSE.exe') + '"')) -PassThru
        $process.WaitForExit()
        $process.Refresh()
        $exitCode = $process.ExitCode
    } finally {
        Restore-Session $env:LOCALAPPDATA
    }
    if ($exitCode -ne 0) { throw "Skyrim Together termino con codigo $exitCode. Revisa sus logs en $runtimePath\logs." }
}

if ($MyInvocation.InvocationName -ne '.') {
    $mutex = New-Object Threading.Mutex($false, 'Local\STRPlaytestInstaller')
    $locked = $false
    try {
        try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
        if (-not $locked) { throw 'Ya hay una sesion del instalador abierta.' }
        Main
    } catch {
        Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host $_.InvocationInfo.PositionMessage
        Write-Host $_.ScriptStackTrace
        exit 1
    }
    finally { if ($locked) { $mutex.ReleaseMutex() }; $mutex.Dispose() }
}
