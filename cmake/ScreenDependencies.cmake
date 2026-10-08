# Screen support is optional. Windows uses an immutable LGPL shared archive;
# other platforms may supply UCD_FFMPEG_ROOT or shared pkg-config packages.
option(UCD_WITH_SCREEN "Build screen sharing when its dependencies are available" ON)
set(UCD_FFMPEG_ROOT "" CACHE PATH "FFmpeg shared SDK root")
set(UCD_SCREEN_AVAILABLE OFF)
if (UCD_WITH_SCREEN)
    find_package(Qt6 6.5 QUIET COMPONENTS Multimedia)
    if (WIN32 AND NOT UCD_FFMPEG_ROOT)
        set(_ffmpeg_name ffmpeg-n7.1.5-12-g1fdbca85aa-win64-lgpl-shared-7.1)
        FetchContent_Declare(ucd_ffmpeg
            URL https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-07-31-14-10/${_ffmpeg_name}.zip
            URL_HASH SHA256=0f376f96fb38554ccefb1b2ae9c7c6a7b351f0e60a372b38262c320e8392c5d0)
        FetchContent_MakeAvailable(ucd_ffmpeg)
        set(UCD_FFMPEG_ROOT "${ucd_ffmpeg_SOURCE_DIR}")
    endif ()
    if (UCD_FFMPEG_ROOT)
        find_path(UCD_AV_INCLUDE libavcodec/avcodec.h HINTS "${UCD_FFMPEG_ROOT}/include" NO_DEFAULT_PATH)
        foreach (_lib avcodec avutil swscale)
            find_library(UCD_${_lib}_LIBRARY NAMES ${_lib} HINTS "${UCD_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
        endforeach ()
        if (UCD_AV_INCLUDE AND UCD_avcodec_LIBRARY AND UCD_avutil_LIBRARY AND UCD_swscale_LIBRARY)
            add_library(ucd_ffmpeg_shared INTERFACE)
            target_include_directories(ucd_ffmpeg_shared INTERFACE "${UCD_AV_INCLUDE}")
            target_link_libraries(ucd_ffmpeg_shared INTERFACE
                "${UCD_avcodec_LIBRARY}" "${UCD_avutil_LIBRARY}" "${UCD_swscale_LIBRARY}")
        endif ()
    else ()
        find_package(PkgConfig QUIET)
        if (PkgConfig_FOUND)
            pkg_check_modules(UCD_AV QUIET IMPORTED_TARGET libavcodec>=61 libavutil libswscale)
            if (TARGET PkgConfig::UCD_AV)
                add_library(ucd_ffmpeg_shared INTERFACE)
                target_link_libraries(ucd_ffmpeg_shared INTERFACE PkgConfig::UCD_AV)
            endif ()
        endif ()
    endif ()
    if (TARGET Qt6::Multimedia AND TARGET ucd_ffmpeg_shared)
        set(UCD_SCREEN_AVAILABLE ON)
    else ()
        message(WARNING "Screen sharing disabled: install Qt Multimedia and an LGPL shared FFmpeg 7 SDK")
    endif ()
endif ()

function(ucd_deploy_screen target)
    if (UCD_SCREEN_AVAILABLE AND WIN32)
        file(GLOB _ffmpeg_dlls "${UCD_FFMPEG_ROOT}/bin/*.dll")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ffmpeg_dlls} "$<TARGET_FILE_DIR:${target}>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/docs/third-party-screen.md" "$<TARGET_FILE_DIR:${target}>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/docs/OpenH264-BINARY-LICENSE.txt" "$<TARGET_FILE_DIR:${target}>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${UCD_FFMPEG_ROOT}/LICENSE.txt" "$<TARGET_FILE_DIR:${target}>/FFmpeg-LICENSE.txt"
            VERBATIM)
    endif ()
endfunction()
