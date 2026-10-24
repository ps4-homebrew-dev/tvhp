cmake_minimum_required(VERSION 3.16)

project(tvhp)

set(CMAKE_CXX_STANDARD 17)

find_package(SDL2 REQUIRED)
find_package(RmlUi REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(FREETYPE2 REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
# libavformat's MPEG-TS muxer puts live TV back into a container for mpv.
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
pkg_check_modules(SDL2_IMAGE REQUIRED IMPORTED_TARGET SDL2_image)

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
    src/player.cpp
    src/tsmux.cpp
    src/RmlUi/RmlUi_Backend_SDL_GL3.cpp
    src/RmlUi/RmlUi_Renderer_GL3.cpp
    src/RmlUi/RmlUi_Platform_SDL.cpp
)

target_compile_options(tvhp PRIVATE -Wall -Werror)

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
    Threads::Threads
    ${CMAKE_DL_LIBS}
    hts
    SceIpmi
    SceAppContent
)

if(ORBIS)
    set(TITLE "TVHP")
    set(TITLE_ID "BREW04328")
    set(CONTENT_ID "IV0000-${TITLE_ID}_00-TVHP000000000000")
    set(VERSION "1.00")

    # The package is staged in pkg/, laid out exactly as it is installed:
    # pkg/ is /app0 on the console, where src/main.cpp looks for the assets
    # and fonts. tvhp.gp4 is generated there from the same list, so whatever
    # is staged is packaged.
    set(PKG_DIR ${CMAKE_CURRENT_BINARY_DIR}/pkg)
    set(SYSROOT_PREFIX ${CMAKE_SYSROOT}/${CMAKE_INSTALL_PREFIX})

    pkg_check_modules(EGL REQUIRED egl)
    set(MESA_MODULES
        libEGL.so.1
        libGL.so.1
        libgallium-${EGL_VERSION}.so
        libvulkan.so.1
    )

    set(PKG_FILES eboot.bin sce_sys/param.sfo sce_sys/icon0.png)
    set(PKG_OUTPUTS)

    add_custom_command(
        OUTPUT ${PKG_DIR}/eboot.bin
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PKG_DIR}
        COMMAND ${ELF2SELF} $<TARGET_FILE:tvhp> ${PKG_DIR}/eboot.bin
        DEPENDS tvhp
    )

    add_custom_command(
        OUTPUT ${PKG_DIR}/sce_sys/param.sfo
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PKG_DIR}/sce_sys
        COMMAND ${MKSFO} -i APP_TYPE=1 -s128 TITLE=${TITLE}
                         -s12 TITLE_ID=${TITLE_ID}
                         -s48 CONTENT_ID=${CONTENT_ID}
                         -s8 APP_VER=${VERSION}
                         -s8 VERSION=${VERSION}
                         -i ATTRIBUTE=0
                         -i DOWNLOAD_DATA_SIZE=0
                         -i SYSTEM_VER=0
                         -s4 CATEGORY=gdc
                         ${PKG_DIR}/sce_sys/param.sfo
        VERBATIM
    )

    # The PS4 wants a 512x512 icon; sce_sys/icon0.png is scaled to fit.
    add_custom_command(
        OUTPUT ${PKG_DIR}/sce_sys/icon0.png
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PKG_DIR}/sce_sys
        COMMAND convert ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png
                -resize 512x512 -background black -gravity center
                -extent 512x512 ${PKG_DIR}/sce_sys/icon0.png
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png
        VERBATIM
    )

    # Backgrounds, already 1920x1080: pic0 behind the app's start screen,
    # pic1 behind the home screen while the tile is focused.
    foreach(pic pic0.png pic1.png)
        add_custom_command(
            OUTPUT ${PKG_DIR}/sce_sys/${pic}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${PKG_DIR}/sce_sys
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/${pic}
                    ${PKG_DIR}/sce_sys/${pic}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/${pic}
        )
        list(APPEND PKG_FILES sce_sys/${pic})
    endforeach()

    # Mesa: libGL.so.1 is what RmlUi's GL loader opens.
    foreach(module ${MESA_MODULES})
        add_custom_command(
            OUTPUT ${PKG_DIR}/sce_module/${module}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${PKG_DIR}/sce_module
            COMMAND ${ELF2SELF} ${SYSROOT_PREFIX}/lib/${module}
                                ${PKG_DIR}/sce_module/${module}
            DEPENDS ${SYSROOT_PREFIX}/lib/${module}
        )
        list(APPEND PKG_FILES sce_module/${module})
    endforeach()

    # Every asset and font, as in the source tree (editor backups left out).
    file(GLOB_RECURSE DATA_FILES CONFIGURE_DEPENDS
         RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
         ${CMAKE_CURRENT_SOURCE_DIR}/assets/*
         ${CMAKE_CURRENT_SOURCE_DIR}/fonts/*)
    list(FILTER DATA_FILES EXCLUDE REGEX "~$")
    list(SORT DATA_FILES)
    foreach(file ${DATA_FILES})
        add_custom_command(
            OUTPUT ${PKG_DIR}/${file}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${CMAKE_CURRENT_SOURCE_DIR}/${file} ${PKG_DIR}/${file}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${file}
        )
        list(APPEND PKG_FILES ${file})
    endforeach()

    foreach(file ${PKG_FILES})
        list(APPEND PKG_OUTPUTS ${PKG_DIR}/${file})
    endforeach()

    add_custom_command(
        OUTPUT ${PKG_DIR}/tvhp.gp4
        COMMAND ${MKGP4} -c ${CONTENT_ID} -o tvhp.gp4 ${PKG_FILES}
        WORKING_DIRECTORY ${PKG_DIR}
        DEPENDS ${PKG_OUTPUTS}
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${CONTENT_ID}.pkg
        COMMAND ${MKPKG} -o ${CMAKE_CURRENT_BINARY_DIR}/${CONTENT_ID}.pkg tvhp.gp4
        WORKING_DIRECTORY ${PKG_DIR}
        DEPENDS ${PKG_DIR}/tvhp.gp4 ${PKG_OUTPUTS}
        VERBATIM
    )

    add_custom_target(pkg ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${CONTENT_ID}.pkg)
endif()
