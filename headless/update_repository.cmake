# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
# EDEN_UPDATE_REPOSITORY as the update check takes it (update_check/github_release.c,
# github_release_repository_valid), refused when configuring otherwise: empty (the catalog), or
# owner/name, the owner of letters, digits and hyphens, not first, at most 39 of them, the name of
# letters, digits, '.', '_' and '-', at most 100, and neither "." nor "..". Included by
# headless/CMakeLists.txt; tools/check-update-check.py runs it alone
# (cmake -DEDEN_UPDATE_REPOSITORY=<name> -P) and checks it refuses what the C code refuses.
if(NOT EDEN_UPDATE_REPOSITORY STREQUAL "")
    set(eden_update_repository_valid FALSE)
    if(EDEN_UPDATE_REPOSITORY MATCHES "^([A-Za-z0-9][A-Za-z0-9-]*)/([A-Za-z0-9._-]+)$")
        set(eden_update_owner "${CMAKE_MATCH_1}")
        set(eden_update_name "${CMAKE_MATCH_2}")
        string(LENGTH "${eden_update_owner}" eden_update_owner_length)
        string(LENGTH "${eden_update_name}" eden_update_name_length)
        if(eden_update_owner_length LESS_EQUAL 39 AND eden_update_name_length LESS_EQUAL 100 AND
           NOT eden_update_name STREQUAL "." AND NOT eden_update_name STREQUAL "..")
            set(eden_update_repository_valid TRUE)
        endif()
    endif()
    if(NOT eden_update_repository_valid)
        message(FATAL_ERROR "EDEN_UPDATE_REPOSITORY must be a GitHub repository as owner/name (an owner of "
            "at most 39 characters, a name of at most 100), not '${EDEN_UPDATE_REPOSITORY}'")
    endif()
endif()
