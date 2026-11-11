cmake_minimum_required(VERSION 3.15)
project(aino_player CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(aino_player WIN32 src/main.cpp src/login.cpp)
target_include_directories(aino_player PRIVATE vendor/webview2/include)

set(SYS_LIBS user32 gdi32 winhttp winmm shell32 ole32 oleaut32 windowscodecs advapi32 mfplat mf mfuuid)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # /MT 静态运行库
  target_compile_options(aino_player PRIVATE /utf-8 /EHsc /O2)
  target_link_libraries(aino_player PRIVATE ${SYS_LIBS}
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/webview2/lib/WebView2LoaderStatic.lib)
  # WIN32 默认入口是 WinMain，这里保留 main()
  target_link_options(aino_player PRIVATE /ENTRY:mainCRTStartup)
else()
  # MinGW 下 WebView2 由 login.cpp 自己定位并加载运行时，不链 MSVC 的静态 loader
  target_compile_options(aino_player PRIVATE -O2 -mcrtdll=ucrt)
  target_link_libraries(aino_player PRIVATE ${SYS_LIBS} uuid)
  target_link_options(aino_player PRIVATE -static -mwindows -mcrtdll=ucrt)
endif()


