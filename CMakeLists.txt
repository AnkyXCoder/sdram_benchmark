# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sdram_benchmark)

target_include_directories(app PRIVATE include)

# Firmware identification printed in the banner
execute_process(
  COMMAND git describe --always --dirty --abbrev=12
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE sb_git_rev
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT sb_git_rev)
  set(sb_git_rev "unknown")
endif()
target_compile_definitions(app PRIVATE SB_GIT_REV="${sb_git_rev}")
target_sources(app PRIVATE
  src/main.c
  src/kernels.c
  src/integrity.c
  src/bench_cpu.c
  src/bench_access.c
  src/bench_stream.c
  src/retention.c
  src/bench_sustained.c
)
if(CONFIG_SDRAM_BENCH_DMA OR CONFIG_SDRAM_BENCH_CONCURRENT)
  target_sources(app PRIVATE src/bench_dma.c)
endif()
target_sources_ifdef(CONFIG_SDRAM_BENCH_CONCURRENT app PRIVATE src/bench_concurrent.c)

# Benchmark kernels at -O2; the rest of the image keeps the default -Os
get_target_property(app_sources app SOURCES)
set_source_files_properties(${app_sources} PROPERTIES COMPILE_OPTIONS -O2)
