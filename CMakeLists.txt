cmake_minimum_required(VERSION 3.16)
project(vxe-monitor VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(vxe-monitor WIN32 src/main.cpp)
target_link_libraries(vxe-monitor setupapi hid user32 gdi32 shell32 advapi32 gdiplus)

add_executable(probe tools/probe.cpp)
target_link_libraries(probe setupapi hid user32)

if(MSVC)
    target_compile_definitions(vxe-monitor PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_definitions(probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
endif()

if(MINGW)
    # 入口是宽字符版 wWinMain/wmain,MinGW 链接器需要 -municode
    # 才链接 wWinMainCRTStartup/wmainCRTStartup;否则报 undefined reference to `WinMain'
    target_link_options(vxe-monitor PRIVATE -mwindows -municode)
    target_link_options(probe PRIVATE -municode)
endif()
