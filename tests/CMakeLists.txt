# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(pairpaint_ui_test ui_test.cpp)
target_link_libraries(pairpaint_ui_test PRIVATE pairpaint_core Qt6::Test)

add_test(NAME ui_test COMMAND pairpaint_ui_test)
# Runs without a display; the test drives the real UI offscreen.
set_tests_properties(ui_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 300)
