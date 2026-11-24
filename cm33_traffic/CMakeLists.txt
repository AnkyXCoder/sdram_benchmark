# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cm33_traffic)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ../include)
set_source_files_properties(src/main.c PROPERTIES COMPILE_OPTIONS -O2)
