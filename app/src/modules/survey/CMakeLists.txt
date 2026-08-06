#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/survey_obs.c
	${CMAKE_CURRENT_SOURCE_DIR}/survey.c
)

target_sources_ifdef(CONFIG_APP_SURVEY_SHELL app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/survey_shell.c
)

target_include_directories(app PRIVATE .)
