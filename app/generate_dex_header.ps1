# 从 classes.dex 生成 overlay_dex.h
param(
    [string]$DexPath,
    [string]$OutputPath
)

if (-not (Test-Path $DexPath)) {
    Write-Error "Dex file not found: $DexPath"
    exit 1
}

$bytes = [System.IO.File]::ReadAllBytes($DexPath)
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("// Auto-generated from classes.dex - DO NOT EDIT")
[void]$sb.AppendLine("#include <cstddef>")
[void]$sb.Append("extern const unsigned char overlay_dex[] = {")
for ($i = 0; $i -lt $bytes.Length; $i++) {
    if ($i % 16 -eq 0) { [void]$sb.Append("`n    ") }
    [void]$sb.AppendFormat("0x{0:X2}", $bytes[$i])
    if ($i -lt $bytes.Length - 1) { [void]$sb.Append(",") }
}
[void]$sb.AppendLine("`n};")
[void]$sb.AppendLine("extern const unsigned int overlay_dex_len = $($bytes.Length);")
[System.IO.File]::WriteAllText($OutputPath, $sb.ToString())
Write-Host "Generated $OutputPath from $DexPath ($($bytes.Length) bytes)"
