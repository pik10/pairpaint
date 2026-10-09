# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.19)
project(PairPaint VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Concurrent)
qt_standard_project_setup()

# Everything except main() is a library, so the tests can link against it.
qt_add_library(pairpaint_core STATIC
    src/Adjustments.cpp src/Adjustments.h
    src/Canvas.cpp src/Canvas.h
    src/ColorWidgets.cpp src/ColorWidgets.h
    src/Dialogs.cpp src/Dialogs.h
    src/Document.cpp src/Document.h
    src/FileIO.cpp src/FileIO.h
    src/FilterDialog.cpp src/FilterDialog.h
    src/Filters.cpp src/Filters.h
    src/Icons.cpp src/Icons.h
    src/LayersPanel.cpp src/LayersPanel.h
    src/MainWindow.cpp src/MainWindow.h
    src/Psd.cpp src/Psd.h
    src/Tools.cpp src/Tools.h
    src/ToolSettings.h
)
target_include_directories(pairpaint_core PUBLIC src)
target_link_libraries(pairpaint_core PUBLIC Qt6::Widgets Qt6::Concurrent)

qt_add_executable(pairpaint src/main.cpp)
target_link_libraries(pairpaint PRIVATE pairpaint_core)
set_target_properties(pairpaint PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_BUNDLE_NAME PairPaint
    MACOSX_BUNDLE_GUI_IDENTIFIER org.pairpaint.PairPaint
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
    install(FILES packaging/pairpaint.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()
