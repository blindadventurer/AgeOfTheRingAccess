# Builds the mod and packs a release archive: release\AotRAccess-<version>.zip.
# The archive mirrors the game's rotwk folder: extract it straight into rotwk.
# Same files as deploy.ps1, plus the licences and the README.
param([Parameter(Mandatory = $true)][string]$Version)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

& "$root\build.ps1"

$stage = "$root\release\stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$data = "$stage\scripts\AotRAccess"
$files = @(
    @{ From = "$root\vendor\ual-9.7.4\dinput8.dll";     To = "$stage\winmm.dll" },
    @{ From = "$root\build\AotRAccess.asi";             To = "$stage\scripts\AotRAccess.asi" },
    @{ From = "$root\lib\prism-x86\prism.dll";          To = "$data\prism.dll" },
    @{ From = "$root\data\labels.ini";                  To = "$data\labels.ini" },
    @{ From = "$root\data\guides.ini";                  To = "$data\guides.ini" },
    @{ From = "$root\data\guides-auto.ini";             To = "$data\guides-auto.ini" },
    @{ From = "$root\data\stealth.ini";                 To = "$data\stealth.ini" },
    @{ From = "$root\data\nocatch.ini";                 To = "$data\nocatch.ini" },
    @{ From = "$root\data\maptext.txt";                 To = "$data\maptext.txt" },
    @{ From = "$root\data\roads.txt";                   To = "$data\roads.txt" },
    @{ From = "$root\data\gates.txt";                   To = "$data\gates.txt" },
    @{ From = "$root\lib\prism\third-party\prism\mpl-2.0.txt"; To = "$data\third-party\prism\mpl-2.0.txt" },
    @{ From = "$root\lib\prism\third-party\prism\NOTICE";      To = "$data\third-party\prism\NOTICE" },
    @{ From = "$root\vendor\ual-9.7.4\LICENSE.txt";     To = "$data\third-party\ultimate-asi-loader\LICENSE.txt" },
    @{ From = "$root\LICENSE";                          To = "$data\LICENSE.txt" },
    @{ From = "$root\README.md";                        To = "$data\README.md" }
)
foreach ($f in $files) {
    New-Item -ItemType Directory -Force (Split-Path $f.To) | Out-Null
    Copy-Item $f.From $f.To -Force
}
$zip = "$root\release\AotRAccess-$Version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Host "Release $zip"
Write-Host "SHA-256 $hash"
