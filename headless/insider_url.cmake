# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
# EDEN_INSIDER_URL as the update check takes it (update_check/insider.c, insider_base_valid), refused
# when configuring otherwise: empty (no insider builds), or https:// and a host of dot-separated labels
# of letters, digits and inner hyphens, then perhaps a path of segments of letters, digits, '.', '_'
# and '-' (neither "." nor ".."), no '/' at the end, no port, user or query, at most 128 characters.
# Included by headless/CMakeLists.txt; tools/check-update-check.py runs it alone
# (cmake -DEDEN_INSIDER_URL=<address> -P) and checks it refuses what the C code refuses.
if(NOT EDEN_INSIDER_URL STREQUAL "")
    set(eden_insider_url_valid FALSE)
    string(LENGTH "${EDEN_INSIDER_URL}" eden_insider_url_length)
    set(eden_insider_label "[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?")
    if(eden_insider_url_length LESS_EQUAL 128 AND
       EDEN_INSIDER_URL MATCHES "^https://${eden_insider_label}(\\.${eden_insider_label})*(/[A-Za-z0-9._-]+)*$" AND
       NOT EDEN_INSIDER_URL MATCHES "/\\.\\.?(/|$)")
        set(eden_insider_url_valid TRUE)
    endif()
    if(NOT eden_insider_url_valid)
        message(FATAL_ERROR "EDEN_INSIDER_URL must be the insider service's address, https:// and a host, perhaps "
            "with a path, without a '/' at the end (https://nightly-insider.example.workers.dev), not "
            "'${EDEN_INSIDER_URL}'")
    endif()
endif()
