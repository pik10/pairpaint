# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(pairpaint_ui_test ui_test.cpp)
target_link_libraries(pairpaint_ui_test PRIVATE pairpaint_core Qt6::Test)
target_compile_definitions(pairpaint_ui_test PRIVATE PAIRPAINT_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")

add_test(NAME ui_test COMMAND pairpaint_ui_test)
# Runs without a display; the test drives the real UI offscreen.
set_tests_properties(ui_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 300)

# File-reader fuzzer (see the comment at the top of fuzz_files.cpp).
qt_add_executable(pairpaint_fuzz fuzz_files.cpp)
target_link_libraries(pairpaint_fuzz PRIVATE pairpaint_core)
