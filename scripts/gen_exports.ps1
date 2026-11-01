# Generates the export list and jump stubs from the system opengl32.dll.
# Names in src\static_hooks.txt export our hook_* function, the rest jump through real_table.
param([string]$OutDir, [string]$Hooks)
$ErrorActionPreference = 'Stop'

$dump = & dumpbin /nologo /exports "$env:WINDIR\System32\opengl32.dll"
$names = @($dump | ForEach-Object {
	if ($_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\w+)') { $Matches[1] }
})
if ($names.Count -lt 300) { throw "only found $($names.Count) exports" }

$hooked = @(Get-Content $Hooks | Where-Object { $_.Trim() })
foreach ($h in $hooked) {
	if ($names -notcontains $h) { throw "$h is not an opengl32 export" }
}

$def = @('LIBRARY opengl32', 'EXPORTS')
$asm = @('EXTERN real_table:QWORD', '.CODE')
$table = @('void *real_table[1];')
$cnames = New-Object System.Collections.ArrayList
$i = 0
foreach ($n in $names) {
	if ($hooked -contains $n) {
		$def += "    $n=hook_$n"
		continue
	}
	$def += "    $n=stub_$n"
	$asm += "stub_$n PROC"
	$asm += "    jmp QWORD PTR [real_table + $($i * 8)]"
	$asm += "stub_$n ENDP"
	[void]$cnames.Add("`t`"$n`",")
	$i++
}
$asm += 'END'

$c = @("void *real_table[$i];", 'const char *const real_names[] = {') + $cnames + @('};', "const int real_count = $i;")

Set-Content -Encoding ASCII -Path (Join-Path $OutDir 'exports.def') -Value $def
Set-Content -Encoding ASCII -Path (Join-Path $OutDir 'stubs.asm') -Value $asm
Set-Content -Encoding ASCII -Path (Join-Path $OutDir 'real_table.c') -Value $c
Write-Host "generated $($names.Count) exports, $($hooked.Count) hooked"
