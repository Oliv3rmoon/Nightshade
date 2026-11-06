/*
 * ProsperoEden - Insider builds: new test builds for members of the nightly Discord, early.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A build that names the insider service (EDEN_INSIDER_URL, headless/CMakeLists.txt) asks it for a
 * test build before it asks GitHub, when Test builds is on (Settings > Diagnostics) and an insider
 * code is kept. A member gets the code on the service's page, signed in with Discord: 8 characters
 * of INSIDER_ALPHABET. The service's contract is nightly-insider's docs/API.md:
 *
 *   POST <base>/v1/updates   {"app":"nightshade","installed_version":"01.010.012","code":"ABCDEFGH"}
 *
 * Only HTTP 200 with "status": "update" and a build that passes every check below is an offer: a
 * newer version of the installed one's line, a SHA-256, a size the kit takes and a download address
 * on the service's own host (<base>/v1/download/<token>, short-lived and signed). That offer fills
 * the same self_update_offer GitHub's check fills (a pre-release, with the build's title), so the
 * download, its SHA-256 and the helper are the kit's, unchanged. Anything else (none, not_qualified,
 * invalid_code, expired_code, another status or HTTP code, no network, a timeout, an answer it can't
 * read or a build it refuses) is no offer, and the update check goes on exactly as without insider
 * builds: insider_check_tests_from asks GitHub's test builds then.
 *
 * This gates the delivery of updates only: nothing here decides whether the app runs, and offline,
 * without a code or without the service the app behaves as it always did. Only the code, the app's
 * id and its version are sent.
 *
 * C11; also compiles as C++. The decisions build without a network (UPDATE_CHECK_NO_NETWORK) and are
 * tested on the host (headless/update_check_check.cpp); the POST is the console's
 * (self_update_ps5.c).
 */
#ifndef PROSPEROEDEN_INSIDER_H
#define PROSPEROEDEN_INSIDER_H

#include "github_release.h"
#include "self_update.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The code's characters: no 0 O 1 I L U. */
#define INSIDER_ALPHABET "23456789ABCDEFGHJKMNPQRSTVWXYZ"
#define INSIDER_CODE_LENGTH 8
/* The service's address as a build names it: https://, a host, perhaps a path. */
#define INSIDER_MAX_BASE 128u
/* What the service takes (1 KB) and what an answer may be (a build's notes are at most 4,000
 * characters). */
#define INSIDER_MAX_REQUEST 1024u
#define INSIDER_MAX_ANSWER (16u * 1024u)
/* How long the request may take: to connect, and in all. */
#define INSIDER_CONNECT_MS 5000
#define INSIDER_TIMEOUT_MS 10000

    typedef enum insider_status
    {
        INSIDER_UPDATE = 0,         /* a newer insider build, offered */
        INSIDER_NONE = 1,           /* "none": the code is good, nothing newer */
        INSIDER_NOT_QUALIFIED = 2,  /* "not_qualified": the member no longer holds the roles */
        INSIDER_INVALID_CODE = 3,   /* "invalid_code": no such code (mistyped, regenerated, revoked) */
        INSIDER_EXPIRED_CODE = 4,   /* "expired_code" */
        INSIDER_OTHER = 5,          /* another status or HTTP code (bad_request, rate_limited...) */
        INSIDER_NETWORK = 6,        /* no answer: see platform_error */
        INSIDER_TOO_LARGE = 7,      /* an answer over INSIDER_MAX_ANSWER */
        INSIDER_BAD_RESPONSE = 8,   /* not the JSON the API documents (cut short, no status...) */
        INSIDER_REFUSED = 9,        /* "update", with a build that did not pass the checks: see why */
        INSIDER_BAD_ARGUMENT = 10,  /* a service, code, app or version that can't be sent */
        INSIDER_NOT_ASKED = 11      /* no service named or no code kept: nothing was sent */
    } insider_status;

    /* What the request found, for the log: never the code, never the download's token. */
    typedef struct insider_report
    {
        insider_status status;
        int http_status;    /* 0 when no answer arrived */
        int platform_error; /* the failed request's code, or 0 */
        char answer[32];    /* the answer's "status" when it is a word of [a-z0-9_-]; else empty */
        const char *why;    /* INSIDER_REFUSED: which check the build failed ("sha256", "url"...) */
        char tag[40];       /* the build's tag, once read */
        char host[128];     /* the download's host, once its address passed */
        char file[128];     /* the build's file name ("Nightshade-v1.010.013.zip"), when it is a plain name */
    } insider_report;

    /* POSTs `json` (Content-Type: application/json) to `url` with User-Agent `agent`, the answer
     * into body (at most capacity bytes). As self_update_platform's fetch: 0, an answer arrived
     * (*status set); negative, failed; SELF_UPDATE_FETCH_TOO_LARGE, an answer longer than capacity.
     * HTTPS only, no redirects, INSIDER_CONNECT_MS and INSIDER_TIMEOUT_MS. */
    typedef int (*insider_post_fn)(void *user, const char *url, const char *agent, const char *json, char *body,
                                   size_t capacity, size_t *length, int *status);

    /* A code as the player typed it, normalised as the service does: upper case, everything that
     * isn't one of INSIDER_ALPHABET's characters dropped ("abcd-efgh" and "ABCD EFGH" are
     * "ABCDEFGH"). 1 when exactly INSIDER_CODE_LENGTH characters remain: then they are in code.
     * 0 otherwise, and code is empty. */
    int insider_code_normalise(const char *typed, char code[INSIDER_CODE_LENGTH + 1]);

    /* 1 when `code` is INSIDER_CODE_LENGTH characters of INSIDER_ALPHABET and nothing else. */
    int insider_code_valid(const char *code);

    /* The kept code (the config folder's insider-code: the code and a line end, nothing else).
     * 1: read into code. 0: anything else, and code is empty. */
    int insider_code_read(const char *text, size_t length, char code[INSIDER_CODE_LENGTH + 1]);

    /* 1 when `base` is the service's address as a build may name it (headless/insider_url.cmake
     * takes and refuses the same): https://, a host of dot-separated labels of letters, digits and
     * inner hyphens, then perhaps a path of segments of letters, digits, '.', '_' and '-' (neither
     * "." nor ".."), no '/' at the end, at most INSIDER_MAX_BASE characters. No port, user or query. */
    int insider_base_valid(const char *base);

    /* 1 when `app` is an app's id as the service takes it: [a-z][a-z0-9-]{1,31}. */
    int insider_app_valid(const char *app);

    /* A version as the service writes one ("1.010.013", "v1.010.013", "01.010.013": three numbers,
     * perhaps a 'v') as a content version ("01.010.013"). 1 when it is one that fits NN.NNN.NNN. */
    int insider_content_version(const char *text, char version[12]);

    /* <base>/v1/updates. Its length, or 0 (out empty) when base isn't valid or out too small. */
    size_t insider_updates_url(char *out, size_t size, const char *base);

    /* The request's body, {"app":...,"installed_version":...,"code":...}, each a JSON string
     * (quotes, backslashes and control characters escaped). Its length, or 0 (out empty) when an
     * argument is missing, out is too small or it would pass INSIDER_MAX_REQUEST. */
    size_t insider_request(char *out, size_t size, const char *app, const char *version, const char *code);

    /* Reads the service's answer to POST /v1/updates. INSIDER_UPDATE only for HTTP 200, "status":
     * "update" and a build that passes every check: its version is newer than `installed` and on
     * its line (github_release_same_line), and as much for `running` when given (the program's own
     * version); its tag is a plain name; its sha256 64 lowercase hexadecimal digits; its size above
     * 0 and at most SELF_UPDATE_MAX_ARCHIVE; its url exactly <base>/v1/download/<token>
     * (self_update_insider_url). Then offer is filled as self_update_check fills one (title_id,
     * name, installed, available, version = tag, artifact = url, sha256, size, notes), with
     * prerelease 1 and release_title the build's title. Otherwise the offer is empty. `report`
     * (optional) says what was found. */
    insider_status insider_evaluate(const char *json, size_t length, int http_status, const char *base,
                                    const char *app, const char *name, const char *title_id,
                                    const char *installed, const char *running, self_update_offer *offer,
                                    insider_report *report);

    /* The request and its answer: POST <base>/v1/updates with the code and `running` (or
     * `installed` without it) as installed_version, User-Agent <name>/<that version>, then
     * insider_evaluate. SELF_UPDATE_AVAILABLE with the offer for INSIDER_UPDATE, SELF_UPDATE_UNKNOWN
     * (and an empty offer) for anything else. Nothing is sent when an argument can't be (a code that
     * isn't valid, a base that isn't). Blocking. */
    self_update_check_result insider_check(insider_post_fn post, void *post_user, const char *base, const char *code,
                                           const char *app, const char *name, const char *title_id,
                                           const char *installed, const char *running, self_update_offer *offer,
                                           insider_report *report);

    /* The update check with Test builds on: with a service named and a code kept, insider_check
     * first; when it offers a build, that is the answer and GitHub is not asked. Otherwise (no code,
     * or any answer but an offer) self_update_check_tests_from(platform, repository, ...) exactly as
     * without insider builds, its report in `github`. insider->status says which it was
     * (INSIDER_UPDATE: the insider's offer; INSIDER_NOT_ASKED: no service or no code). Blocking. */
    self_update_check_result insider_check_tests_from(const self_update_platform *platform, insider_post_fn post,
                                                      void *post_user, const char *base, const char *code,
                                                      const char *app, const char *repository, const char *name,
                                                      const char *title_id, const char *installed,
                                                      const char *running, self_update_offer *offer,
                                                      insider_report *insider, github_release_report *github);

    /* 1 for the answers after which the settings say where to get a code (not_qualified,
     * invalid_code, expired_code); no answer at all says nothing. */
    int insider_status_hints(insider_status status);

    /* A short English word for a status, for logs. */
    const char *insider_status_text(insider_status status);

    /* The same for the running app, on the console (self_update_ps5.c): its title ID and content
     * version from its param.json, the request through the console's libcurl. */
    self_update_check_result self_update_check_self_insider(const char *base, const char *code, const char *app,
                                                            const char *repository, const char *name,
                                                            const char *running, self_update_offer *offer,
                                                            insider_report *insider,
                                                            github_release_report *github);

#ifdef __cplusplus
}
#endif

#endif
