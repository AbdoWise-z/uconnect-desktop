# uconnect-vcam.dll: the uConnect Camera media source for Windows 11's virtual
# camera API, loaded by Frame Server. Built with MSVC, apart from the app
# (which may use MinGW): it uses WRL and the Windows SDK's Media Foundation
# headers.
#
#   cmake -S vcam/windows -B build-vcam -G "Visual Studio 17 2022" -A x64
#   cmake --build build-vcam --config Release
#   ctest --test-dir build-vcam -C Release
cmake_minimum_required(VERSION 3.21)
project(uconnect_vcam LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "uconnect-vcam needs MSVC (WRL, Media Foundation headers)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The static runtime: Frame Server should not depend on a VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options(/W4 /permissive-)

add_library(uconnect-vcam SHARED
    dllmain.cpp
    Activator.cpp
    MediaSource.cpp
    MediaStream.cpp
    FrameReader.cpp
    uconnect-vcam.def)
target_include_directories(uconnect-vcam PRIVATE ../include)
target_link_libraries(uconnect-vcam PRIVATE mfplat mfuuid ole32 shell32 advapi32)

enable_testing()
add_executable(vcam-source-test test/source_test.cpp)
target_include_directories(vcam-source-test PRIVATE ../include)
target_link_libraries(vcam-source-test PRIVATE mfplat mfreadwrite mfuuid ole32)
add_test(NAME vcam-source COMMAND vcam-source-test $<TARGET_FILE:uconnect-vcam>)
