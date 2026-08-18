# SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
#
# SPDX-License-Identifier: LGPL-3.0-or-later

# Top-level test directory for dde-services.
#
# Framework: Qt6::Test, consistent with the existing
# src/plugin-qt/shortcut/tests suite (Qt6::Test + add_test, gated by
# BUILD_TESTING). The project root enables BUILD_TESTING via include(CTest).
#
# This directory lives at the project root (not inside any plugin) on purpose:
# the thememanager / wallpaperslideshow / xsettings plugins build their sources
# with `file(GLOB_RECURSE)`, so a per-plugin tests/ tree would be pulled into
# the plugin MODULE and fail to link. Each test executable here compiles *only*
# the production source under test plus the test file, mirroring the shortcut
# tests' registration style, so no plugin MODULE is required to run the tests.

set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core DBus Gui Test)

set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# thememanager: SunriseSunset pure-math unit test
# ---------------------------------------------------------------------------
add_executable(tst-sunrisesunset
    tst_sunrisesunset.cpp
    ${SRC_DIR}/plugin-qt/thememanager/sunrisesunset.cpp
)
target_include_directories(tst-sunrisesunset PRIVATE
    ${SRC_DIR}/plugin-qt/thememanager
)
target_link_libraries(tst-sunrisesunset PRIVATE Qt6::Core Qt6::Test)
add_test(NAME thememanager-sunrisesunset COMMAND tst-sunrisesunset)

# ---------------------------------------------------------------------------
# wallpaperslideshow: utils helper unit test
# ---------------------------------------------------------------------------
add_executable(tst-wpssl-utils
    tst_wpssl_utils.cpp
    ${SRC_DIR}/plugin-qt/wallpaperslideshow/utils.cpp
)
target_include_directories(tst-wpssl-utils PRIVATE
    ${SRC_DIR}/plugin-qt/wallpaperslideshow
)
target_link_libraries(tst-wpssl-utils PRIVATE Qt6::Core Qt6::DBus Qt6::Test)
add_test(NAME wallpaperslideshow-utils COMMAND tst-wpssl-utils)
# writeWallpaperConfig writes to a path derived from XDG_CONFIG_HOME at
# static-init time; redirect it to a throwaway build-dir path so the test
# does not pollute the real user config dir. (The test slot itself QSKIPs
# when XDG_CONFIG_HOME is unset, e.g. when the binary is run directly.)
set_tests_properties(wallpaperslideshow-utils PROPERTIES
    ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/dde_svc_test_xdgconfig")

# ---------------------------------------------------------------------------
# wallpaperslideshow: FormatPicture MIME mapping unit test
# ---------------------------------------------------------------------------
add_executable(tst-format
    tst_format.cpp
    ${SRC_DIR}/plugin-qt/wallpaperslideshow/background/format.cpp
)
target_include_directories(tst-format PRIVATE
    ${SRC_DIR}/plugin-qt/wallpaperslideshow/background
)
target_link_libraries(tst-format PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
add_test(NAME wallpaperslideshow-format COMMAND tst-format)

# ---------------------------------------------------------------------------
# wallpaperslideshow: org.deepin.dde.WallpaperSlideshow D-Bus contract test
# ---------------------------------------------------------------------------
# Generate the production adaptor from the project introspection XML, wrapping
# the test-side FakeWallpaperSlideshowService, so the interface contract is
# validated against a real D-Bus connection.
qt_add_dbus_adaptor(WSS_ADAPTOR_SOURCES
    ${SRC_DIR}/plugin-qt/wallpaperslideshow/org.deepin.dde.WallpaperSlideshow.xml
    ${CMAKE_CURRENT_SOURCE_DIR}/fakeservice.h
    FakeWallpaperSlideshowService
    wallpaperslideshowadaptor
    WallpaperSlideshowAdaptor
)

add_executable(tst-wallpaperslideshow-dbus
    tst_wallpaperslideshow_dbus.cpp
    ${WSS_ADAPTOR_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/fakeservice.h
)
target_include_directories(tst-wallpaperslideshow-dbus PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(tst-wallpaperslideshow-dbus PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Test
)
add_test(NAME wallpaperslideshow-dbus COMMAND tst-wallpaperslideshow-dbus)

# ---------------------------------------------------------------------------
# xsettings: KeyFile ini/.desktop parser unit test
# ---------------------------------------------------------------------------
add_executable(tst-keyfile
    tst_keyfile.cpp
    ${SRC_DIR}/plugin-qt/xsettings/modules/api/keyfile.cpp
)
target_include_directories(tst-keyfile PRIVATE
    ${SRC_DIR}/plugin-qt/xsettings/modules/api
)
target_link_libraries(tst-keyfile PRIVATE Qt6::Core Qt6::Test)
add_test(NAME xsettings-keyfile COMMAND tst-keyfile)

# ---------------------------------------------------------------------------
# xsettings: Utils byte-manipulation helper unit test
# ---------------------------------------------------------------------------
add_executable(tst-xsutils
    tst_xsutils.cpp
    ${SRC_DIR}/plugin-qt/xsettings/modules/api/utils.cpp
)
target_include_directories(tst-xsutils PRIVATE
    ${SRC_DIR}/plugin-qt/xsettings/modules/api
    ${SRC_DIR}/plugin-qt/xsettings/modules
)
target_link_libraries(tst-xsutils PRIVATE Qt6::Core Qt6::Test)
add_test(NAME xsettings-xsutils COMMAND tst-xsutils)

# ---------------------------------------------------------------------------
# xsettings: XSItemInfo/XSDataInfo marshal/unmarshal unit test
# ---------------------------------------------------------------------------
# Pure byte-serialization layer (XSETTINGS wire format). Depends on the
# already-tested Utils byte helpers; no D-Bus/DConfig/XCB dependencies.
add_executable(tst-xsdatainfo
    tst_xsdatainfo.cpp
    ${SRC_DIR}/plugin-qt/xsettings/impl/xsdatainfo.cpp
    ${SRC_DIR}/plugin-qt/xsettings/modules/api/utils.cpp
)
target_include_directories(tst-xsdatainfo PRIVATE
    ${SRC_DIR}/plugin-qt/xsettings/impl
    ${SRC_DIR}/plugin-qt/xsettings
    ${SRC_DIR}/plugin-qt/xsettings/modules/api
    ${SRC_DIR}/plugin-qt/xsettings/modules
)
target_link_libraries(tst-xsdatainfo PRIVATE Qt6::Core Qt6::Test)
add_test(NAME xsettings-xsdatainfo COMMAND tst-xsdatainfo)
