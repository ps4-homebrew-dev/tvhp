cmake_minimum_required(VERSION 3.16)

project(tvhp)

set(CMAKE_CXX_STANDARD 17)

find_package(SDL2 REQUIRED)
find_package(RmlUi REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(FREETYPE2 REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
pkg_check_modules(SDL2_IMAGE REQUIRED IMPORTED_TARGET SDL2_image)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)

add_executable(tvhp
    src/main.cpp
    src/app.cpp
    src/app_connect.cpp
    src/app_guide.cpp
    src/app_recordings.cpp
    src/app_watch.cpp
    src/config.cpp
    src/format.cpp
    src/gamepad.cpp
    src/htsp.cpp
    src/iconcache.cpp
    src/net/http_client.cpp
    src/player.cpp
    src/tsmux.cpp
    src/RmlUi/RmlUi_Backend_SDL_GL3.cpp
    src/RmlUi/RmlUi_Renderer_GL3.cpp
    src/RmlUi/RmlUi_Platform_SDL.cpp
)

target_compile_options(tvhp PRIVATE -Wall -Werror)
# Antialiasing of the UI's edges (rounded corners, curves): the renderer's
# default of 2 samples leaves them visibly stepped. Clamped to what the
# driver supports (RmlUi_Renderer_GL3.cpp). As in jtplay.
target_compile_definitions(tvhp PRIVATE RMLUI_NUM_MSAA_SAMPLES=4)
target_include_directories(tvhp PRIVATE src)

add_library(hts STATIC
  libhts/htsmsg.c
  libhts/htsmsg_binary.c
  libhts/htsbuf.c
  libhts/sha1.c
  libhts/net_posix.c
)
target_include_directories(hts PUBLIC libhts)


target_link_libraries(tvhp PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
    RmlUi::Core
    PkgConfig::FREETYPE2
    PkgConfig::MPV
    PkgConfig::FFMPEG
    PkgConfig::SDL2_IMAGE
    PkgConfig::CURL
    Threads::Threads
    ${CMAKE_DL_LIBS}
    hts
)

if(ORBIS)
    pkg_check_modules(EGL REQUIRED egl)
    pkg_check_modules(GL REQUIRED gl)
    pkg_check_modules(VULKAN REQUIRED vulkan)

    # A non-game application ("gdc"), as jtplay is: a PS5 running it as a
    # PS4 app treats it as a media app, and only that gets the remote's media
    # keys there. A non-game has no /temp0 (temporary data, games only), so
    # it asks for download data instead, mounted at /download0, where
    # SDL2main (pacbrew 0.8.7 and later) puts the XDG base directories: the
    # saved servers and settings, the channel icons, Mesa's shader cache.
    # Its size is in MiB. The content ID is the one earlier versions got by
    # default, so a new package installs over an old one.
    # The certificates https needs (channel logos on web servers), as jtplay
    # ships them: libcurl is told where with SSL_CERT_FILE (src/main.cpp).
    file(COPY ${CMAKE_SYSROOT}/usr/etc/ca-bundle.crt DESTINATION ${CMAKE_CURRENT_SOURCE_DIR})

    orbis_add_pkg(tvhp
        TITLE tvhp
        TITLE_ID BREW04328
        CONTENT_ID IV0000-BREW04328_00-TVHP000000000000
        CATEGORY gdc
        DOWNLOAD_DATA_SIZE 128
        ASSETS sce_sys assets fonts ca-bundle.crt
        MODULES libEGL.so.1 libGL.so.1 libgallium-${EGL_VERSION}.so libvulkan.so.1
    )
endif()
