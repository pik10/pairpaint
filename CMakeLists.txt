# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.19)
project(PairPaint VERSION 0.7.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Concurrent)
qt_standard_project_setup()

# Developer builds that catch memory errors and undefined behavior at run time:
#   cmake -B build-asan -DPAIRPAINT_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
option(PAIRPAINT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(PAIRPAINT_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# Everything except main() is a library, so the tests can link against it.
qt_add_library(pairpaint_core STATIC
    src/Adjustments.cpp src/Adjustments.h
    src/Autosave.cpp src/Autosave.h
    src/Blending.cpp src/Blending.h
    src/Canvas.cpp src/Canvas.h
    src/ColorWidgets.cpp src/ColorWidgets.h
    src/Dialogs.cpp src/Dialogs.h
    src/Document.cpp src/Document.h
    src/FileIO.cpp src/FileIO.h
    src/FilterDialog.cpp src/FilterDialog.h
    src/Filters.cpp src/Filters.h
    src/Heif.cpp src/Heif.h
    src/Icons.cpp src/Icons.h
    src/LayersPanel.cpp src/LayersPanel.h
    src/MainWindow.cpp src/MainWindow.h
    src/Psd.cpp src/Psd.h
    src/TextBox.cpp src/TextBox.h
    src/Tools.cpp src/Tools.h
    src/ToolSettings.h
)
target_include_directories(pairpaint_core PUBLIC src)
target_link_libraries(pairpaint_core PUBLIC Qt6::Widgets Qt6::Concurrent)

# HEIC/HEIF photos: on Windows through the system's codec (WIC), on macOS through Qt's own
# plugin, elsewhere with libheif.
if(WIN32)
    target_link_libraries(pairpaint_core PRIVATE windowscodecs ole32)
elseif(NOT APPLE)
    option(PAIRPAINT_HEIF "Open HEIC/HEIF photos (needs libheif)" ON)
    if(PAIRPAINT_HEIF)
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBHEIF IMPORTED_TARGET libheif)
        endif()
        if(LIBHEIF_FOUND)
            message(STATUS "HEIC/HEIF support: libheif ${LIBHEIF_VERSION}")
            target_link_libraries(pairpaint_core PRIVATE PkgConfig::LIBHEIF)
            target_compile_definitions(pairpaint_core PRIVATE PAIRPAINT_HAVE_LIBHEIF)
        else()
            message(WARNING "libheif not found, so HEIC/HEIF photos won't open. Install it (e.g. libheif-dev) "
                            "or configure with -DPAIRPAINT_HEIF=OFF.")
        endif()
    endif()
endif()

qt_add_executable(pairpaint src/main.cpp)
if(WIN32)
    target_sources(pairpaint PRIVATE packaging/windows/pairpaint.rc)
elseif(APPLE)
    set(icns ${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/PairPaint.icns)
    set_source_files_properties(${icns} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(pairpaint PRIVATE ${icns})
    # On macOS the app is PairPaint.app; elsewhere the program is called pairpaint.
    set_target_properties(pairpaint PROPERTIES OUTPUT_NAME PairPaint MACOSX_BUNDLE_ICON_FILE PairPaint.icns)
endif()
target_link_libraries(pairpaint PRIVATE pairpaint_core)
target_compile_definitions(pairpaint PRIVATE PAIRPAINT_VERSION="${PROJECT_VERSION}")
set(PAIRPAINT_APP_ID io.github.pik10.PairPaint)
set_target_properties(pairpaint PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_BUNDLE_NAME PairPaint
    MACOSX_BUNDLE_GUI_IDENTIFIER ${PAIRPAINT_APP_ID}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
)

foreach(target pairpaint_core pairpaint)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()

option(PAIRPAINT_BUILD_TESTS "Build the test suite" ON)
if(PAIRPAINT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS pairpaint
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES packaging/${PAIRPAINT_APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/${PAIRPAINT_APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    install(FILES packaging/${PAIRPAINT_APP_ID}.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    foreach(size 32 48 64 128 256 512)
        install(FILES packaging/icons/${size}x${size}/${PAIRPAINT_APP_ID}.png
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps)
    endforeach()
endif()
