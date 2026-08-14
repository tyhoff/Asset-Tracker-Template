#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(survey_obs_test)

test_runner_generate(src/survey_obs_test.c)

set(ASSET_TRACKER_TEMPLATE_DIR ../../..)

# survey_obs.c has no zbus, shell or modem dependencies, so it is linked directly with
# no fakes at all. That is the point of keeping the cache and the formatter separate from
# the glue in survey.c and survey_shell.c.
target_sources(app
  PRIVATE
  src/survey_obs_test.c
  ${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/survey/survey_obs.c
)

zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/common)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/location)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/survey)

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(${ZEPHYR_BASE}/../nrfxlib/nrf_modem/include)

# The formatter renders latitude and longitude as doubles.
zephyr_compile_definitions(UNITY_INCLUDE_DOUBLE)

target_link_options(app PRIVATE --whole-archive)

# Options that cannot be passed through Kconfig fragments
target_compile_definitions(app PRIVATE
  -DCONFIG_APP_LOCATION_WIFI_APS_MAX=10
  -DCONFIG_APP_LOCATION_NEIGHBOR_CELLS_MAX=10
  -DCONFIG_LTE_NEIGHBOR_CELLS_MAX=10
  -DCONFIG_LOCATION=1
  -DCONFIG_LOCATION_DATA_DETAILS=1
  -DCONFIG_LOCATION_METHOD_GNSS=1
  -DCONFIG_LOCATION_METHOD_CELLULAR=1
  -DCONFIG_LOCATION_METHOD_WIFI=1
  -DCONFIG_LOCATION_METHODS_LIST_SIZE=3
)

set(SURVEY_DROP_LOCAL_MAC "y" CACHE STRING "Drop locally-administered BSSIDs: y or n")

if(SURVEY_DROP_LOCAL_MAC STREQUAL "y")
	target_compile_definitions(app PRIVATE -DCONFIG_APP_SURVEY_DROP_LOCAL_MAC=1)
elseif(NOT SURVEY_DROP_LOCAL_MAC STREQUAL "n")
	message(FATAL_ERROR "SURVEY_DROP_LOCAL_MAC must be y or n, got '${SURVEY_DROP_LOCAL_MAC}'")
endif()
