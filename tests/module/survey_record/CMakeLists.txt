#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(survey_record_test)

test_runner_generate(src/survey_record_test.c)

set(ASSET_TRACKER_TEMPLATE_DIR ../../..)
# Absolute: execute_process() below does not resolve paths relative to this file.
get_filename_component(SURVEY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../app/src/modules/survey
		       ABSOLUTE)

target_sources(app
  PRIVATE
  src/survey_record_test.c
  ${SURVEY_DIR}/survey_record.c
)

zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/common)
zephyr_include_directories(${ASSET_TRACKER_TEMPLATE_DIR}/app/src/modules/location)
zephyr_include_directories(${SURVEY_DIR})

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(${ZEPHYR_BASE}/../nrfxlib/nrf_modem/include)

# Generate the same encoder and decoder the application uses. The decoder is what makes
# the round trip possible, and the round trip is the only way to prove that an omitted
# field is absent on the wire rather than merely zero in the struct.
set(survey_zcbor_command
	zcbor code
	--cddl ${SURVEY_DIR}/survey_record.cddl
	--encode
	--decode
	--short-names
	--default-max-qty 10
	-t survey-record survey-session
	--output-cmake survey_record.cmake
)
execute_process(COMMAND ${survey_zcbor_command}
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		COMMAND_ERROR_IS_FATAL ANY)
include(${CMAKE_CURRENT_BINARY_DIR}/survey_record.cmake)
zephyr_link_libraries(survey_record)
target_link_libraries(survey_record PRIVATE zephyr_interface)

target_link_options(app PRIVATE --whole-archive)

# The encoder scales latitude and longitude as doubles.
zephyr_compile_definitions(UNITY_INCLUDE_DOUBLE)

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
