<#
.SYNOPSIS
Compiles the HLSL raw-string literals of a header with fxc, so a broken shader fails here and not
in the game's log (the in-game compiler reports errors only there).

.DESCRIPTION
Takes every R"( ... )" literal in each file, writes it to a temporary .hlsl and compiles the
entry points named (default VS as vs_5_0 and PS as ps_5_0). A literal that has none of an entry
point's name is skipped for it. Shaders built from several literals (Pictures/*.cpp) are not
assembled here: the doorway daylight pass keeps its whole shader in one literal.

.EXAMPLE
tools\check-hlsl.ps1 -Files src\Skyrim\Render\DaylightHlsl.h
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory, Position = 0)][string[]]$Files,
	[string[]]$Entries = @('VS:vs_5_0', 'PS:ps_5_0')
)
$ErrorActionPreference = 'Stop'
$fxc = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\fxc.exe" | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $fxc) { throw 'fxc.exe not found (Windows SDK)' }
$failed = 0
foreach ($file in $Files) {
	$text = Get-Content -Raw -LiteralPath $file
	$literals = [regex]::Matches($text, '(?s)R"\((.*?)\)";')
	$index = 0
	foreach ($literal in $literals) {
		$index++
		$tmp = Join-Path ([IO.Path]::GetTempPath()) ("check-hlsl-$PID-$index.hlsl")
		Set-Content -LiteralPath $tmp -Value $literal.Groups[1].Value -Encoding ASCII
		foreach ($entry in $Entries) {
			$name, $profile = $entry.Split(':')
			if ($literal.Groups[1].Value -notmatch "\b$name\s*\(") { continue }
			$output = & $fxc /nologo /T $profile /E $name /Fo "$tmp.obj" $tmp 2>&1
			if ($LASTEXITCODE -ne 0) {
				$failed++
				Write-Host "FAILED $file literal $index $name ($profile)"
				$output | ForEach-Object { Write-Host "  $_" }
			} else {
				Write-Host "ok     $file literal $index $name ($profile)"
			}
		}
		Remove-Item -LiteralPath $tmp, "$tmp.obj" -ErrorAction SilentlyContinue
	}
}
if ($failed) { exit 1 }
