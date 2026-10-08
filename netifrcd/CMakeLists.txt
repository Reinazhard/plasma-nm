cmake_minimum_required(VERSION 3.16)

# netifrcd - a NetworkManager-compatible D-Bus daemon backed by netifrc/OpenRC.
#
# It implements the org.freedesktop.NetworkManager D-Bus API on top of Gentoo-style
# netifrc (/etc/conf.d/net + /etc/init.d/net.*) so that unmodified NetworkManager
# clients - notably the KDE Plasma applet in this repository - keep working.
#
# This is a standalone project: it can be configured and built on its own with
#     cmake -S netifrcd -B build-netifrcd
# or pulled into the plasma-nm build with -DBUILD_NETIFRCD=ON, which is what CI uses so
# the unit tests below run alongside plasma-nm's own.

set(NETIFRCD_VERSION 0.1.0)

# Only take over the project when we *are* the top-level directory. Inside plasma-nm the
# enclosing project() already exists and must not be replaced - in particular a nested
# project() would reset PROJECT_VERSION, which is why the version above is a local
# variable rather than being read back off it.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(netifrcd
        VERSION ${NETIFRCD_VERSION}
        DESCRIPTION "NetworkManager-compatible D-Bus daemon backed by netifrc"
        LANGUAGES CXX
    )
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Standalone only: writing the cache from a subdirectory would override the parent's
# build type.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR
        AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core DBus Network)

# nm-dbus-interface.h is the authoritative, glib-free description of the
# NetworkManager D-Bus API. We use its enum values so the wire protocol can never
# drift from upstream.
find_path(NM_DBUS_INTERFACE_INCLUDE_DIR
    NAMES nm-dbus-interface.h
    PATHS /usr/include/libnm /usr/local/include/libnm
    DOC "Directory containing libnm's nm-dbus-interface.h"
)
if(NOT NM_DBUS_INTERFACE_INCLUDE_DIR)
    message(FATAL_ERROR "nm-dbus-interface.h not found. Install libnm (NetworkManager development headers).")
endif()

include(GNUInstallDirs)

add_executable(netifrcd
    src/main.cpp

    src/common/Logging.cpp
    src/common/Logging.h
    src/common/Constants.h

    src/netifrc/NetifrcConfig.cpp
    src/netifrc/NetifrcConfig.h
    src/netifrc/ServiceControl.cpp
    src/netifrc/ServiceControl.h
    src/netifrc/InterfaceWatcher.cpp
    src/netifrc/InterfaceWatcher.h

    src/wifi/WifiScanner.cpp
    src/wifi/WifiScanner.h

    src/model/Profile.cpp
    src/model/Profile.h
    src/model/ProfileStore.cpp
    src/model/ProfileStore.h
    src/model/Backend.cpp
    src/model/Backend.h

    src/dbus/ObjectBase.cpp
    src/dbus/ObjectBase.h
    src/dbus/ManagerObject.cpp
    src/dbus/ManagerObject.h
    src/dbus/ObjectManagerObject.cpp
    src/dbus/ObjectManagerObject.h
    src/dbus/DeviceObject.cpp
    src/dbus/DeviceObject.h
    src/dbus/AccessPointObject.cpp
    src/dbus/AccessPointObject.h
    src/dbus/ActiveConnectionObject.cpp
    src/dbus/ActiveConnectionObject.h
    src/dbus/SettingsObject.cpp
    src/dbus/SettingsObject.h
    src/dbus/ConfigObjects.cpp
    src/dbus/ConfigObjects.h
    src/dbus/AgentManagerObject.cpp
    src/dbus/AgentManagerObject.h
)

target_include_directories(netifrcd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${NM_DBUS_INTERFACE_INCLUDE_DIR}
)

target_link_libraries(netifrcd PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Network
)

target_compile_definitions(netifrcd PRIVATE
    NETIFRCD_VERSION="${NETIFRCD_VERSION}"
    NETIFRCD_NM_VERSION="1.52.0"
)

install(TARGETS netifrcd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
# NetifrcConfig is a pure file writer (it takes its path in the constructor), so its
# invariants are pinned with a real unit test rather than only through the end-to-end
# shell scripts in tests/, which need root and a radio. Run with `ctest` in the build dir.
#
# These are registered as ordinary CTest tests, so CI picks them up for free: plasma-nm's
# .kde-ci.yml sets `require-passing-tests-on: ['@all']` and turns BUILD_NETIFRCD on, and
# the sysadmin/ci-utilities templates run ctest.
option(BUILD_TESTING "Build the netifrcd unit tests" ON)

if(BUILD_TESTING)
    find_package(Qt6 6.5 QUIET COMPONENTS Test)
    if(Qt6Test_FOUND)
        enable_testing()

        add_executable(netifrcconfig_test
            tests/netifrcconfig_test.cpp
            src/netifrc/NetifrcConfig.cpp
            src/common/Logging.cpp
        )
        target_include_directories(netifrcconfig_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(netifrcconfig_test PRIVATE Qt6::Core Qt6::Test)

        add_test(NAME netifrcconfig COMMAND netifrcconfig_test)
    else()
        message(STATUS "Qt6::Test not found - skipping the netifrcd unit tests")
    endif()
endif()

# D-Bus system bus policy: allow root (and the netifrcd service user) to own the
# NetworkManager bus name.
install(FILES data/org.freedesktop.NetworkManager.netifrcd.conf
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d
)

# OpenRC service + D-Bus activation.
install(FILES data/netifrcd.initd
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/init.d
    RENAME netifrcd
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)
install(FILES data/org.freedesktop.NetworkManager.service
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system-services
)
