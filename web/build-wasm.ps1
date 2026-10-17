# Build the DSP core to a standalone WebAssembly module (no Emscripten JS runtime needed).
# Usage: powershell -File web\build-wasm.ps1 [-Emsdk C:\path\to\emsdk]
param([string]$Emsdk = "C:\Users\produ\tools\emsdk")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$web = $PSScriptRoot

$exports = @(
  "_ab_init", "_ab_out_ptr", "_ab_out_capacity", "_ab_process", "_ab_process_single",
  "_ab_set_mix", "_ab_set_trim_db", "_ab_note_on", "_ab_note_off", "_ab_all_notes_off", "_ab_panic",
  "_ab_set_param", "_ab_get_param", "_ab_set_profile", "_ab_set_constraint",
  "_ab_active_voices", "_ab_latency", "_ab_max_voices",
  "_ab_param_count", "_ab_param_name", "_ab_param_min", "_ab_param_max", "_ab_param_default",
  "_ab_param_index", "_ab_constraint_count"
) -join ","

$args_ = @(
  "-O3", "-std=c++17", "-fno-exceptions", "-fno-rtti", "-ffast-math", "-msimd128",
  "-I", "$root\dsp\include",
  "$root\dsp\src\c_api.cpp",
  "--no-entry",
  "-sSTANDALONE_WASM=1",
  "-sEXPORTED_FUNCTIONS=[$exports]",
  "-sINITIAL_MEMORY=33554432",
  "-sALLOW_MEMORY_GROWTH=0",
  "-sSTACK_SIZE=1048576",
  "-o", "$web\absynth.wasm"
)

Write-Host "em++ $($args_ -join ' ')"
cmd /c "`"$Emsdk\emsdk_env.bat`" >nul 2>&1 && em++ $($args_ -join ' ')"
if ($LASTEXITCODE -ne 0) { throw "em++ failed with exit code $LASTEXITCODE" }

# ship the patches next to the page
New-Item -ItemType Directory -Force "$web\patches" | Out-Null
Copy-Item "$root\patches\*.json" "$web\patches\" -Force

$size = (Get-Item "$web\absynth.wasm").Length
Write-Host "absynth.wasm: $size bytes"
