#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/survey_obs.c
	${CMAKE_CURRENT_SOURCE_DIR}/survey_record.c
	${CMAKE_CURRENT_SOURCE_DIR}/survey.c
)

target_sources_ifdef(CONFIG_APP_SURVEY_SHELL app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/survey_shell.c
)

target_sources_ifdef(CONFIG_APP_SURVEY_STORAGE app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/survey_store.c
	${CMAKE_CURRENT_SOURCE_DIR}/survey_session.c
)

target_sources_ifdef(CONFIG_APP_SURVEY_CAPTURE app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/survey_capture.c
)

target_sources_ifdef(CONFIG_APP_SURVEY_EXPORT app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/survey_export.c
)

target_include_directories(app PRIVATE .)

# Generate the record encoder and decoder from the CDDL schema. Same pattern as
# app/src/cbor, but kept local to the module so the survey record and the device shadow
# stay independent.
#
# The decoder is generated as well as the encoder even though the firmware only encodes:
# the round-trip unit test decodes what it encoded, which is what proves that an omitted
# field really is absent on the wire rather than merely zero in the struct.
set(survey_zcbor_command
	zcbor code
	--cddl ${CMAKE_CURRENT_SOURCE_DIR}/survey_record.cddl
	--encode
	--decode
	--short-names
	# Must be at least the largest of the AP and neighbour caps, otherwise the
	# generated arrays silently hold fewer entries than the application collects.
	--default-max-qty 10
	-t survey-record survey-session
	--output-cmake survey_record.cmake
)
execute_process(COMMAND ${survey_zcbor_command}
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		COMMAND_ERROR_IS_FATAL ANY)

include(${CMAKE_CURRENT_BINARY_DIR}/survey_record.cmake)

# Re-run CMake when the schema changes, so the generated code cannot go stale.
set_property(
	DIRECTORY
	APPEND
	PROPERTY
	CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/survey_record.cddl
)

zephyr_link_libraries(survey_record)
target_link_libraries(survey_record PRIVATE zephyr_interface)
