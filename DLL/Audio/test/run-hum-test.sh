#!/bin/bash
# Offline test for the adaptive hum remover (DLL/Audio/HumRemover.hpp). No game, no hardware, ~10 s.
# Optional arguments: 16-bit PCM WAV takes to analyse as well (reported, not asserted).
set -e
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../.." && pwd)"
output_dir="$repo_root/build/Tests/HumRemover"
mkdir -p "$output_dir"
msvc="/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207"
sdk="/c/Program Files (x86)/Windows Kits/10"
sdkv="10.0.26100.0"
export INCLUDE="$(cygpath -w "$msvc/include");$(cygpath -w "$sdk/Include/$sdkv/um");$(cygpath -w "$sdk/Include/$sdkv/shared");$(cygpath -w "$sdk/Include/$sdkv/ucrt")"
export LIB="$(cygpath -w "$msvc/lib/x86");$(cygpath -w "$sdk/Lib/$sdkv/um/x86");$(cygpath -w "$sdk/Lib/$sdkv/ucrt/x86")"
cl="$msvc/bin/Hostx86/x86/cl.exe"
cd "$output_dir"
echo "== building hum remover test =="
MSYS_NO_PATHCONV=1 "$cl" -nologo -EHsc -std:c++17 -O2 -MT "$(cygpath -w "$script_dir/HumRemoverTest.cpp")" -Fe:HumRemoverTest.exe > build.log || { cat build.log; exit 1; }
echo "== running =="
args=()
for take in "$@"; do args+=("$(cygpath -w "$take")"); done
MSYS_NO_PATHCONV=1 "$output_dir/HumRemoverTest.exe" "${args[@]}"
