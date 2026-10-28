/*
 * Nightshade - The launch payload: opens another app for Nightshade's Apps (headless/apps.h).
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Based on the Game Forwarder's launch payload of Mihawk's PS5 RetroArch
 * (https://github.com/mihawk-99/PS5_RetroArch, forwarder/launch.c, Copyright (C) 2026 Mihawk,
 * GPL-3.0-or-later): the wait for the calling app to close and the two launch calls are his.
 *
 * An app cannot start another title from its sandbox (sceSystemServiceLaunchApp answers it
 * 0x8094000f); a payload can. Nightshade sends this program to the console's payload loader
 * (loopback port 9021) as it sends its self-update helper: the loader gives it the rest of the
 * connection as its standard input and output, where Nightshade says which title to open and
 * which one is asking. Lines end with '\n':
 *
 *   app      PSL1, <title ID to open>, <title ID of the app asking>      (three lines)
 *   payload  waiting                              or  fail <reason>
 *
 * Nightshade then closes. The payload waits for it to stop being the app that runs (up to 30 s:
 * Nightshade gives its graphics and sound back first), then opens the title as the foreground
 * user with sceLncUtilLaunchApp and, when that refuses, sceSystemServiceLaunchApp. Each step is
 * written to /data/prosperoeden/logs/app-launch.log.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LAUNCH_MAGIC "PSL1"
#define LAUNCH_LOG "/data/prosperoeden/logs/app-launch.log"
#define LAUNCH_WAIT_STEP_US 250000
#define LAUNCH_WAIT_STEPS 120

typedef struct
{
    uint32_t size;
    int user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint64_t check_flag;
} lnc_app_param_t;

int sceSystemServiceLaunchApp(const char *title_id, const char *argv[], lnc_app_param_t *param);
int sceLncUtilLaunchApp(const char *title_id, const char *argv[], lnc_app_param_t *param);
uint32_t sceLncUtilGetAppIdOfRunningBigApp(void);
int sceLncUtilGetAppTitleId(uint32_t app_id, char *title_id);
int sceUserServiceInitialize(void *param);
int sceUserServiceGetForegroundUser(int *user_id);

/* Four capitals and five digits (PPSA99003). */
static int title_id(const char *text)
{
    size_t i;
    if (strlen(text) != 9)
        return 0;
    for (i = 0; i < 9; i++)
        if (i < 4 ? !(text[i] >= 'A' && text[i] <= 'Z') : !(text[i] >= '0' && text[i] <= '9'))
            return 0;
    return 1;
}

/* One line from the app, without its end. 0: the connection ended or the line is too long. */
static int read_line(char *line, size_t size)
{
    size_t used = 0;
    for (;;)
    {
        char byte = 0;
        if (read(STDIN_FILENO, &byte, 1) != 1)
            return 0;
        if (byte == '\n')
        {
            line[used] = '\0';
            return 1;
        }
        if (used + 1 >= size)
            return 0;
        line[used++] = byte;
    }
}

static void say(const char *line)
{
    const size_t length = strlen(line);
    size_t done = 0;
    while (done < length)
    {
        const ssize_t count = write(STDOUT_FILENO, line + done, length - done);
        if (count <= 0)
            return;
        done += (size_t)count;
    }
}

/* The running big app's title id, or "" when none runs (as Mihawk's payload reads it). */
static void running_title(char title[16])
{
    const uint32_t app = sceLncUtilGetAppIdOfRunningBigApp();
    memset(title, 0, 16);
    if (app != 0 && app != 0xffffffffu && sceLncUtilGetAppTitleId(app, title) != 0)
        memset(title, 0, 16);
    title[15] = '\0';
}

/* Whether the console lists the title: what it has of an app it registered (ShadowMountPlus's
 * record is in /user/app/<title>, the home screen's artwork in /user/appmeta/<title>). A new app
 * the homebrew loader has not listed yet has neither, and opening it could only fail after
 * Nightshade has closed. */
static int registered(const char *title)
{
    char path[64];
    struct stat info;
    (void)snprintf(path, sizeof(path), "/user/app/%s", title);
    if (stat(path, &info) == 0 && S_ISDIR(info.st_mode))
        return 1;
    (void)snprintf(path, sizeof(path), "/user/appmeta/%s", title);
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

int main(void)
{
    char magic[16];
    char target[16];
    char asking[16];
    char title[16];
    const char *arguments[] = {NULL};
    FILE *log;
    int init;
    int foreground;
    int user = -1;
    int waited = 0;
    int result;
    lnc_app_param_t param;
    /* The app closes its end once it has the answer. */
    (void)signal(SIGPIPE, SIG_IGN);
    if (!read_line(magic, sizeof(magic)) || strcmp(magic, LAUNCH_MAGIC) != 0 || !read_line(target, sizeof(target)) ||
        !read_line(asking, sizeof(asking)) || !title_id(target) || !title_id(asking) || strcmp(target, asking) == 0)
    {
        say("fail The request was refused\n");
        return 1;
    }
    log = fopen(LAUNCH_LOG, "w");
    if (!registered(target))
    {
        if (log)
        {
            fprintf(log, "launch %s for %s: not registered (no /user/app or /user/appmeta folder); nothing launched\n",
                    target, asking);
            fclose(log);
        }
        say("fail The console doesn't list the app yet\n");
        return 1;
    }
    say("waiting\n");
    init = sceUserServiceInitialize(NULL);
    foreground = sceUserServiceGetForegroundUser(&user);
    for (running_title(title); strcmp(title, asking) == 0 && waited < LAUNCH_WAIT_STEPS; waited++)
    {
        usleep(LAUNCH_WAIT_STEP_US);
        running_title(title);
    }
    if (log)
    {
        fprintf(log,
                "launch %s for %s: user service %08x, foreground user %d (%08x); waited %d step(s) for %s, "
                "running now: '%s'\n",
                target, asking, (unsigned)init, user, (unsigned)foreground, waited, asking, title);
        fflush(log);
    }
    param.size = sizeof(param);
    param.user_id = user;
    param.app_opt = 0;
    param.crash_report = 0;
    param.check_flag = 0;
    result = sceLncUtilLaunchApp(target, arguments, &param);
    if (log)
        fprintf(log, "launch %s: sceLncUtilLaunchApp = %08x\n", target, (unsigned)result);
    if (result < 0)
    {
        result = sceSystemServiceLaunchApp(target, arguments, &param);
        if (log)
            fprintf(log, "launch %s: sceSystemServiceLaunchApp = %08x\n", target, (unsigned)result);
    }
    if (log)
        fclose(log);
    return result < 0;
}
