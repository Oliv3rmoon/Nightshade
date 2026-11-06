/*
 * ProsperoEden - Insider builds: new test builds for members of the nightly Discord, early.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See insider.h. This file is C11 and also compiles as C++. It holds no console calls: the request
 * goes through an insider_post_fn and GitHub's check through the kit's self_update_platform, so the
 * same code runs in the host tests.
 *
 * What it reads comes from the network: every length is checked, a value that doesn't fit is an
 * error, never a truncation, and a build that fails any check is not offered.
 */
#include "insider.h"

#include "self_update_protocol.h"
#include "update_check.h"

#include <stdio.h>
#include <string.h>

/* ---- The code -------------------------------------------------------------------------------- */

static int in_alphabet(char c)
{
    return c != '\0' && strchr(INSIDER_ALPHABET, c) != NULL;
}

int insider_code_normalise(const char *typed, char code[INSIDER_CODE_LENGTH + 1])
{
    size_t used = 0;
    if (code == NULL)
        return 0;
    code[0] = '\0';
    if (typed == NULL)
        return 0;
    for (; *typed != '\0'; ++typed)
    {
        char c = *typed;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (!in_alphabet(c))
            continue;
        if (used == INSIDER_CODE_LENGTH)
        {
            code[0] = '\0';
            return 0; /* more than a code */
        }
        code[used++] = c;
    }
    if (used != INSIDER_CODE_LENGTH)
    {
        code[0] = '\0';
        return 0;
    }
    code[used] = '\0';
    return 1;
}

int insider_code_valid(const char *code)
{
    size_t i;
    if (code == NULL)
        return 0;
    for (i = 0; i < INSIDER_CODE_LENGTH; ++i)
        if (!in_alphabet(code[i]))
            return 0;
    return code[INSIDER_CODE_LENGTH] == '\0';
}

int insider_code_read(const char *text, size_t length, char code[INSIDER_CODE_LENGTH + 1])
{
    size_t i;
    if (code == NULL)
        return 0;
    code[0] = '\0';
    if (text == NULL || length != INSIDER_CODE_LENGTH + 1 || text[INSIDER_CODE_LENGTH] != '\n')
        return 0;
    for (i = 0; i < INSIDER_CODE_LENGTH; ++i)
        if (!in_alphabet(text[i]))
            return 0;
    memcpy(code, text, INSIDER_CODE_LENGTH);
    code[INSIDER_CODE_LENGTH] = '\0';
    return 1;
}

/* ---- Addresses and the request ---------------------------------------------------------------- */

static int is_alnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int insider_base_valid(const char *base)
{
    const char *at;
    size_t length;
    if (base == NULL || strncmp(base, "https://", 8) != 0)
        return 0;
    length = strlen(base);
    if (length > INSIDER_MAX_BASE)
        return 0;
    /* The host: labels of letters, digits and hyphens, a hyphen never first or last. */
    at = base + 8;
    for (;;)
    {
        const char *label = at;
        while (is_alnum(*at) || *at == '-')
            ++at;
        if (at == label || label[0] == '-' || at[-1] == '-')
            return 0;
        if (*at != '.')
            break;
        ++at;
    }
    /* The path: segments of letters, digits, '.', '_' and '-', neither "." nor "..". */
    while (*at == '/')
    {
        const char *segment = ++at;
        while (is_alnum(*at) || *at == '.' || *at == '_' || *at == '-')
            ++at;
        if (at == segment || (at - segment == 1 && segment[0] == '.') ||
            (at - segment == 2 && segment[0] == '.' && segment[1] == '.'))
            return 0;
    }
    return *at == '\0';
}

int insider_app_valid(const char *app)
{
    size_t i;
    if (app == NULL || !(app[0] >= 'a' && app[0] <= 'z'))
        return 0;
    for (i = 1; app[i] != '\0'; ++i)
        if (i > 31 || !((app[i] >= 'a' && app[i] <= 'z') || (app[i] >= '0' && app[i] <= '9') || app[i] == '-'))
            return 0;
    return i >= 2;
}

int insider_content_version(const char *text, char version[12])
{
    static const unsigned most[3] = {99u, 999u, 999u};
    unsigned parts[3] = {0, 0, 0};
    int part;
    if (version != NULL)
        version[0] = '\0';
    if (text == NULL)
        return 0;
    if (*text == 'v')
        ++text;
    for (part = 0; part < 3; ++part)
    {
        int digits = 0;
        if (part > 0 && *text++ != '.')
            return 0;
        while (*text >= '0' && *text <= '9')
        {
            if (++digits > 6)
                return 0;
            parts[part] = parts[part] * 10u + (unsigned)(*text++ - '0');
        }
        if (digits == 0 || parts[part] > most[part])
            return 0;
    }
    if (*text != '\0')
        return 0;
    if (version != NULL)
        (void)snprintf(version, 12, "%02u.%03u.%03u", parts[0], parts[1], parts[2]);
    return 1;
}

size_t insider_updates_url(char *out, size_t size, const char *base)
{
    int length;
    if (out == NULL || size == 0)
        return 0;
    out[0] = '\0';
    if (!insider_base_valid(base))
        return 0;
    length = snprintf(out, size, "%s/v1/updates", base);
    if (length <= 0 || (size_t)length >= size)
    {
        out[0] = '\0';
        return 0;
    }
    return (size_t)length;
}

/* Appends `text` as a JSON string. 0 when it doesn't fit. */
static int put_string(char *out, size_t size, size_t *used, const char *text)
{
    char piece[8];
    if (*used + 1 >= size)
        return 0;
    out[(*used)++] = '"';
    for (; *text != '\0'; ++text)
    {
        const unsigned char c = (unsigned char)*text;
        size_t length = 1;
        if (c == '"' || c == '\\')
        {
            piece[0] = '\\';
            piece[1] = (char)c;
            length = 2;
        }
        else if (c < 0x20 || c == 0x7f)
            length = (size_t)snprintf(piece, sizeof(piece), "\\u%04x", c);
        else
            piece[0] = (char)c;
        if (*used + length + 1 >= size)
            return 0;
        memcpy(out + *used, piece, length);
        *used += length;
    }
    out[(*used)++] = '"';
    out[*used] = '\0';
    return 1;
}

static int put_text(char *out, size_t size, size_t *used, const char *text)
{
    const size_t length = strlen(text);
    if (*used + length + 1 >= size)
        return 0;
    memcpy(out + *used, text, length + 1);
    *used += length;
    return 1;
}

size_t insider_request(char *out, size_t size, const char *app, const char *version, const char *code)
{
    size_t used = 0;
    if (out == NULL || size == 0)
        return 0;
    out[0] = '\0';
    if (app == NULL || version == NULL || code == NULL)
        return 0;
    if (size > INSIDER_MAX_REQUEST + 1)
        size = INSIDER_MAX_REQUEST + 1;
    if (!put_text(out, size, &used, "{\"app\":") || !put_string(out, size, &used, app) ||
        !put_text(out, size, &used, ",\"installed_version\":") || !put_string(out, size, &used, version) ||
        !put_text(out, size, &used, ",\"code\":") || !put_string(out, size, &used, code) ||
        !put_text(out, size, &used, "}"))
    {
        out[0] = '\0';
        return 0;
    }
    return used;
}

/* ---- The answer ------------------------------------------------------------------------------ */

/* A member's string value, the object read whole first: a member said twice, an object cut short
 * or a value that isn't a string (or doesn't fit) is -1; 0 when there is none; 1 when copied. */
static int string_member(const char *json, size_t length, const char *key, char *out, size_t size)
{
    const char *value = NULL;
    size_t value_length = 0;
    const int found = update_check_json_value(json, length, key, &value, &value_length);
    if (found != 1)
        return found;
    if (value_length == 0 || value[0] != '"')
        return -1;
    return update_check_json_string(json, length, key, out, size) == 1 ? 1 : -1;
}

/* A plain word: letters, digits, '.', '_' and '-', 1 to size - 1 of them. */
static int plain_word(const char *text, size_t size)
{
    size_t i;
    for (i = 0; text[i] != '\0'; ++i)
        if (i + 1 >= size || !(is_alnum(text[i]) || text[i] == '.' || text[i] == '_' || text[i] == '-'))
            return 0;
    return i > 0;
}

static int lowercase_digest(const char *text)
{
    size_t i;
    for (i = 0; i < 64; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return 0;
    return text[64] == '\0';
}

/* The build's size: a whole number, no sign, no leading zero, no fraction. 1: read. */
static int size_member(const char *json, size_t length, uint64_t *out)
{
    const char *value = NULL;
    size_t value_length = 0;
    size_t i;
    uint64_t size = 0;
    if (update_check_json_value(json, length, "size", &value, &value_length) != 1 || value_length == 0 ||
        value_length > 19 || (value_length > 1 && value[0] == '0'))
        return 0;
    for (i = 0; i < value_length; ++i)
    {
        if (value[i] < '0' || value[i] > '9')
            return 0;
        size = size * 10u + (uint64_t)(value[i] - '0');
    }
    *out = size;
    return 1;
}

static void plain_title(char *title)
{
    for (; *title != '\0'; ++title)
        if ((unsigned char)*title < 0x20 || *title == 0x7f)
            *title = ' ';
}

/* Ends the reading: the status for the report, an empty offer unless it is an update. */
static insider_status finish(insider_status status, const char *why, self_update_offer *offer,
                             insider_report *report)
{
    if (status != INSIDER_UPDATE && offer != NULL)
        memset(offer, 0, sizeof(*offer));
    if (report != NULL)
    {
        report->status = status;
        report->why = why;
    }
    return status;
}

/* 1 when `available` may be offered to an install of `installed`: newer, on its line. */
static int offered_to(const char *available, const char *installed, const char **why)
{
    int comparable = 0;
    if (!github_release_same_line(available, installed))
    {
        *why = "another-line";
        return 0;
    }
    if (update_check_version_compare(available, installed, &comparable) <= 0 || !comparable)
    {
        *why = "not-newer";
        return 0;
    }
    return 1;
}

insider_status insider_evaluate(const char *json, size_t length, int http_status, const char *base,
                                const char *app, const char *name, const char *title_id,
                                const char *installed, const char *running, self_update_offer *offer,
                                insider_report *report)
{
    char status[32];
    char text[64];
    char available[12];
    char tag[40];
    char sha256[72];
    char url[SELF_UPDATE_INSIDER_MAX_URL + 8];
    const char *build = NULL;
    const char *why = NULL;
    size_t build_length = 0;
    uint64_t size = 0;
    int found;
    if (offer != NULL)
        memset(offer, 0, sizeof(*offer));
    if (report != NULL)
    {
        memset(report, 0, sizeof(*report));
        report->http_status = http_status;
    }
    if (json == NULL || offer == NULL || base == NULL || app == NULL || name == NULL || title_id == NULL ||
        installed == NULL)
        return finish(INSIDER_BAD_ARGUMENT, NULL, offer, report);

    /* The status, from an object read whole: anything else is an answer it can't read. */
    found = string_member(json, length, "status", status, sizeof(status));
    if (found == 1 && plain_word(status, sizeof(status)) && report != NULL)
        (void)snprintf(report->answer, sizeof(report->answer), "%s", status);
    if (http_status != 200)
        return finish(INSIDER_OTHER, NULL, offer, report);
    if (found != 1 || !plain_word(status, sizeof(status)))
        return finish(INSIDER_BAD_RESPONSE, NULL, offer, report);
    if (strcmp(status, "none") == 0)
        return finish(INSIDER_NONE, NULL, offer, report);
    if (strcmp(status, "not_qualified") == 0)
        return finish(INSIDER_NOT_QUALIFIED, NULL, offer, report);
    if (strcmp(status, "invalid_code") == 0)
        return finish(INSIDER_INVALID_CODE, NULL, offer, report);
    if (strcmp(status, "expired_code") == 0)
        return finish(INSIDER_EXPIRED_CODE, NULL, offer, report);
    if (strcmp(status, "update") != 0)
        return finish(INSIDER_OTHER, NULL, offer, report);

    /* "update": the answer is for this app, and its build passes every check. */
    found = string_member(json, length, "app", text, sizeof(text));
    if (found < 0 || (found == 1 && strcmp(text, app) != 0))
        return finish(INSIDER_REFUSED, "app", offer, report);
    if (update_check_json_value(json, length, "build", &build, &build_length) != 1 || build_length == 0 ||
        build[0] != '{')
        return finish(INSIDER_REFUSED, "build", offer, report);

    if (string_member(build, build_length, "tag", tag, sizeof(tag)) != 1 || !plain_word(tag, sizeof(tag)))
        return finish(INSIDER_REFUSED, "tag", offer, report);
    if (report != NULL)
        (void)snprintf(report->tag, sizeof(report->tag), "%s", tag);
    if (string_member(build, build_length, "version", text, sizeof(text)) != 1 ||
        !insider_content_version(text, available))
        return finish(INSIDER_REFUSED, "version", offer, report);
    if (!offered_to(available, installed, &why) || (running != NULL && !offered_to(available, running, &why)))
        return finish(INSIDER_REFUSED, why, offer, report);
    if (string_member(build, build_length, "sha256", sha256, sizeof(sha256)) != 1 || !lowercase_digest(sha256))
        return finish(INSIDER_REFUSED, "sha256", offer, report);
    if (!size_member(build, build_length, &size) || size == 0 || size > SELF_UPDATE_MAX_ARCHIVE)
        return finish(INSIDER_REFUSED, "size", offer, report);
    if (string_member(build, build_length, "url", url, sizeof(url)) != 1 || !self_update_insider_url(base, url) ||
        strlen(url) >= sizeof(offer->artifact))
        return finish(INSIDER_REFUSED, "url", offer, report);
    if (report != NULL)
    {
        const char *host = url + 8;
        (void)snprintf(report->host, sizeof(report->host), "%.*s", (int)strcspn(host, "/"), host);
        if (string_member(build, build_length, "file", text, sizeof(text)) == 1 && plain_word(text, sizeof(text)))
            (void)snprintf(report->file, sizeof(report->file), "%s", text);
    }

    (void)snprintf(offer->title, sizeof(offer->title), "%s", title_id);
    (void)snprintf(offer->name, sizeof(offer->name), "%s", name);
    (void)snprintf(offer->installed, sizeof(offer->installed), "%s", installed);
    memcpy(offer->available, available, sizeof(offer->available));
    memcpy(offer->version, tag, sizeof(offer->version));
    memcpy(offer->artifact, url, strlen(url) + 1);
    memcpy(offer->sha256, sha256, sizeof(offer->sha256));
    offer->size = size;
    offer->prerelease = 1;
    /* The title and the notes are for the dialog: one that can't be read is left out. */
    if (string_member(build, build_length, "title", offer->release_title, sizeof(offer->release_title)) != 1)
        offer->release_title[0] = '\0';
    plain_title(offer->release_title);
    if (string_member(build, build_length, "notes", offer->notes, sizeof(offer->notes)) != 1)
        offer->notes[0] = '\0';
    return finish(INSIDER_UPDATE, NULL, offer, report);
}

/* ---- The request ----------------------------------------------------------------------------- */

self_update_check_result insider_check(insider_post_fn post, void *post_user, const char *base, const char *code,
                                       const char *app, const char *name, const char *title_id,
                                       const char *installed, const char *running, self_update_offer *offer,
                                       insider_report *report)
{
    static char body[INSIDER_MAX_ANSWER]; /* one check at a time, as the kit's */
    char url[INSIDER_MAX_BASE + 16];
    char request[INSIDER_MAX_REQUEST + 1];
    char agent[96];
    const char *version = running != NULL ? running : installed;
    size_t length = 0;
    int status = 0;
    int result;
    if (offer != NULL)
        memset(offer, 0, sizeof(*offer));
    if (report != NULL)
        memset(report, 0, sizeof(*report));
    if (post == NULL || offer == NULL || name == NULL || title_id == NULL || !insider_code_valid(code) ||
        !insider_app_valid(app) || update_check_version_parse(installed, NULL) != 1 ||
        update_check_version_parse(version, NULL) != 1 || insider_updates_url(url, sizeof(url), base) == 0 ||
        insider_request(request, sizeof(request), app, version, code) == 0 || strlen(name) > 63 ||
        snprintf(agent, sizeof(agent), "%s/%s", name, version) >= (int)sizeof(agent))
    {
        (void)finish(INSIDER_BAD_ARGUMENT, NULL, offer, report);
        return SELF_UPDATE_UNKNOWN;
    }
    result = post(post_user, url, agent, request, body, sizeof(body), &length, &status);
    if (report != NULL)
        report->http_status = status;
    if (result == SELF_UPDATE_FETCH_TOO_LARGE)
    {
        (void)finish(INSIDER_TOO_LARGE, NULL, offer, report);
        return SELF_UPDATE_UNKNOWN;
    }
    if (result != 0 || length > sizeof(body))
    {
        if (report != NULL)
        {
            report->http_status = 0;
            report->platform_error = result;
        }
        (void)finish(INSIDER_NETWORK, NULL, offer, report);
        return SELF_UPDATE_UNKNOWN;
    }
    return insider_evaluate(body, length, status, base, app, name, title_id, installed, running, offer, report) ==
                   INSIDER_UPDATE
               ? SELF_UPDATE_AVAILABLE
               : SELF_UPDATE_UNKNOWN;
}

self_update_check_result insider_check_tests_from(const self_update_platform *platform, insider_post_fn post,
                                                  void *post_user, const char *base, const char *code,
                                                  const char *app, const char *repository, const char *name,
                                                  const char *title_id, const char *installed,
                                                  const char *running, self_update_offer *offer,
                                                  insider_report *insider, github_release_report *github)
{
    insider_report report;
    memset(&report, 0, sizeof(report));
    report.status = INSIDER_NOT_ASKED;
    if (github != NULL)
        memset(github, 0, sizeof(*github));
    if (base != NULL && base[0] != '\0' && code != NULL && code[0] != '\0' &&
        insider_check(post, post_user, base, code, app, name, title_id, installed, running, offer, &report) ==
            SELF_UPDATE_AVAILABLE)
    {
        if (insider != NULL)
            *insider = report;
        return SELF_UPDATE_AVAILABLE;
    }
    if (insider != NULL)
        *insider = report;
    return self_update_check_tests_from(platform, repository, name, title_id, installed, offer, github);
}

int insider_status_hints(insider_status status)
{
    return status == INSIDER_NOT_QUALIFIED || status == INSIDER_INVALID_CODE || status == INSIDER_EXPIRED_CODE;
}

const char *insider_status_text(insider_status status)
{
    static const char *const names[] = {"update",  "none",      "not_qualified", "invalid_code",
                                        "expired_code", "other", "network",      "too-large",
                                        "malformed",    "refused", "bad-argument", "not-asked"};
    return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status] : "?";
}
