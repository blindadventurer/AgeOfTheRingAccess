# Builds the mod and copies it into the game. Refuses while the game runs.
# Game folder changes are listed in NOTEBOOK.md.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$game = 'F:\games\AgeoftheRing\rotwk'

$running = Get-Process -ErrorAction SilentlyContinue | Where-Object {
    $_.Name -in @('game.dat', 'game', 'lotrbfme2ep1') }
if ($running) { throw "The game is running ($($running.Name -join ', ')). Close it first." }

& "$root\build.ps1"

$files = @(
    @{ From = "$root\vendor\ual-9.7.4\dinput8.dll"; To = "$game\winmm.dll" },
    @{ From = "$root\build\AotRAccess.asi";         To = "$game\scripts\AotRAccess.asi" },
    @{ From = "$root\lib\prism-x86\prism.dll";      To = "$game\scripts\AotRAccess\prism.dll" },
    @{ From = "$root\data\labels.ini";              To = "$game\scripts\AotRAccess\labels.ini" },
    @{ From = "$root\data\guides.ini";              To = "$game\scripts\AotRAccess\guides.ini" },
    @{ From = "$root\data\guides-auto.ini";         To = "$game\scripts\AotRAccess\guides-auto.ini" },
    @{ From = "$root\data\stealth.ini";             To = "$game\scripts\AotRAccess\stealth.ini" },
    @{ From = "$root\data\nocatch.ini";             To = "$game\scripts\AotRAccess\nocatch.ini" },
    @{ From = "$root\data\maptext.txt";             To = "$game\scripts\AotRAccess\maptext.txt" },
    @{ From = "$root\data\roads.txt";               To = "$game\scripts\AotRAccess\roads.txt" },
    @{ From = "$root\data\gates.txt";               To = "$game\scripts\AotRAccess\gates.txt" },
    @{ From = "$root\lib\prism\third-party\prism\mpl-2.0.txt"; To = "$game\scripts\AotRAccess\third-party\prism\mpl-2.0.txt" },
    @{ From = "$root\lib\prism\third-party\prism\NOTICE";      To = "$game\scripts\AotRAccess\third-party\prism\NOTICE" }
)
New-Item -ItemType Directory -Force "$game\scripts\AotRAccess\third-party\prism" | Out-Null
foreach ($f in $files) {
    if ((Test-Path $f.To) -and ($f.To -like '*\winmm.dll')) {
        $same = (Get-FileHash $f.From).Hash -eq (Get-FileHash $f.To).Hash
        if (-not $same) { throw "$($f.To) exists and is not our loader. Not overwriting." }
    }
    Copy-Item $f.From $f.To -Force
    Write-Host "Copied $($f.From) -> $($f.To)"
}
