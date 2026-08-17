#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(survey_store_test)

# Which full-behaviour to compile in. The choice lives in Kconfig.storage in the real
# build, but unit tests do not source the application's Kconfig, so it arrives as a CMake
# argument instead -- see testcase.yaml, which builds this suite once per value.
set(SURVEY_STORE_FULL "STOP" CACHE STRING "Full-behaviour under test: STOP or OVERWRITE")

if(SURVEY_STORE_FULL STREQUAL "STOP")
	set(full_behaviour_def -DCONFIG_APP_STORAGE_FULL_STOP=1)
elseif(SURVEY_STORE_FULL STREQUAL "OVERWRITE")
	set(full_behaviour_def -DCONFIG_APP_STORAGE_FULL_OVERWRITE=1)
else()
	message(FATAL_ERROR "SURVEY_STORE_FULL must be STOP or OVERWRITE, got '${SURVEY_STORE_FULL}'")
endif()

test_runner_generate(src/survey_store_test.c)

set(ATT_DIR ../../..)
# Absolute: execute_process() below does not resolve paths relative to this file.
get_filename_component(SURVEY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../app/src/modules/survey
		       ABSOLUTE)

target_sources(app
  PRIVATE
  src/survey_store_test.c
  ${SURVEY_DIR}/survey_record.c
  ${SURVEY_DIR}/survey_store.c
  ${ATT_DIR}/app/src/modules/storage/storage.c
  ${ATT_DIR}/app/src/modules/storage/storage_data_types.c
  ${ATT_DIR}/app/src/modules/storage/backends/littlefs_backend.c
)

zephyr_include_directories(${ATT_DIR}/app/src/common)
zephyr_include_directories(${ATT_DIR}/app/src/modules/location)
zephyr_include_directories(${ATT_DIR}/app/src/modules/network)
zephyr_include_directories(${ATT_DIR}/app/src/modules/storage)
zephyr_include_directories(${ATT_DIR}/app/src/modules/storage/backends)
zephyr_include_directories(${SURVEY_DIR})

zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr/)
zephyr_include_directories(${ZEPHYR_BASE}/subsys/testsuite/include)
zephyr_include_directories(${ZEPHYR_BASE}/../nrfxlib/nrf_modem/include)

zephyr_linker_sources(SECTIONS ${ATT_DIR}/app/src/modules/storage/storage_sections.ld)

# Same encoder and decoder the application builds. The decoder is what proves a record
# read back off flash is still a record, rather than merely equal to a buffer this test
# produced with the same code that wrote it.
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
  ${full_behaviour_def}
  -DCONFIG_LOG=1
  -DCONFIG_APP_STORAGE_LOG_LEVEL=3
  -DCONFIG_APP_SURVEY_STORAGE=1
  # Only the survey type is registered, so 2 is already headroom.
  -DCONFIG_APP_STORAGE_MAX_TYPES=2
  # Small enough that a test can fill the ring twice in a second, which is the only way
  # the capacity behaviour gets exercised at all.
  -DCONFIG_APP_STORAGE_MAX_RECORDS_PER_TYPE=20
  # As late as it can be set: update_threshold() rejects anything above
  # MAX_RECORDS_PER_TYPE. The capacity tests do reach it and do publish
  # STORAGE_THRESHOLD_REACHED; nothing in this suite observes storage_chan, so it goes
  # nowhere. In the application it goes to the state machine -- see the note on the same
  # symbol in overlay-survey.conf.
  -DCONFIG_APP_STORAGE_INITIAL_THRESHOLD=20
  -DCONFIG_APP_STORAGE_LITTLEFS_MAX_PATH_LEN=64
  -DCONFIG_APP_STORAGE_LITTLEFS_TARGET_FILE_SIZE=0
  -DCONFIG_APP_STORAGE_THREAD_STACK_SIZE=6144
  -DCONFIG_APP_STORAGE_WATCHDOG_TIMEOUT_SECONDS=120
  -DCONFIG_APP_STORAGE_MSG_PROCESSING_TIMEOUT_SECONDS=60
  -DCONFIG_APP_STORAGE_SESSION_TIMEOUT_SECONDS=60
  # One 816-byte slot plus its pipe header; storage.c BUILD_ASSERTs the exact figure.
  -DCONFIG_APP_STORAGE_BATCH_BUFFER_SIZE=1024
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
