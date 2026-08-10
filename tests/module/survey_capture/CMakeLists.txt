#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(survey_capture_test)

test_runner_generate(src/survey_capture_test.c)

set(ATT_DIR ../../..)
get_filename_component(SURVEY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../app/src/modules/survey
		       ABSOLUTE)

# Only the unit under test. The observation cache, the record encoder and the storage
# module are all stubbed in the test file, because what is being tested is the order and
# separation of the location requests -- and a real encoder or filesystem would add
# failure modes that have nothing to do with sequencing.
target_sources(app
  PRIVATE
  src/survey_capture_test.c
  ${SURVEY_DIR}/survey_capture.c
)

zephyr_include_directories(${ATT_DIR}/app/src/common)
zephyr_include_directories(${ATT_DIR}/app/src/modules/location)
zephyr_include_directories(${ATT_DIR}/app/src/modules/network)
zephyr_include_directories(${SURVEY_DIR})

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(${ZEPHYR_BASE}/../nrfxlib/nrf_modem/include)

target_link_options(app PRIVATE --whole-archive)

# Options that cannot be passed through Kconfig fragments, because unit tests do not
# source the application's Kconfig.
target_compile_definitions(app PRIVATE
  -DCONFIG_APP_SURVEY_CAPTURE=1
  # Short compared with the application's defaults: the fake answers in milliseconds, so
  # these only ever fire if the orchestrator has stopped waiting correctly -- in which
  # case a test should fail in seconds rather than in minutes.
  -DCONFIG_APP_SURVEY_CAPTURE_GNSS_TIMEOUT_SECONDS=5
  -DCONFIG_APP_SURVEY_CAPTURE_SCAN_TIMEOUT_SECONDS=5
  -DCONFIG_APP_SURVEY_CAPTURE_IDLE_TIMEOUT_SECONDS=5
  -DCONFIG_APP_SURVEY_CAPTURE_THREAD_STACK_SIZE=4096
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
