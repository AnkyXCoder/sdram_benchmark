# SPDX-License-Identifier: Apache-2.0
#
# Concurrent build: adds the CM33 traffic generator image and enables the
# concurrent and CM33 tests in the CM85 image. Plain (non-sysbuild) builds
# do not use this file.

# BOARD is the board name and BOARD_QUALIFIERS "/<soc>/cm85" in this scope
string(REGEX REPLACE "^/?([^/]+).*$" "\\1" sb_soc "${BOARD_QUALIFIERS}")

ExternalZephyrProject_Add(
  APPLICATION cm33_traffic
  SOURCE_DIR ${APP_DIR}/cm33_traffic
  BOARD ${BOARD}/${sb_soc}/cm33
)

set_config_bool(${DEFAULT_IMAGE} CONFIG_SDRAM_BENCH_CONCURRENT y)
set_config_bool(${DEFAULT_IMAGE} CONFIG_SDRAM_BENCH_CM33 y)

# Flash the CM33 image first; the CM85 flash resets the board afterwards
sysbuild_add_dependencies(FLASH ${DEFAULT_IMAGE} cm33_traffic)
