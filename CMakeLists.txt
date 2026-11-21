cmake_minimum_required(VERSION 3.21)

project(memonitor VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "MemMonitor requires Linux /proc and /sys interfaces.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Concurrent DBus)

qt_add_executable(memonitor
    src/main.cpp
    src/memory_sampler.cpp
    src/memory_sampler.h
    src/monitor_policy.h
    src/main_window.cpp
    src/main_window.h
    src/window_chrome.cpp
    src/window_chrome.h
    src/application_icon_resolver.cpp
    src/application_icon_resolver.h
    src/memory_pie_chart.cpp
    src/memory_pie_chart.h
    src/window_tracker.cpp
    src/window_tracker.h
)

target_compile_options(memonitor PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
)

target_compile_definitions(memonitor PRIVATE MEMONITOR_VERSION="${PROJECT_VERSION}")

target_link_libraries(memonitor PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::DBus
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    qt_add_executable(memonitor_tests
        tests/memory_sampler_test.cpp
        src/memory_sampler.cpp
        src/memory_sampler.h
        src/monitor_policy.h
    )
    target_compile_options(memonitor_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
    target_include_directories(memonitor_tests PRIVATE src)
    target_link_libraries(memonitor_tests PRIVATE Qt6::Test)
    add_dependencies(memonitor_tests memonitor)
    add_test(NAME memonitor.memory_sampler COMMAND memonitor_tests)
    add_test(NAME memonitor.cli_dump COMMAND memonitor --dump)
    set_tests_properties(memonitor.cli_dump PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=memonitor-invalid-platform"
        TIMEOUT 30
    )

    qt_add_executable(memonitor_gui_tests
        tests/gui_smoke_test.cpp
        src/main_window.cpp
        src/main_window.h
        src/window_chrome.cpp
        src/window_chrome.h
        src/application_icon_resolver.cpp
        src/application_icon_resolver.h
        src/memory_pie_chart.cpp
        src/memory_pie_chart.h
        src/window_tracker.cpp
        src/window_tracker.h
        src/memory_sampler.cpp
        src/memory_sampler.h
        src/monitor_policy.h
    )
    target_compile_options(memonitor_gui_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
    target_include_directories(memonitor_gui_tests PRIVATE src)
    target_link_libraries(memonitor_gui_tests PRIVATE
        Qt6::Widgets
        Qt6::Concurrent
        Qt6::DBus
        Qt6::Test
    )
    add_test(NAME memonitor.gui_smoke COMMAND memonitor_gui_tests)
    set_tests_properties(memonitor.gui_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session)
    if(DBUS_RUN_SESSION_EXECUTABLE)
        add_test(NAME memonitor.notifications
            COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- $<TARGET_FILE:memonitor_gui_tests>
                warningNotificationsUsePlainTextAndRearm
        )
        set_tests_properties(memonitor.notifications PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MEMONITOR_TEST_NOTIFICATION_BUS=1"
        )
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS memonitor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/memonitor.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGE_NAME "memonitor")
    set(CPACK_PACKAGE_VENDOR "MemMonitor")
    set(CPACK_PACKAGE_CONTACT "MemMonitor contributors")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Qt memory monitor for Linux")
    set(CPACK_PACKAGE_DESCRIPTION "Monitor process PSS, swap, memory pressure and zram usage with a native Qt desktop interface.")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE
    )
    set(CPACK_STRIP_FILES ON)
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins")
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-wayland")
    set(CPACK_DEBIAN_COMPRESSION_TYPE "xz")
    include(CPack)
endif()
