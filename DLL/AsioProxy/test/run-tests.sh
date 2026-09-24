#!/bin/bash
# Automated test for the Rocksmith Audio Bridge proxy ASIO driver. Builds the proxy, a stub "real"
# ASIO driver and a test host, then drives the proxy with no game/admin/hardware and asserts the
# output tap captured exactly what the host wrote. Exit 0 = pass.
set -e
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../.." && pwd)"
output_dir="$repo_root/build/Tests/AsioProxy"
mkdir -p "$output_dir"
msvc="/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207"
sdk="/c/Program Files (x86)/Windows Kits/10"
sdkv="10.0.26100.0"
export INCLUDE="$(cygpath -w "$msvc/include");$(cygpath -w "$sdk/Include/$sdkv/um");$(cygpath -w "$sdk/Include/$sdkv/shared");$(cygpath -w "$sdk/Include/$sdkv/ucrt");$(cygpath -w "$sdk/Include/$sdkv/winrt")"
export LIB="$(cygpath -w "$msvc/lib/x86");$(cygpath -w "$sdk/Lib/$sdkv/um/x86");$(cygpath -w "$sdk/Lib/$sdkv/ucrt/x86")"
cl="$msvc/bin/Hostx86/x86/cl.exe"
here="$script_dir"
cd "$output_dir"

echo "== building proxy (current source) =="
MSYS_NO_PATHCONV=1 "$cl" -nologo -LD -EHsc -std:c++17 -O2 -MT -DRSMODS_ASIO_PROXY_TESTING "$(cygpath -w "$here/../AsioProxyDriver.cpp")" "$(cygpath -w "$here/../VirtualCableCapture.cpp")" -Fe:RocksmithAudioBridgeAsio.dll -link -DEF:"$(cygpath -w "$here/../RocksmithAudioBridge.def")" ole32.lib advapi32.lib propsys.lib

echo "== building stub ASIO driver =="
MSYS_NO_PATHCONV=1 "$cl" -nologo -LD -EHsc -std:c++17 -O2 -MT "$(cygpath -w "$here/StubAsioDriver.cpp")" -Fe:StubAsioDriver.dll \
  -link -DEF:"$(cygpath -w "$here/StubAsioDriver.def")" ole32.lib uuid.lib

echo "== building test host =="
MSYS_NO_PATHCONV=1 "$cl" -nologo -EHsc -std:c++17 -O2 -MT "$(cygpath -w "$here/ProxyTestHost.cpp")" -Fe:ProxyTestHost.exe \
  -link ole32.lib uuid.lib advapi32.lib

echo "== building latency DSP test =="
MSYS_NO_PATHCONV=1 "$cl" -nologo -EHsc -std:c++17 -O2 -MT "$(cygpath -w "$here/LatencyDspTest.cpp")" -Fe:LatencyDspTest.exe

echo "== building limiter DSP test =="
MSYS_NO_PATHCONV=1 "$cl" -nologo -EHsc -std:c++17 -O2 -MT "$(cygpath -w "$here/LimiterDspTest.cpp")" -Fe:LimiterDspTest.exe

echo "== building meter DSP test =="
MSYS_NO_PATHCONV=1 "$cl" -nologo -EHsc -std:c++17 -O2 -MT "$(cygpath -w "$here/MeterDspTest.cpp")" -Fe:MeterDspTest.exe

echo "== building output guard DSP test =="
MSYS_NO_PATHCONV=1 "$cl" -nologo -EHsc -std:c++17 -O2 -MT "$(cygpath -w "$here/OutputGuardDspTest.cpp")" -Fe:OutputGuardDspTest.exe

echo "== running proxy harness =="
MSYS_NO_PATHCONV=1 "$output_dir/ProxyTestHost.exe" "$(cygpath -w "$output_dir/RocksmithAudioBridgeAsio.dll")" "$(cygpath -w "$output_dir/StubAsioDriver.dll")"
code=$?
[ $code -ne 0 ] && { echo "== proxy harness exit $code =="; exit $code; }

echo "== running latency DSP test =="
MSYS_NO_PATHCONV=1 "$output_dir/LatencyDspTest.exe"
code=$?
[ $code -ne 0 ] && { echo "== latency DSP exit $code =="; exit $code; }

echo "== running limiter DSP test =="
MSYS_NO_PATHCONV=1 "$output_dir/LimiterDspTest.exe"
code=$?
[ $code -ne 0 ] && { echo "== limiter DSP exit $code =="; exit $code; }

echo "== running meter DSP test =="
MSYS_NO_PATHCONV=1 "$output_dir/MeterDspTest.exe"
code=$?
[ $code -ne 0 ] && { echo "== meter DSP exit $code =="; exit $code; }

echo "== running output guard DSP test =="
MSYS_NO_PATHCONV=1 "$output_dir/OutputGuardDspTest.exe"
code=$?
echo "== exit $code =="
exit $code
