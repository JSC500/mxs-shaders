# Bakes a shader pack into a C file, so the DLL works on its own.
param([string]$Pack, [string]$Out)
$ErrorActionPreference = 'Stop'

$files = @(Get-ChildItem -File $Pack | Sort-Object Name)
if ($files.Count -eq 0) { throw "no files in $Pack" }

$c = New-Object System.Text.StringBuilder
[void]$c.AppendLine('#include "common.h"')
$i = 0
foreach ($f in $files) {
	$bytes = [IO.File]::ReadAllBytes($f.FullName)
	[void]$c.Append("static const unsigned char file_$i[] = {")
	foreach ($b in $bytes) { [void]$c.Append("$b,") }
	[void]$c.AppendLine('0};')
	$i++
}
[void]$c.AppendLine('const embedded_file embedded_pack[] = {')
$i = 0
foreach ($f in $files) {
	[void]$c.AppendLine("`t{ `"$($f.Name)`", (const char *)file_$i },")
	$i++
}
[void]$c.AppendLine("`t{ 0, 0 }`n};")
Set-Content -Encoding ASCII -Path $Out -Value $c.ToString()
