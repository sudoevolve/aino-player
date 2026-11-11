#!/usr/bin/env bash
# build.sh —— 直接用 cl.exe 构建（不依赖 vcvars，编译所需路径在这里显式给出）
# 用法：./build.sh           构建播放器 build/aino_player.exe
#       ./build.sh probe     构建源层探针 build/probe_sources.exe
set -e

VC="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231"
SDK="C:/Program Files (x86)/Windows Kits/10"
SDKV="10.0.26100.0"

ROOT="$(cd "$(dirname "$0")" && pwd)"
export INCLUDE="$VC/include;$SDK/Include/$SDKV/ucrt;$SDK/Include/$SDKV/um;$SDK/Include/$SDKV/shared;$SDK/Include/$SDKV/winrt;$ROOT/vendor/webview2/include"
export LIB="$VC/lib/x64;$SDK/Lib/$SDKV/ucrt/x64;$SDK/Lib/$SDKV/um/x64"
CL="$VC/bin/Hostx64/x64/cl.exe"

# MSYS 会把 /xxx 形式的参数当成路径改写，这里关掉
export MSYS2_ARG_CONV_EXCL="*"
export MSYS_NO_PATHCONV=1

cd "$(dirname "$0")"
mkdir -p build

if [ "$1" = "probe" ]; then
  "$CL" -nologo -std:c++17 -utf-8 -EHsc -O2 -MT /Fo:build/ -Fe:build/probe_sources.exe \
    tools/probe_sources.cpp user32.lib winhttp.lib shell32.lib
elif [ "$1" = "lyrics" ]; then
  "$CL" -nologo -std:c++17 -utf-8 -EHsc -O2 -MT /Fo:build/ -Fe:build/probe_lyrics.exe \
    tools/probe_lyrics.cpp user32.lib winhttp.lib shell32.lib
elif [ "$1" = "audio" ]; then
  "$CL" -nologo -std:c++17 -utf-8 -EHsc -O2 -MT /Fo:build/ -Fe:build/probe_audio.exe \
    tools/probe_audio.cpp user32.lib winmm.lib shell32.lib
elif [ "$1" = "tag" ]; then
  "$CL" -nologo -std:c++17 -utf-8 -EHsc -O2 -MT /Fo:build/ -Fe:build/probe_tag.exe \
    tools/probe_tag.cpp user32.lib winhttp.lib shell32.lib
elif [ "$1" = "probes" ]; then
  "$CL" -nologo -std:c++17 -utf-8 -EHsc -O2 -MT /Fo:build/ -Fe:build/probe_sources.exe \
    tools/probe_sources.cpp user32.lib winhttp.lib shell32.lib
  "$CL" -nologo -std:c++17 -utf-8 -EHsc -O2 -MT /Fo:build/ -Fe:build/probe_lyrics.exe \
    tools/probe_lyrics.cpp user32.lib winhttp.lib shell32.lib
else
  # /SUBSYSTEM:WINDOWS + mainCRTStartup：双击启动不弹黑色控制台窗口
  # （默认 console 子系统会挂一个 cmd 宿主窗，看起来像"程序带了个终端"）
  "$CL" -nologo -std:c++17 -utf-8 -EHsc -O2 -MT /Fo:build/ -Fe:build/aino_player.exe \
    src/main.cpp src/login.cpp user32.lib gdi32.lib winhttp.lib winmm.lib shell32.lib ole32.lib windowscodecs.lib advapi32.lib \
    mfplat.lib mf.lib mfuuid.lib oleaut32.lib \
    vendor/webview2/lib/WebView2LoaderStatic.lib \
    /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
fi
