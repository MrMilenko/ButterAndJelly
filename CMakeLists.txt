# WiiU Butter and Jelly - a Jellyfin client for the Wii U.
#
# One tree, two targets:
#   host    cmake -B build -G Ninja
#   console cmake -B build-wiiu -G Ninja \
#             -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/WiiU.cmake
#
# The console toolchain sets NINTENDO_WIIU, which is how everything below
# decides which platform layer to compile.

cmake_minimum_required(VERSION 3.16)
project(butterandjelly VERSION 0.1.2 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------- third party

add_library(cjson STATIC third_party/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cJSON)

# --------------------------------------------------------------------- core

set(CORE_SOURCES
    src/core/thread.cpp
    src/core/log.cpp
    src/core/json.cpp
    src/core/ts_demux.cpp
    src/core/video_frame.cpp
    src/core/hls.cpp
    src/core/settings.cpp
    src/core/audio_decoder_minimp3.cpp
    src/core/audio_decoder_mpg123.cpp
    src/core/audio_output.cpp
    src/core/player.cpp
    src/core/jellyfin.cpp
    src/core/discovery.cpp
    src/platform/http_curl.cpp
)

if(NINTENDO_WIIU)
    list(APPEND CORE_SOURCES
        src/platform/wiiu/platform_wiiu.cpp
        src/platform/wiiu/video_decoder_h264dec.cpp
        src/platform/wiiu/gx2_video.cpp)
else()
    list(APPEND CORE_SOURCES
        src/platform/desktop/platform_desktop.cpp
        src/platform/desktop/video_decoder_ffmpeg.cpp)
endif()

add_library(bjcore STATIC ${CORE_SOURCES})
target_include_directories(bjcore PUBLIC src)
target_link_libraries(bjcore PUBLIC cjson)

# MP3 decoding, the same library on both targets.
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPG123 REQUIRED libmpg123)
target_include_directories(bjcore PUBLIC ${MPG123_INCLUDE_DIRS})
target_link_libraries(bjcore PUBLIC ${MPG123_LINK_LIBRARIES})

if(NINTENDO_WIIU)
    # devkitPro ships plain .pc files for these; pkg-config is the supported
    # way to pick up the right link order for curl and its TLS backend.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CURL REQUIRED libcurl)
    target_include_directories(bjcore PUBLIC ${CURL_INCLUDE_DIRS})
    target_link_libraries(bjcore PUBLIC ${CURL_LINK_LIBRARIES} wut)
    target_compile_definitions(bjcore PUBLIC __WIIU__)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(bjcore PUBLIC CURL::libcurl)

    # Host-only: stands in for the console's hardware decoder so the video
    # pipeline can be exercised without a Wii U attached.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil)
    target_include_directories(bjcore PUBLIC ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(bjcore PUBLIC ${FFMPEG_LINK_LIBRARIES})
endif()

# SDL provides the audio device on both targets, so the core needs its
# headers. Linking happens with the executables.
pkg_check_modules(SDL2_FOR_CORE REQUIRED sdl2)
target_include_directories(bjcore PUBLIC ${SDL2_FOR_CORE_INCLUDE_DIRS})
target_link_libraries(bjcore PUBLIC ${SDL2_FOR_CORE_LINK_LIBRARIES})

find_package(Threads REQUIRED)
target_link_libraries(bjcore PUBLIC Threads::Threads)

# ------------------------------------------------------------------ jfcli

# Host-only: a headless driver for the same core the console runs, so API
# problems surface on a laptop instead of on a TV.
if(NOT NINTENDO_WIIU)
    add_executable(jfcli tools/jfcli.cpp)
    target_link_libraries(jfcli PRIVATE bjcore)
endif()

# --------------------------------------------------------------- SDL2 app

# pkg-config is the one discovery mechanism both sides agree on: Homebrew
# ships .pc files, and so does every devkitPro portlib.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2       REQUIRED sdl2)
pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)
pkg_check_modules(SDL2_TTF   REQUIRED SDL2_ttf)

set(APP_SOURCES
    src/main.cpp
    src/core/worker.cpp
    src/ui/app.cpp
    src/ui/art_cache.cpp
    src/ui/render.cpp
    src/ui/image_load_sdl.cpp
    src/ui/image_load_stb.cpp
    src/ui/text_font_stb.cpp
    src/ui/text_font_ttf.cpp
)

add_executable(butterandjelly ${APP_SOURCES})
target_link_libraries(butterandjelly PRIVATE bjcore)
target_include_directories(butterandjelly PRIVATE
    ${SDL2_INCLUDE_DIRS} ${SDL2_IMAGE_INCLUDE_DIRS} ${SDL2_TTF_INCLUDE_DIRS})
target_compile_options(butterandjelly PRIVATE ${SDL2_CFLAGS_OTHER})

if(NINTENDO_WIIU)
    # Static portlibs: the link order below is what wut expects.
    target_link_libraries(butterandjelly PRIVATE
        ${SDL2_TTF_LINK_LIBRARIES}
        ${SDL2_IMAGE_LINK_LIBRARIES}
        ${SDL2_LINK_LIBRARIES})

    wut_create_rpx(butterandjelly)

    # Bundle the fonts and CA list alongside the executable so the .wuhb is
    # self-contained and nothing has to be copied to the SD card by hand.
    wut_create_wuhb(butterandjelly
        NAME       "WiiU Butter and Jelly"
        SHORTNAME  "WiiU Butter and Jelly"
        AUTHOR     "MrMilenko"
        CONTENT    "${CMAKE_SOURCE_DIR}/assets"
    )
else()
    target_link_libraries(butterandjelly PRIVATE
        ${SDL2_LINK_LIBRARIES}
        ${SDL2_IMAGE_LINK_LIBRARIES}
        ${SDL2_TTF_LINK_LIBRARIES})
endif()
