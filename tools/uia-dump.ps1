# Read-only dump of a window's UI Automation tree (what a screen reader can see).
# Usage: powershell -File uia-dump.ps1 -ProcessName AotR_Launcher -Out tree.txt [-MaxDepth 25]
param(
    [Parameter(Mandatory)] [string]$ProcessName,
    [Parameter(Mandatory)] [string]$Out,
    [int]$MaxDepth = 25,
    [int]$MaxNodes = 3000
)
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$proc = Get-Process -Name $ProcessName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw "No window for $ProcessName" }
$root = [System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)
$walker = [System.Windows.Automation.TreeWalker]::RawViewWalker
$lines = New-Object System.Collections.Generic.List[string]
$script:count = 0

function Describe($e) {
    $c = $e.Current
    $type = $c.ControlType.ProgrammaticName -replace '^ControlType\.', ''
    $r = $c.BoundingRectangle
    $rect = if ($r.IsEmpty) { 'no-rect' } else { '{0},{1} {2}x{3}' -f [int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height }
    $patterns = ($e.GetSupportedPatterns() | ForEach-Object { $_.ProgrammaticName -replace 'PatternIdentifiers\.Pattern$', '' }) -join ','
    $value = ''
    try {
        $vp = $e.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
        if ($vp) { $value = ' value="' + $vp.Current.Value + '"' }
    } catch {}
    $toggle = ''
    try {
        $tp = $e.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
        if ($tp) { $toggle = ' toggle=' + $tp.Current.ToggleState }
    } catch {}
    $sel = ''
    try {
        $sp = $e.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
        if ($sp) { $sel = ' selected=' + $sp.Current.IsSelected }
    } catch {}
    '{0} "{1}" id="{2}" class="{3}" enabled={4} offscreen={5} focusable={6} [{7}] {{{8}}}{9}{10}{11}' -f `
        $type, $c.Name, $c.AutomationId, $c.ClassName, $c.IsEnabled, $c.IsOffscreen, $c.IsKeyboardFocusable, $rect, $patterns, $value, $toggle, $sel
}

function Walk($e, $depth) {
    if ($script:count -ge $MaxNodes -or $depth -gt $MaxDepth) { return }
    $script:count++
    try { $lines.Add(('  ' * $depth) + (Describe $e)) } catch { $lines.Add(('  ' * $depth) + '<unreadable>') }
    $child = $walker.GetFirstChild($e)
    while ($child) {
        Walk $child ($depth + 1)
        $child = $walker.GetNextSibling($child)
    }
}

Walk $root 0
$lines | Out-File -FilePath $Out -Encoding utf8
"nodes: $script:count -> $Out"
