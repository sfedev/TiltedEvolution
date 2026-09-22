#Requires -Version 5.1
<#
Local split screen playtest: one command starts the server and two clients on this machine.
  Client A: keyboard and mouse. Keep its window focused.
  Client B: gamepad. It reads the pad without focus once a save is loaded (STR_SPLITSCREEN_INPUT=pad).

Layouts:
  Monitors    one borderless window per monitor, each at that monitor's resolution (default with 2+ monitors)
  SideBySide  left/right halves of one monitor (default with a single monitor)
  Stacked     top/bottom halves of one monitor

Both clients share SkyrimPrefs.ini and read it only at startup, so the script backs it up and rewrites it before
each launch: A gets its window size and bGamepadEnable=0; once A has created its window, B gets its own size and
bGamepadEnable=1. The backup is restored when both clients exit (or run with -RestorePrefs after a crash).

Expected folder layout next to this script: Server\, ClientA\, ClientB\ (one runtime copy each).
#>
param(
    [ValidateSet('Auto', 'Monitors', 'SideBySide', 'Stacked')][string]$Layout = 'Auto',
    # 1-based monitor numbers as listed by -CheckOnly; 1 is the primary monitor.
    [int]$MonitorA = 1,
    [int]$MonitorB = 2,
    [string]$GameExe = 'C:\ST-Test\Game\SkyrimSE.exe',
    [string]$Endpoint = '127.0.0.1:10578',
    [switch]$NoAutoConnect,
    [switch]$CheckOnly,
    [switch]$RestorePrefs
)
$ErrorActionPreference = 'Stop'
$session = $PSScriptRoot
$prefs = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Skyrim Special Edition\SkyrimPrefs.ini'
$prefsBackup = $prefs + '.splitscreen-backup'
$launchLog = Join-Path $session 'LANZAMIENTOS.log'

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class SplitscreenNative
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct DEVMODE
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct DISPLAY_DEVICE
    {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
        public int StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
    }
    [DllImport("user32.dll", CharSet = CharSet.Ansi)] static extern bool EnumDisplaySettings(string device, int mode, ref DEVMODE devMode);
    [DllImport("user32.dll", CharSet = CharSet.Ansi)] static extern bool EnumDisplayDevices(string device, uint index, ref DISPLAY_DEVICE displayDevice, uint flags);
    delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr lParam);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int max);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);

    // Attached displays as {x, y, width, height} in physical pixels of the virtual screen, primary first.
    // EnumDisplaySettings is not affected by this process' DPI virtualization; the game is per-monitor DPI aware
    // and uses these same coordinates.
    public static int[][] Displays()
    {
        List<int[]> result = new List<int[]>();
        for (uint i = 0; ; i++)
        {
            DISPLAY_DEVICE device = new DISPLAY_DEVICE();
            device.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
            if (!EnumDisplayDevices(null, i, ref device, 0)) break;
            const int AttachedToDesktop = 0x1, PrimaryDevice = 0x4;
            if ((device.StateFlags & AttachedToDesktop) == 0) continue;
            DEVMODE mode = new DEVMODE();
            mode.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
            if (!EnumDisplaySettings(device.DeviceName, -1, ref mode)) continue;
            int[] row = new int[] { mode.dmPositionX, mode.dmPositionY, mode.dmPelsWidth, mode.dmPelsHeight };
            if ((device.StateFlags & PrimaryDevice) != 0) result.Insert(0, row); else result.Add(row);
        }
        return result.ToArray();
    }

    public static IntPtr FindGameWindow(uint processId)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr hwnd, IntPtr lParam)
        {
            uint pid;
            GetWindowThreadProcessId(hwnd, out pid);
            if (pid != processId || !IsWindowVisible(hwnd)) return true;
            StringBuilder text = new StringBuilder(256);
            GetWindowText(hwnd, text, text.Capacity);
            if (text.ToString().StartsWith("Skyrim")) { found = hwnd; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

function Write-LaunchLog([string]$Text) {
    $line = '{0} | {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Text
    Add-Content -LiteralPath $launchLog -Value $line -Encoding UTF8
    Write-Host $line
}

function Restore-Prefs {
    if (Test-Path -LiteralPath $prefsBackup) {
        Copy-Item -LiteralPath $prefsBackup -Destination $prefs -Force
        Remove-Item -LiteralPath $prefsBackup -Force
        Write-LaunchLog 'SkyrimPrefs.ini restaurado desde la copia de seguridad'
    }
}

# SkyrimPrefs.ini is plain ASCII without BOM; keep it that way.
function Set-IniValue([string]$Section, [string]$Key, [string]$Value) {
    $lines = [System.Collections.Generic.List[string]]([IO.File]::ReadAllLines($prefs, [Text.Encoding]::Default))
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].Trim() -ieq "[$Section]") { $start = $i; break }
    }
    if ($start -lt 0) { $lines.Add("[$Section]"); $lines.Add("$Key=$Value") }
    else {
        $done = $false
        for ($i = $start + 1; $i -lt $lines.Count -and -not $lines[$i].TrimStart().StartsWith('['); $i++) {
            if ($lines[$i] -match ('^\s*' + [regex]::Escape($Key) + '\s*=')) { $lines[$i] = "$Key=$Value"; $done = $true; break }
        }
        if (-not $done) { $lines.Insert($start + 1, "$Key=$Value") }
    }
    [IO.File]::WriteAllLines($prefs, $lines, [Text.Encoding]::Default)
}

$windowMarker = 'Split screen: window placed'

function Get-WindowMarkerCount([string]$Role) {
    # Counted from the file contents: the directory's LastWriteTime lags while the client keeps the log open.
    $count = 0
    Get-ChildItem -LiteralPath (Join-Path $session "$Role\logs") -Filter 'tp_client*.log' -ErrorAction SilentlyContinue | ForEach-Object {
        $count += @(Select-String -LiteralPath $_.FullName -Pattern $windowMarker -SimpleMatch).Count
    }
    return $count
}

function Wait-ClientWindow([string]$Role, [System.Diagnostics.Process]$Process, [int]$MarkersBefore) {
    # The client logs this marker when it has placed its window; SkyrimPrefs.ini was read long before.
    $deadline = (Get-Date).AddMinutes(4)
    while ((Get-Date) -lt $deadline) {
        if ($Process.HasExited) { throw "$Role se ha cerrado durante el arranque (codigo $($Process.ExitCode))" }
        if ((Get-WindowMarkerCount $Role) -gt $MarkersBefore) { return }
        Start-Sleep -Milliseconds 500
    }
    throw "$Role no ha creado su ventana en 4 minutos; revisar $session\$Role\logs"
}

# $Rect = x, y, width, height
function Start-Client([string]$Role, [string]$InputMode, [int[]]$Rect) {
    $working = Join-Path $session $Role
    Set-IniValue 'Display' 'iSize W' "$($Rect[2])"
    Set-IniValue 'Display' 'iSize H' "$($Rect[3])"
    Set-IniValue 'MAIN' 'bGamepadEnable' $(if ($InputMode -eq 'pad') { '1' } else { '0' })
    $env:STR_SPLITSCREEN_INPUT = $InputMode
    $env:STR_SPLITSCREEN_RECT = $Rect -join ','
    $env:STR_SPLITSCREEN_CONNECT = if ($NoAutoConnect) { '' } else { $Endpoint }
    $markersBefore = Get-WindowMarkerCount $Role
    $process = Start-Process -FilePath (Join-Path $working 'SkyrimTogether.exe') -WorkingDirectory $working -ArgumentList ('--exePath "' + $GameExe + '"') -PassThru
    Write-LaunchLog "$Role | PID=$($process.Id) | entrada=$InputMode | ventana=$($Rect -join ',') | logs=$working\logs"
    Wait-ClientWindow -Role $Role -Process $process -MarkersBefore $markersBefore
    return $process
}

if ($RestorePrefs) { Restore-Prefs; exit 0 }

# --- Checks -----------------------------------------------------------------------------------------------
foreach ($role in 'Server', 'ClientA', 'ClientB') {
    $exe = if ($role -eq 'Server') { 'SkyrimTogetherServer.exe' } else { 'SkyrimTogether.exe' }
    if (-not (Test-Path -LiteralPath (Join-Path $session "$role\$exe"))) { throw "Falta $role\$exe junto a este script" }
}
if (-not (Test-Path -LiteralPath $GameExe)) { throw "No existe $GameExe" }
if (-not (Test-Path -LiteralPath $prefs)) { throw "No existe $prefs" }
$plugins = Get-Content (Join-Path $env:LOCALAPPDATA 'Skyrim Special Edition\plugins.txt')
foreach ($required in '*SkyrimTogether.esp', '*SkyrimTogetherQuestPatches.esp') {
    if ($plugins -notcontains $required) { throw "Reactiva el perfil multijugador antes de arrancar: falta $required" }
}

$displays = [SplitscreenNative]::Displays()
if (-not $displays -or $displays.Count -eq 0) { throw 'No se pudo leer la configuracion de pantallas' }
for ($i = 0; $i -lt $displays.Count; $i++) {
    $d = $displays[$i]
    Write-Host ("Monitor {0}: {1}x{2} en {3},{4}{5}" -f ($i + 1), $d[2], $d[3], $d[0], $d[1], $(if ($i -eq 0) { ' (principal)' } else { '' }))
}
if ($Layout -eq 'Auto') { $Layout = if ($displays.Count -ge 2) { 'Monitors' } else { 'SideBySide' } }
if ($MonitorA -lt 1 -or $MonitorA -gt $displays.Count) { throw "MonitorA=$MonitorA no existe" }
$a = $displays[$MonitorA - 1]
switch ($Layout) {
    'Monitors' {
        if ($MonitorB -lt 1 -or $MonitorB -gt $displays.Count -or $MonitorB -eq $MonitorA) { throw "MonitorB=$MonitorB no es valido (hay $($displays.Count) monitores y debe ser distinto de MonitorA)" }
        $b = $displays[$MonitorB - 1]
        $rectA = @($a[0], $a[1], $a[2], $a[3])
        $rectB = @($b[0], $b[1], $b[2], $b[3])
    }
    'SideBySide' {
        $half = [int][math]::Floor($a[2] / 2)
        $rectA = @($a[0], $a[1], $half, $a[3])
        $rectB = @(($a[0] + $half), $a[1], $half, $a[3])
    }
    'Stacked' {
        $half = [int][math]::Floor($a[3] / 2)
        $rectA = @($a[0], $a[1], $a[2], $half)
        $rectB = @($a[0], ($a[1] + $half), $a[2], $half)
    }
}
Write-Host "Disposicion $Layout. A (teclado/raton): $($rectA -join ','). B (mando): $($rectB -join ',')."
if ($CheckOnly) { Write-Host 'Comprobaciones de archivos y pantallas correctas; no se ha arrancado nada.'; exit 0 }

if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { throw 'Abre Steam antes de lanzar los clientes' }
if (Get-Process SkyrimSE, SkyrimTogether, TPProcess -ErrorAction SilentlyContinue) { throw 'Ya hay un cliente de Skyrim abierto; cierralo antes' }
$foreignServer = Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue | Where-Object { $_.Path -ne (Join-Path $session 'Server\SkyrimTogetherServer.exe') }
if ($foreignServer) { throw "Hay un servidor de otra sesion abierto (PID $($foreignServer.Id -join ', ')); usa el mismo puerto y otra etiqueta de version. Cierralo antes." }

# --- Launch -----------------------------------------------------------------------------------------------
$startedServer = $null
$clients = @()
try {
    Restore-Prefs # leftover from an interrupted run
    Copy-Item -LiteralPath $prefs -Destination $prefsBackup
    Set-IniValue 'Display' 'bFull Screen' '0'
    Set-IniValue 'Display' 'bBorderless' '1'

    if (-not (Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue)) {
        $serverDir = Join-Path $session 'Server'
        $startedServer = Start-Process -FilePath (Join-Path $serverDir 'SkyrimTogetherServer.exe') -WorkingDirectory $serverDir -WindowStyle Hidden -PassThru
        Write-LaunchLog "Server | PID=$($startedServer.Id) | logs=$serverDir\logs"
    }

    $clientA = Start-Client -Role 'ClientA' -InputMode 'kbm' -Rect $rectA
    $clients += $clientA
    $clientB = Start-Client -Role 'ClientB' -InputMode 'pad' -Rect $rectB
    $clients += $clientB

    $windowA = [SplitscreenNative]::FindGameWindow([uint32]$clientA.Id)
    if ($windowA -ne [IntPtr]::Zero) { [void][SplitscreenNative]::SetForegroundWindow($windowA) }

    Write-Host ''
    Write-Host 'Los dos clientes estan abiertos. Manten el foco en la ventana A (teclado/raton); B se juega con el mando.'
    Write-Host 'El menu principal de B solo responde con foco: haz clic en B, carga su guardado y vuelve a hacer clic en A.'
    Write-Host 'Cada cliente conecta solo al servidor unos segundos despues de cargar su guardado.'
    Write-Host 'Esta ventana espera a que cierres ambos para restaurar SkyrimPrefs.ini.'
    foreach ($client in $clients) { $client.WaitForExit() }
}
finally {
    foreach ($client in $clients) { if (-not $client.HasExited) { Write-LaunchLog "Aviso: el cliente PID=$($client.Id) sigue abierto" } }
    if (-not ($clients | Where-Object { -not $_.HasExited })) { Restore-Prefs }
    else { Write-Host 'SkyrimPrefs.ini NO restaurado porque queda un cliente abierto. Al cerrarlo: Jugar-Splitscreen.ps1 -RestorePrefs' }
    if ($startedServer -and -not $startedServer.HasExited -and -not ($clients | Where-Object { -not $_.HasExited })) {
        Stop-Process -Id $startedServer.Id -Force
        Write-LaunchLog "Server | PID=$($startedServer.Id) detenido"
    }
}
