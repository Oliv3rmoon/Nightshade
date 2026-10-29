/*
 * ProsperoEden - Updates from a GitHub repository's releases instead of the homebrew.page catalog.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See github_release.h. This file is C11 and also compiles as C++. It holds no console calls: the
 * requests go through the kit's self_update_platform, so the same code runs in the host tests.
 *
 * What it reads comes from the network, as in update_check.c: every length is checked, and a
 * value that doesn't fit is an error, never a truncation (the notes alone are cut, on purpose,
 * and say so).
 */
#include "github_release.h"

#include "self_update_protocol.h"
#include "update_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Names --------------------------------------------------------------------------------- */

static int is_alnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* A release file's or the app's name: letters, digits, '.', '_' and '-', not starting with '.'. */
static int is_file_name(const char *text, size_t longest)
{
    size_t i;
    if (text == NULL || text[0] == '\0' || text[0] == '.')
        return 0;
    for (i = 0; text[i] != '\0'; ++i)
        if (i >= longest || !(is_alnum(text[i]) || text[i] == '.' || text[i] == '_' || text[i] == '-'))
            return 0;
    return 1;
}

int github_release_tag_version(const char *tag, char version[12])
{
    char text[12];
    size_t length;
    if (tag == NULL || tag[0] != 'v')
        return 0;
    length = strlen(tag + 1);
    /* make-dist.py drops one leading zero: one digit before the first dot is that digit after a
     * zero; two digits are the version as it is, and never start with a zero (it would have
     * dropped it). */
    if (length == 9 && tag[1] >= '0' && tag[1] <= '9')
    {
        text[0] = '0';
        memcpy(text + 1, tag + 1, 10);
    }
    else if (length == 10 && tag[1] >= '1' && tag[1] <= '9')
    {
        memcpy(text, tag + 1, 11);
    }
    else
    {
        return 0;
    }
    if (!update_check_version_parse(text, NULL))
        return 0;
    if (version != NULL)
        memcpy(version, text, sizeof(text));
    return 1;
}

int github_release_repository_valid(const char *repository)
{
    const char *slash;
    const char *name;
    size_t i;
    if (repository == NULL || (slash = strchr(repository, '/')) == NULL)
        return 0;
    /* An owner: letters, digits and hyphens, not first, up to 39. A repository's name: letters,
     * digits, '.', '_' and '-', up to 100, and neither "." nor "..". */
    if (slash == repository || slash - repository > 39 || repository[0] == '-')
        return 0;
    for (i = 0; repository + i < slash; ++i)
        if (!is_alnum(repository[i]) && repository[i] != '-')
            return 0;
    name = slash + 1;
    if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (i = 0; name[i] != '\0'; ++i)
        if (i >= 100 || !(is_alnum(name[i]) || name[i] == '.' || name[i] == '_' || name[i] == '-'))
            return 0;
    return 1;
}

size_t github_release_url(char *out, size_t size, const char *repository)
{
    int length;
    if (out == NULL || size == 0)
        return 0;
    out[0] = '\0';
    if (!github_release_repository_valid(repository))
        return 0;
    length = snprintf(out, size, "%s%s/releases/latest", GITHUB_RELEASE_API, repository);
    if (length <= 0 || (size_t)length >= size)
    {
        out[0] = '\0';
        return 0;
    }
    return (size_t)length;
}

size_t github_release_file_url(char *out, size_t size, const char *repository, const char *tag,
                               const char *file)
{
    int length;
    if (out == NULL || size == 0)
        return 0;
    out[0] = '\0';
    if (!github_release_repository_valid(repository) || !github_release_tag_version(tag, NULL) ||
        !is_file_name(file, 128))
        return 0;
    length = snprintf(out, size, "https://github.com/%s/releases/download/%s/%s", repository, tag,
                      file);
    if (length <= 0 || (size_t)length >= size)
    {
        out[0] = '\0';
        return 0;
    }
    return (size_t)length;
}

int github_release_api_url_allowed(const char *url)
{
    static const char start[] = GITHUB_RELEASE_HOST "repositories/";
    static const char end[] = "/releases/latest";
    size_t digits = 0;
    if (url == NULL || strncmp(url, start, sizeof(start) - 1) != 0)
        return 0;
    url += sizeof(start) - 1;
    /* GitHub's number for the repository: digits, no leading zero, at most a 64-bit number's. */
    while (url[digits] >= '0' && url[digits] <= '9')
        ++digits;
    if (digits == 0 || digits > 19 || url[0] == '0')
        return 0;
    return strcmp(url + digits, end) == 0;
}

/* ---- The release --------------------------------------------------------------------------- */

static int is_literal(const char *value, size_t length, const char *literal)
{
    return length == strlen(literal) && memcmp(value, literal, length) == 0;
}

/* update_check_json_string, and -2 when `key` is there twice: an answer that says two things is
 * read as neither (update_check_json_value refuses it so for the members it reads). */
static int string_member(const char *json, size_t length, const char *key, char *out, size_t size)
{
    const char *value = NULL;
    size_t value_length = 0;
    if (update_check_json_value(json, length, key, &value, &value_length) < 0)
        return -2;
    return update_check_json_string(json, length, key, out, size);
}

/* GITHUB_RELEASE_NEWER when the boolean `key` is false, NOT_PUBLISHED when it is true. */
static github_release_reason published(const char *json, size_t length, const char *key)
{
    const char *value = NULL;
    size_t size = 0;
    if (update_check_json_value(json, length, key, &value, &size) != 1)
        return GITHUB_RELEASE_BAD_RESPONSE;
    if (is_literal(value, size, "false"))
        return GITHUB_RELEASE_NEWER;
    return is_literal(value, size, "true") ? GITHUB_RELEASE_NOT_PUBLISHED : GITHUB_RELEASE_BAD_RESPONSE;
}

github_release_reason github_release_evaluate(const char *json, size_t length,
                                              const char *installed, github_release *release)
{
    const char *assets = NULL;
    const char *value = NULL;
    size_t assets_length = 0;
    size_t size = 0;
    size_t cursor = 0;
    char suffix[48];
    size_t suffix_length;
    int sums = 0;
    int comparable = 0;
    int found;
    github_release_reason state;
    if (release == NULL)
        return GITHUB_RELEASE_BAD_ARGUMENT;
    memset(release, 0, sizeof(*release));
    if (!update_check_version_parse(installed, NULL))
        return GITHUB_RELEASE_BAD_ARGUMENT;
    /* Read as one whole object first: an answer cut short in transit is refused whole. */
    if (json == NULL || update_check_json_value(json, length, "assets", &assets, &assets_length) != 1)
        return GITHUB_RELEASE_BAD_RESPONSE;
    /* releases/latest never gives a draft or a pre-release; a release that says it is one is not
     * offered all the same. */
    if ((state = published(json, length, "draft")) != GITHUB_RELEASE_NEWER ||
        (state = published(json, length, "prerelease")) != GITHUB_RELEASE_NEWER)
        return state;
    if ((found = string_member(json, length, "tag_name", release->tag, sizeof(release->tag))) != 1)
    {
        release->tag[0] = '\0';
        if (found == -2)
            return GITHUB_RELEASE_BAD_RESPONSE;
        /* A tag too long to be a version is still a tag. */
        return update_check_json_value(json, length, "tag_name", &value, &size) == 1 && size > 0 &&
                       value[0] == '"'
                   ? GITHUB_RELEASE_NOT_A_VERSION
                   : GITHUB_RELEASE_BAD_RESPONSE;
    }
    if (!github_release_tag_version(release->tag, release->available))
        return GITHUB_RELEASE_NOT_A_VERSION;
    if (update_check_version_compare(release->available, installed, &comparable) <= 0 || !comparable)
        return GITHUB_RELEASE_NOT_NEWER;

    /* Its files, each uploaded in full, read once from first to last (GitHub allows 1,000):
     * SHA256SUMS, and the ZIPs of this version, whatever the release line calls itself before
     * "-<tag>.zip" (SHA256SUMS says which one is the release). */
    suffix_length = (size_t)snprintf(suffix, sizeof(suffix), "-%s.zip", release->tag);
    for (;;)
    {
        const char *asset = NULL;
        size_t asset_length = 0;
        char file[128];
        char upload[16];
        unsigned bytes = 0;
        size_t file_length;
        int is_zip;
        int named;
        found = update_check_json_array_next(assets, assets_length, &cursor, &asset, &asset_length);
        if (found == 0)
            break;
        if (found != 1)
            return GITHUB_RELEASE_BAD_RESPONSE;
        /* A name too long for `file` is neither. */
        if ((named = string_member(asset, asset_length, "name", file, sizeof(file))) == -2)
            return GITHUB_RELEASE_BAD_RESPONSE;
        if (named != 1)
            continue;
        file_length = strlen(file);
        is_zip = file_length > suffix_length && strcmp(file + file_length - suffix_length, suffix) == 0 &&
                 is_file_name(file, sizeof(file));
        if (!is_zip && strcmp(file, "SHA256SUMS") != 0)
            continue;
        if ((named = string_member(asset, asset_length, "state", upload, sizeof(upload))) == -2)
            return GITHUB_RELEASE_BAD_RESPONSE;
        if (named != 1 || strcmp(upload, "uploaded") != 0 ||
            update_check_json_uint(asset, asset_length, "size", &bytes) != 1)
            continue;
        if (!is_zip)
        {
            ++sums;
            release->sums_size = bytes;
        }
        else if (bytes != 0 && bytes <= SELF_UPDATE_MAX_ARCHIVE)
        {
            /* More ZIPs of the version than are told apart: none is dropped to make room. */
            if (release->zip_count == GITHUB_RELEASE_MAX_ZIPS)
                return GITHUB_RELEASE_TOO_MANY_ZIPS;
            memcpy(release->zips[release->zip_count], file, file_length + 1);
            release->zip_sizes[release->zip_count++] = bytes;
        }
    }
    if (release->zip_count == 0)
        return GITHUB_RELEASE_NO_ZIP;
    if (sums != 1 || release->sums_size == 0 || release->sums_size > GITHUB_RELEASE_MAX_SUMS)
        return GITHUB_RELEASE_NO_SUMS;

    /* For the What's new view; none (null, or not text) is no notes. They are read as GitHub has
     * them first, which is never longer than they are in the answer, into memory of that size
     * for as long as they are made plain text (none: no notes). */
    if ((found = update_check_json_value(json, length, "body", &value, &size)) < 0)
        return GITHUB_RELEASE_BAD_RESPONSE;
    if (found == 1 && size > 0 && value[0] == '"')
    {
        char *body = (char *)malloc(size);
        if (body != NULL && update_check_json_string(json, length, "body", body, size) == 1)
            (void)github_release_notes(body, strlen(body), release->notes, sizeof(release->notes),
                                       GITHUB_RELEASE_NOTES_CHARACTERS, &release->notes_truncated);
        free(body);
    }
    return GITHUB_RELEASE_NEWER;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int github_release_sums(const char *text, size_t length, const char *file, char sha256[65])
{
    char digest[65] = {0};
    size_t at = 0;
    size_t name;
    int lines = 0;
    int malformed = 0;
    if (text == NULL || sha256 == NULL || !is_file_name(file, 128))
        return -1;
    name = strlen(file);
    while (at < length)
    {
        size_t end = at;
        size_t size;
        const char *line = text + at;
        size_t i;
        while (end < length && text[end] != '\n')
            ++end;
        size = end - at;
        if (size > 0 && line[size - 1] == '\r')
            --size;
        at = end + 1;
        /* The file's line ends in its name after a space or sha256sum's binary mark; one that
         * ends in "...-v1.005.100.zip.sig" or "My...zip" names another file. */
        if (size <= name || memcmp(line + size - name, file, name) != 0 ||
            (line[size - name - 1] != ' ' && line[size - name - 1] != '*'))
            continue;
        ++lines;
        if (size != 64 + 2 + name || line[64] != ' ' || (line[65] != ' ' && line[65] != '*'))
        {
            malformed = 1;
            continue;
        }
        for (i = 0; i < 64 && !malformed; ++i)
        {
            const int value = hex_value(line[i]);
            if (value < 0)
                malformed = 1;
            else
                digest[i] = "0123456789abcdef"[value];
        }
        digest[64] = '\0';
    }
    if (lines == 0)
        return 0;
    if (lines > 1 || malformed)
        return -1;
    memcpy(sha256, digest, sizeof(digest));
    return 1;
}

github_release_reason github_release_choose(github_release *release, const char *sums,
                                            size_t length, char sha256[65])
{
    char digest[65] = {0};
    char listed_digest[65] = {0};
    size_t index;
    size_t chosen = 0;
    int listed = 0;
    if (release == NULL || sha256 == NULL)
        return GITHUB_RELEASE_BAD_ARGUMENT;
    sha256[0] = '\0';
    release->zip[0] = '\0';
    release->zip_size = 0;
    for (index = 0; index < release->zip_count; ++index)
    {
        const int found = github_release_sums(sums, length, release->zips[index], digest);
        if (found < 0)
            return GITHUB_RELEASE_SUMS_MALFORMED;
        if (found == 1)
        {
            ++listed;
            chosen = index;
            memcpy(listed_digest, digest, sizeof(digest));
        }
    }
    if (listed == 0)
        return GITHUB_RELEASE_SUMS_NO_LINE;
    if (listed > 1)
        return GITHUB_RELEASE_SUMS_TWO_ZIPS;
    memcpy(release->zip, release->zips[chosen], sizeof(release->zip));
    release->zip_size = release->zip_sizes[chosen];
    memcpy(sha256, listed_digest, sizeof(listed_digest));
    return GITHUB_RELEASE_NEWER;
}

/* ---- The notes ----------------------------------------------------------------------------- */

typedef struct notes_out
{
    char *out;
    size_t size;
    size_t used;
    size_t characters; /* code points written */
    size_t limit;
    size_t kept;       /* where the last whole line ends */
    int stopped;       /* the limit was reached */
} notes_out;

/* Appends text, a character at a time; stops before one that doesn't fit whole (with room for
 * the end of the text). */
static void notes_put(notes_out *n, const char *text, size_t length)
{
    size_t i;
    for (i = 0; i < length && !n->stopped; ++i)
    {
        const unsigned char c = (unsigned char)text[i];
        if ((c & 0xC0u) != 0x80u)
        {
            if (n->characters >= n->limit || n->used + 5 > n->size)
            {
                n->stopped = 1;
                return;
            }
            ++n->characters;
        }
        else if (n->used + 2 > n->size)
        {
            n->stopped = 1;
            return;
        }
        n->out[n->used++] = (char)c;
    }
}

/* Ends the line written so far (it is whole now); a blank line between them when asked. */
static void notes_line(notes_out *n, int blank)
{
    if (n->used == 0)
        return;
    n->kept = n->used;
    notes_put(n, blank ? "\n\n" : "\n", blank ? 2u : 1u);
}

static const char *find_char(const char *text, size_t length, char wanted)
{
    return length > 0 ? (const char *)memchr(text, wanted, length) : NULL;
}

static int contains(const char *text, size_t length, const char *wanted)
{
    const size_t size = strlen(wanted);
    size_t i;
    for (i = 0; i + size <= length; ++i)
        if (memcmp(text + i, wanted, size) == 0)
            return 1;
    return 0;
}

/* A line's text without its marks: emphasis and code marks go, a link or an image keeps its
 * text, an address in <> loses the <>, a backslash escape keeps its character. */
static void notes_inline(notes_out *n, const char *text, size_t length)
{
    size_t i = 0;
    while (i < length && !n->stopped)
    {
        const char c = text[i];
        if (c == '\\' && i + 1 < length && text[i + 1] > ' ' && text[i + 1] < 0x7f &&
            !is_alnum(text[i + 1]))
        {
            notes_put(n, text + i + 1, 1);
            i += 2;
            continue;
        }
        if (c == '`')
        {
            ++i;
            continue;
        }
        if ((c == '*' || c == '_') && i + 1 < length && text[i + 1] == c)
        {
            i += 2;
            continue;
        }
        if (c == '!' && i + 1 < length && text[i + 1] == '[')
        {
            ++i;
            continue;
        }
        if (c == '[')
        {
            /* [text](address), its text without brackets inside it. */
            const char *close = find_char(text + i + 1, length - i - 1, ']');
            const size_t inner = close != NULL ? (size_t)(close - text) - i - 1 : 0;
            const size_t after = close != NULL ? (size_t)(close - text) + 1 : length;
            const char *end =
                after < length && text[after] == '(' ? find_char(text + after, length - after, ')') : NULL;
            if (end != NULL && find_char(text + i + 1, inner, '[') == NULL)
            {
                notes_inline(n, text + i + 1, inner);
                i = (size_t)(end - text) + 1;
                continue;
            }
        }
        if (c == '<' && length - i > 8 &&
            (memcmp(text + i + 1, "https://", 8) == 0 || memcmp(text + i + 1, "http://", 7) == 0))
        {
            const char *close = find_char(text + i + 1, length - i - 1, '>');
            if (close != NULL)
            {
                notes_put(n, text + i + 1, (size_t)(close - text) - i - 1);
                i = (size_t)(close - text) + 1;
                continue;
            }
        }
        notes_put(n, text + i, 1);
        ++i;
    }
}

/* "Warning: " for GitHub's "[!WARNING]" (a callout's first line), NULL for anything else. */
static const char *callout(const char *line, size_t length)
{
    static const char *const kinds[][2] = {{"[!NOTE]", "Note: "},
                                           {"[!TIP]", "Tip: "},
                                           {"[!IMPORTANT]", "Important: "},
                                           {"[!WARNING]", "Warning: "},
                                           {"[!CAUTION]", "Caution: "}};
    size_t kind;
    for (kind = 0; kind < sizeof(kinds) / sizeof(kinds[0]); ++kind)
    {
        const char *mark = kinds[kind][0];
        size_t i;
        if (strlen(mark) != length)
            continue;
        for (i = 0; i < length; ++i)
        {
            char a = line[i];
            if (a >= 'a' && a <= 'z')
                a = (char)(a - 'a' + 'A');
            if (a != mark[i])
                break;
        }
        if (i == length)
            return kinds[kind][1];
    }
    return NULL;
}

/* A thematic break: three or more of one of '-', '*' and '_', with spaces between them or not. */
static int is_rule(const char *line, size_t length)
{
    size_t i;
    int marks = 0;
    if (length < 3 || (line[0] != '-' && line[0] != '*' && line[0] != '_'))
        return 0;
    for (i = 0; i < length; ++i)
    {
        if (line[i] == line[0])
            ++marks;
        else if (line[i] != ' ')
            return 0;
    }
    return marks >= 3;
}

/* "1. " or "12) ": an item of a numbered list, kept as it is. */
static int is_numbered(const char *line, size_t length)
{
    size_t i = 0;
    while (i < length && i < 9 && line[i] >= '0' && line[i] <= '9')
        ++i;
    return i > 0 && i + 1 < length && (line[i] == '.' || line[i] == ')') && line[i + 1] == ' ';
}

size_t github_release_notes(const char *markdown, size_t length, char *out, size_t size,
                            size_t characters, int *truncated)
{
    enum
    {
        joins = 0,   /* text: a paragraph's next line joins it */
        item = 1,    /* a list item: its next line joins it too */
        closed = 2   /* a heading or a table row: nothing joins it */
    };
    notes_out n;
    size_t at = 0;
    int code = 0;             /* inside a ``` block */
    int comment = 0;          /* inside <!-- --> */
    int open = 0;             /* the last line can be joined */
    int blank = 0;            /* a blank line before the next one */
    int quoted = 0;           /* the last line was in a > block */
    const char *label = NULL; /* "Warning: " for a callout's first line */
    if (truncated != NULL)
        *truncated = 0;
    if (out == NULL || size == 0)
        return 0;
    out[0] = '\0';
    if (markdown == NULL)
        return 0;
    memset(&n, 0, sizeof(n));
    n.out = out;
    n.size = size;
    n.limit = characters;
    while (at < length && !n.stopped)
    {
        size_t end = at;
        const char *line = markdown + at;
        size_t line_length;
        int kind = joins;
        int quote = 0;
        int bullet = 0;
        while (end < length && markdown[end] != '\n')
            ++end;
        line_length = end - at;
        at = end + 1;
        while (line_length > 0 && (line[line_length - 1] == '\r' || line[line_length - 1] == ' ' ||
                                   line[line_length - 1] == '\t'))
            --line_length;
        while (line_length > 0 && (line[0] == ' ' || line[0] == '\t'))
        {
            ++line;
            --line_length;
        }
        if (comment)
        {
            comment = !contains(line, line_length, "-->");
            continue;
        }
        if (line_length >= 3 && (memcmp(line, "```", 3) == 0 || memcmp(line, "~~~", 3) == 0))
        {
            code = !code;
            open = 0;
            continue;
        }
        if (code)
        {
            /* Code as it is, a line each. */
            if (line_length == 0)
            {
                blank = 1;
                continue;
            }
            notes_line(&n, blank);
            notes_put(&n, line, line_length);
            blank = 0;
            continue;
        }
        if (line_length >= 4 && memcmp(line, "<!--", 4) == 0)
        {
            comment = !contains(line + 4, line_length - 4, "-->");
            continue;
        }
        while (line_length > 0 && line[0] == '>')
        {
            quote = 1;
            ++line;
            --line_length;
            while (line_length > 0 && line[0] == ' ')
            {
                ++line;
                --line_length;
            }
        }
        /* Going into a quote or out of one ends what was open. */
        if (quote != quoted)
        {
            open = 0;
            label = NULL;
        }
        quoted = quote;
        if (line_length == 0 || is_rule(line, line_length))
        {
            blank = 1;
            open = 0;
            continue;
        }
        if (quote && callout(line, line_length) != NULL)
        {
            label = callout(line, line_length);
            open = 0;
            continue;
        }
        if (line[0] == '#')
        {
            size_t marks = 0;
            while (marks < line_length && line[marks] == '#')
                ++marks;
            if (marks <= 6 && (marks == line_length || line[marks] == ' '))
            {
                line += marks;
                line_length -= marks;
                while (line_length > 0 && (line[line_length - 1] == '#' || line[line_length - 1] == ' '))
                    --line_length;
                while (line_length > 0 && line[0] == ' ')
                {
                    ++line;
                    --line_length;
                }
                if (line_length == 0)
                {
                    blank = 1;
                    open = 0;
                    continue;
                }
                kind = closed;
            }
        }
        if (kind == joins && line_length >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') &&
            line[1] == ' ')
        {
            line += 2;
            line_length -= 2;
            while (line_length > 0 && line[0] == ' ')
            {
                ++line;
                --line_length;
            }
            bullet = 1;
            kind = item;
        }
        else if (kind == joins && is_numbered(line, line_length))
        {
            kind = item;
        }
        else if (kind == joins && line[0] == '|')
        {
            kind = closed;
        }
        if (kind != joins || !open || blank || label != NULL)
        {
            notes_line(&n, blank);
            if (bullet)
                notes_put(&n, "- ", 2);
            if (label != NULL)
                notes_put(&n, label, strlen(label));
            label = NULL;
        }
        else
        {
            notes_put(&n, " ", 1);
        }
        notes_inline(&n, line, line_length);
        blank = 0;
        open = kind != closed;
    }
    if (n.stopped)
    {
        /* Cut at the end of the last whole line; a first line too long alone is kept in part. */
        if (truncated != NULL)
            *truncated = 1;
        if (n.kept > 0)
            n.used = n.kept;
    }
    out[n.used] = '\0';
    return n.used;
}

const char *github_release_reason_text(github_release_reason reason)
{
    switch (reason)
    {
    case GITHUB_RELEASE_NEWER:
        return "newer";
    case GITHUB_RELEASE_NOT_NEWER:
        return "not-newer";
    case GITHUB_RELEASE_BAD_ARGUMENT:
        return "bad-argument";
    case GITHUB_RELEASE_NETWORK:
        return "network";
    case GITHUB_RELEASE_NONE:
        return "no-release";
    case GITHUB_RELEASE_RATE_LIMITED:
        return "rate-limited";
    case GITHUB_RELEASE_HTTP_STATUS:
        return "http-status";
    case GITHUB_RELEASE_BAD_RESPONSE:
        return "bad-response";
    case GITHUB_RELEASE_NOT_PUBLISHED:
        return "not-published";
    case GITHUB_RELEASE_NOT_A_VERSION:
        return "not-a-version";
    case GITHUB_RELEASE_NO_ZIP:
        return "no-zip";
    case GITHUB_RELEASE_NO_SUMS:
        return "no-sha256sums";
    case GITHUB_RELEASE_SUMS_UNREADABLE:
        return "sha256sums-unreadable";
    case GITHUB_RELEASE_SUMS_NO_LINE:
        return "sha256sums-no-line";
    case GITHUB_RELEASE_SUMS_MALFORMED:
        return "sha256sums-malformed";
    case GITHUB_RELEASE_SUMS_TWO_ZIPS:
        return "sha256sums-two-zips";
    case GITHUB_RELEASE_REDIRECT_REFUSED:
        return "redirect-refused";
    case GITHUB_RELEASE_TOO_LARGE:
        return "too-large";
    case GITHUB_RELEASE_TOO_MANY_ZIPS:
        return "too-many-zips";
    case GITHUB_RELEASE_NO_HASH:
        return "no-sha256";
    case GITHUB_RELEASE_HASHES_DIFFER:
        return "sha256-differ";
    case GITHUB_RELEASE_BAD_TAG:
        return "bad-tag";
    case GITHUB_RELEASE_OWNER_CHANGED:
        return "owner-changed";
    }
    return "unknown";
}

/* ---- The check ----------------------------------------------------------------------------- */

typedef struct sums_sink
{
    char *body;
    size_t capacity;
    size_t used;
} sums_sink;

static int on_sums(void *user, const void *data, size_t size)
{
    sums_sink *sink = (sums_sink *)user;
    if (size > sink->capacity - sink->used)
        return 0;
    memcpy(sink->body + sink->used, data, size);
    sink->used += size;
    return 1;
}

/* Text from the answer as the log may show it: printable ASCII as it is, anything else as \xNN, so
 * that it can't end the log's line or make one up; cut where an escape would not fit. */
static void log_text(char *out, size_t size, const char *text)
{
    size_t used = 0;
    for (; *text != '\0'; ++text)
    {
        const unsigned char c = (unsigned char)*text;
        char piece[5];
        size_t length = 1;
        if (c > 0x20 && c < 0x7f && c != '\\')
            piece[0] = (char)c;
        else
            length = (size_t)snprintf(piece, sizeof(piece), "\\x%02x", c);
        if (used + length >= size)
            break;
        memcpy(out + used, piece, length);
        used += length;
    }
    out[used] = '\0';
}

/* Ends the check without an offer: the reason for the log, and an empty offer as
 * self_update_check() leaves one. */
static self_update_check_result no_offer(self_update_offer *offer, github_release_report *report,
                                         github_release_reason reason)
{
    memset(offer, 0, sizeof(*offer));
    report->reason = reason;
    return reason == GITHUB_RELEASE_NOT_NEWER ? SELF_UPDATE_UP_TO_DATE : SELF_UPDATE_UNKNOWN;
}

/* releases/latest of `repository` into `answer`, following a renamed repository's redirect; with
 * `tag`, the release that tag names (releases/tags/<tag>), whose address a redirect never leads to
 * (github_release_api_url_allowed: a renamed repository's newest release alone).
 * GITHUB_RELEASE_NEWER: a 200 answer of *length bytes is there; otherwise why there is none. */
static github_release_reason fetch_release(const self_update_platform *platform, const char *repository,
                                           const char *tag, char *answer, size_t capacity, size_t *length,
                                           github_release_report *report)
{
    char url[256];
    int status = 0;
    int hop;
    if (tag == NULL)
    {
        if (github_release_url(url, sizeof(url), repository) == 0)
            return GITHUB_RELEASE_BAD_ARGUMENT;
    }
    else
    {
        const int written = snprintf(url, sizeof(url), "%s%s/releases/tags/%s", GITHUB_RELEASE_API, repository, tag);
        if (!github_release_repository_valid(repository) || !is_file_name(tag, 39) || written <= 0 ||
            (size_t)written >= sizeof(url))
            return GITHUB_RELEASE_BAD_ARGUMENT;
    }
    for (hop = 0;; ++hop)
    {
        const int code = platform->fetch(platform->user, url, answer, capacity, length, &status);
        char next[256];
        report->http_status = status;
        if (code == SELF_UPDATE_FETCH_TOO_LARGE || (code == 0 && *length > capacity))
            return GITHUB_RELEASE_TOO_LARGE;
        if (code != 0)
        {
            report->platform_error = code;
            return GITHUB_RELEASE_NETWORK;
        }
        if (status != 301 && status != 302 && status != 307 && status != 308)
            break;
        /* A renamed repository: GitHub's redirect names its new address in its body as well as in
         * its Location header, and the kit's fetch gives the body. Followed to that address alone
         * (github_release_api_url_allowed). */
        if (hop >= GITHUB_RELEASE_REDIRECTS || string_member(answer, *length, "url", next, sizeof(next)) != 1 ||
            !github_release_api_url_allowed(next))
            return GITHUB_RELEASE_REDIRECT_REFUSED;
        memcpy(url, next, sizeof(url));
        report->redirects = hop + 1;
    }
    if (status == 404)
        return GITHUB_RELEASE_NONE;
    /* The rate limit for requests without an account: 403 with no requests left, or 429. */
    if (status == 403 || status == 429)
        return GITHUB_RELEASE_RATE_LIMITED;
    if (status != 200)
        return GITHUB_RELEASE_HTTP_STATUS;
    return GITHUB_RELEASE_NEWER;
}

static github_release_reason fetch_latest(const self_update_platform *platform, const char *repository,
                                          char *answer, size_t capacity, size_t *length,
                                          github_release_report *report)
{
    return fetch_release(platform, repository, NULL, answer, capacity, length, report);
}

self_update_check_result github_release_check(const self_update_platform *platform,
                                              const char *repository, const char *name,
                                              const char *title_id, const char *installed,
                                              self_update_offer *offer,
                                              github_release_report *report)
{
    /* One check at a time: the buffers are shared (the release holds 16 KiB of notes). */
    static char answer[GITHUB_RELEASE_MAX_ANSWER];
    static github_release release;
    github_release_report unused;
    char url[256];
    char sums_url[512];
    char sums[GITHUB_RELEASE_MAX_SUMS];
    sums_sink sink;
    size_t length = 0;
    github_release_reason fetched;
    if (report == NULL)
        report = &unused;
    memset(report, 0, sizeof(*report));
    if (offer == NULL)
        return SELF_UPDATE_UNKNOWN;
    /* update_check_url also refuses anything that isn't a title ID. */
    if (platform == NULL || !is_file_name(name, 40) || update_check_url(url, sizeof(url), title_id) == 0 ||
        !update_check_version_parse(installed, NULL) || github_release_url(url, sizeof(url), repository) == 0)
        return no_offer(offer, report, GITHUB_RELEASE_BAD_ARGUMENT);

    if ((fetched = fetch_latest(platform, repository, answer, sizeof(answer), &length, report)) !=
        GITHUB_RELEASE_NEWER)
        return no_offer(offer, report, fetched);
    report->reason = github_release_evaluate(answer, length, installed, &release);
    log_text(report->tag, sizeof(report->tag), release.tag);
    if (release.zip_count == 1)
        log_text(report->zip, sizeof(report->zip), release.zips[0]);
    if (report->reason != GITHUB_RELEASE_NEWER)
        return no_offer(offer, report, report->reason);

    /* SHA256SUMS comes the way the ZIP will: from github.com, by way of its file host. The
     * addresses are made from the repository and the tag, not taken from the answer, and are ones
     * a download may be made from (self_update_url_allowed); github.com sends a renamed
     * repository's on to its new name. */
    memset(offer, 0, sizeof(*offer));
    if (github_release_file_url(sums_url, sizeof(sums_url), repository, release.tag, "SHA256SUMS") == 0 ||
        !self_update_url_allowed(sums_url, 0))
        return no_offer(offer, report, GITHUB_RELEASE_BAD_ARGUMENT);
    sink.body = sums;
    sink.capacity = sizeof(sums);
    sink.used = 0;
    if (platform->download(platform->user, sums_url, sizeof(sums), on_sums, &sink) != 0)
        return no_offer(offer, report, GITHUB_RELEASE_SUMS_UNREADABLE);
    report->reason = github_release_choose(&release, sums, sink.used, offer->sha256);
    if (report->reason != GITHUB_RELEASE_NEWER)
        return no_offer(offer, report, report->reason);
    log_text(report->zip, sizeof(report->zip), release.zip);
    if (github_release_file_url(offer->artifact, sizeof(offer->artifact), repository, release.tag,
                                release.zip) == 0 ||
        !self_update_url_allowed(offer->artifact, 0))
        return no_offer(offer, report, GITHUB_RELEASE_BAD_ARGUMENT);

    (void)snprintf(offer->title, sizeof(offer->title), "%s", title_id);
    (void)snprintf(offer->name, sizeof(offer->name), "%s", name);
    (void)snprintf(offer->installed, sizeof(offer->installed), "%s", installed);
    (void)snprintf(offer->available, sizeof(offer->available), "%s", release.available);
    (void)snprintf(offer->version, sizeof(offer->version), "%s", release.tag);
    (void)snprintf(offer->page, sizeof(offer->page), "https://github.com/%s/releases/tag/%s", repository,
                   release.tag);
    offer->size = release.zip_size;
    (void)snprintf(offer->notes, sizeof(offer->notes), "%s", release.notes);
    offer->notes_truncated = release.notes_truncated;
    report->reason = GITHUB_RELEASE_NEWER;
    return SELF_UPDATE_AVAILABLE;
}

self_update_check_result self_update_check_from(const self_update_platform *platform,
                                                const char *repository, const char *name,
                                                const char *title_id, const char *installed,
                                                self_update_offer *offer,
                                                github_release_report *report)
{
    if (report != NULL)
        memset(report, 0, sizeof(*report));
    if (repository == NULL || repository[0] == '\0')
        return self_update_check(platform, title_id, installed, offer);
    return github_release_check(platform, repository, name, title_id, installed, offer, report);
}

/* ---- Another app's release ----------------------------------------------------------------- */

static github_release_reason asset_reason(github_release_report *report, github_release_reason reason)
{
    report->reason = reason;
    return reason;
}

/* Whether a release's answer is of a repository of `repository`'s owner: its "url" is
 * https://api.github.com/repos/<owner>/<name>/releases/<id>, with GitHub's own spelling of the
 * owner (an account's name is the same whatever its letters' case). */
static int same_owner(const char *json, size_t length, const char *repository)
{
    static const char start[] = GITHUB_RELEASE_API;
    char url[256];
    const char *owner;
    const size_t wanted = (size_t)(strchr(repository, '/') - repository);
    size_t i;
    if (string_member(json, length, "url", url, sizeof(url)) != 1 || strncmp(url, start, sizeof(start) - 1) != 0)
        return 0;
    owner = url + sizeof(start) - 1;
    if (strlen(owner) <= wanted || owner[wanted] != '/')
        return 0;
    for (i = 0; i < wanted; ++i)
    {
        char a = owner[i];
        char b = repository[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

/* "sha256:<64 hex>", GitHub's digest of a file, as 64 lowercase characters. 1: it is one. */
static int sha256_digest(const char *text, char out[65])
{
    size_t i;
    if (strncmp(text, "sha256:", 7) != 0 || strlen(text) != 7 + 64)
        return 0;
    for (i = 0; i < 64; ++i)
    {
        const int value = hex_value(text[7 + i]);
        if (value < 0)
            return 0;
        out[i] = "0123456789abcdef"[value];
    }
    out[64] = '\0';
    return 1;
}

/* github_release_asset_check, of the newest release (tag NULL) or of the one `tag` names. */
static github_release_reason asset_check(const self_update_platform *platform, const char *repository,
                                         const char *tag, const char *file, github_release_asset *asset,
                                         github_release_report *report)
{
    github_release_report unused;
    char *answer;
    const char *assets = NULL;
    size_t assets_length = 0;
    size_t cursor = 0;
    size_t length = 0;
    char digest[65] = {0};
    char sums_url[512];
    char sums[GITHUB_RELEASE_MAX_SUMS];
    sums_sink sink;
    unsigned sums_size = 0;
    int sums_files = 0;
    int named = 0;
    int found;
    github_release_reason reason;
    if (report == NULL)
        report = &unused;
    memset(report, 0, sizeof(*report));
    if (asset == NULL)
        return GITHUB_RELEASE_BAD_ARGUMENT;
    memset(asset, 0, sizeof(*asset));
    if (platform == NULL || !is_file_name(file, 64) || !github_release_repository_valid(repository) ||
        (tag != NULL && !is_file_name(tag, 39)))
        return asset_reason(report, GITHUB_RELEASE_BAD_ARGUMENT);
    /* Its own memory: the app's update check can be asking at the same time. */
    answer = (char *)malloc(GITHUB_RELEASE_MAX_ANSWER);
    if (answer == NULL)
        return asset_reason(report, GITHUB_RELEASE_NETWORK);
    reason = fetch_release(platform, repository, tag, answer, GITHUB_RELEASE_MAX_ANSWER, &length, report);
    /* A renamed repository is followed to its number; where that is now must be the same owner's. */
    if (reason == GITHUB_RELEASE_NEWER && report->redirects > 0 && !same_owner(answer, length, repository))
        reason = GITHUB_RELEASE_OWNER_CHANGED;
    /* As github_release_evaluate reads it: one whole object, of a release that is published. */
    if (reason == GITHUB_RELEASE_NEWER &&
        update_check_json_value(answer, length, "assets", &assets, &assets_length) != 1)
        reason = GITHUB_RELEASE_BAD_RESPONSE;
    /* The newest release is never a pre-release; one asked for by its tag may be (a release is
     * published as one first, .github/workflows/release.yml). */
    if (reason == GITHUB_RELEASE_NEWER && (reason = published(answer, length, "draft")) == GITHUB_RELEASE_NEWER &&
        tag == NULL)
        reason = published(answer, length, "prerelease");
    if (reason == GITHUB_RELEASE_NEWER &&
        (found = string_member(answer, length, "tag_name", asset->tag, sizeof(asset->tag))) != 1)
        reason = found == -2 ? GITHUB_RELEASE_BAD_RESPONSE : GITHUB_RELEASE_BAD_TAG;
    /* The release asked for, and no other. */
    if (reason == GITHUB_RELEASE_NEWER && tag != NULL && strcmp(asset->tag, tag) != 0)
        reason = GITHUB_RELEASE_BAD_RESPONSE;
    /* The tag goes into the download's address: letters, digits, '.', '_' and '-'. */
    if (reason == GITHUB_RELEASE_NEWER && !is_file_name(asset->tag, sizeof(asset->tag)))
        reason = GITHUB_RELEASE_BAD_TAG;
    if (reason == GITHUB_RELEASE_NEWER)
        log_text(report->tag, sizeof(report->tag), asset->tag);

    /* Its files, read once: the one asked for and SHA256SUMS, each uploaded in full. */
    while (reason == GITHUB_RELEASE_NEWER)
    {
        const char *item = NULL;
        size_t item_length = 0;
        char name[128];
        char upload[16];
        char text[80];
        unsigned bytes = 0;
        found = update_check_json_array_next(assets, assets_length, &cursor, &item, &item_length);
        if (found == 0)
            break;
        if (found != 1 || (found = string_member(item, item_length, "name", name, sizeof(name))) == -2)
        {
            reason = GITHUB_RELEASE_BAD_RESPONSE;
            break;
        }
        if (found != 1 || (strcmp(name, file) != 0 && strcmp(name, "SHA256SUMS") != 0))
            continue;
        if ((found = string_member(item, item_length, "state", upload, sizeof(upload))) == -2)
        {
            reason = GITHUB_RELEASE_BAD_RESPONSE;
            break;
        }
        if (found != 1 || strcmp(upload, "uploaded") != 0 ||
            update_check_json_uint(item, item_length, "size", &bytes) != 1)
            continue;
        if (strcmp(name, "SHA256SUMS") == 0)
        {
            ++sums_files;
            sums_size = bytes;
            continue;
        }
        if (bytes == 0 || bytes > SELF_UPDATE_MAX_ARCHIVE)
            continue;
        ++named;
        asset->size = bytes;
        /* null for a file uploaded before GitHub kept digests; another kind of digest is none. */
        if ((found = string_member(item, item_length, "digest", text, sizeof(text))) == -2)
        {
            reason = GITHUB_RELEASE_BAD_RESPONSE;
            break;
        }
        if (found != 1 || !sha256_digest(text, digest))
            digest[0] = '\0';
    }
    free(answer);
    if (reason != GITHUB_RELEASE_NEWER)
        return asset_reason(report, reason);
    /* GitHub names a release's files once; an answer that lists one twice says two things. */
    if (named > 1 || sums_files > 1)
        return asset_reason(report, GITHUB_RELEASE_BAD_RESPONSE);
    if (named == 0)
        return asset_reason(report, GITHUB_RELEASE_NO_ZIP);
    log_text(report->zip, sizeof(report->zip), file);

    if (sums_files == 1)
    {
        /* From github.com, as the file will be: its address made, not taken from the answer. */
        if (sums_size == 0 || sums_size > GITHUB_RELEASE_MAX_SUMS)
            return asset_reason(report, GITHUB_RELEASE_SUMS_MALFORMED);
        found = snprintf(sums_url, sizeof(sums_url), "https://github.com/%s/releases/download/%s/SHA256SUMS",
                         repository, asset->tag);
        if (found <= 0 || (size_t)found >= sizeof(sums_url) || !self_update_url_allowed(sums_url, 0))
            return asset_reason(report, GITHUB_RELEASE_BAD_TAG);
        sink.body = sums;
        sink.capacity = sizeof(sums);
        sink.used = 0;
        if (platform->download(platform->user, sums_url, sizeof(sums), on_sums, &sink) != 0)
            return asset_reason(report, GITHUB_RELEASE_SUMS_UNREADABLE);
        found = github_release_sums(sums, sink.used, file, asset->sha256);
        if (found == 0)
            return asset_reason(report, GITHUB_RELEASE_SUMS_NO_LINE);
        if (found < 0)
            return asset_reason(report, GITHUB_RELEASE_SUMS_MALFORMED);
        if (digest[0] != '\0' && strcmp(digest, asset->sha256) != 0)
        {
            asset->sha256[0] = '\0';
            return asset_reason(report, GITHUB_RELEASE_HASHES_DIFFER);
        }
        asset->from_sums = 1;
    }
    else if (digest[0] != '\0')
        memcpy(asset->sha256, digest, sizeof(digest));
    else
        return asset_reason(report, GITHUB_RELEASE_NO_HASH);

    found = snprintf(asset->url, sizeof(asset->url), "https://github.com/%s/releases/download/%s/%s", repository,
                     asset->tag, file);
    if (found <= 0 || (size_t)found >= sizeof(asset->url) || !self_update_url_allowed(asset->url, 0))
    {
        memset(asset, 0, sizeof(*asset));
        return asset_reason(report, GITHUB_RELEASE_BAD_TAG);
    }
    (void)snprintf(asset->page, sizeof(asset->page), "https://github.com/%s/releases/tag/%s", repository, asset->tag);
    return asset_reason(report, GITHUB_RELEASE_NEWER);
}

github_release_reason github_release_asset_check(const self_update_platform *platform,
                                                 const char *repository, const char *file,
                                                 github_release_asset *asset,
                                                 github_release_report *report)
{
    return asset_check(platform, repository, NULL, file, asset, report);
}

github_release_reason github_release_tag_asset_check(const self_update_platform *platform,
                                                     const char *repository, const char *tag,
                                                     const char *file, github_release_asset *asset,
                                                     github_release_report *report)
{
    return asset_check(platform, repository, tag, file, asset, report);
}
