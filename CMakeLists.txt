# SPDX-FileCopyrightText: 2023 - 2026 UnionTech Software Technology Co., Ltd.
#
# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)

set (VERSION "1.0.0" CACHE STRING "define project version")

project(dde-services)

include(CTest)

if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX /usr)
endif()

include(GNUInstallDirs)
include(CTest)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
# 设置二进制输出目录 方便调试
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")

set(QT_VERSION_MAJOR 6)
set(DTK_VERSION_MAJOR 6)

add_subdirectory("src")

if(BUILD_TESTING)
    add_subdirectory("tests")
endif()
