/*
 * Copyright 2012-2016, Bromium, Inc.
 * SPDX-License-Identifier: ISC
 */

#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <windows.h>

#if defined(_WIN32)
#define _POSIX
#endif
#include <time.h>
#include <sys/time.h>


FILE *logfile = NULL;

#ifndef PROCESS_MODE_BACKGROUND_BEGIN
#define PROCESS_MODE_BACKGROUND_BEGIN 0x00100000
#endif

int reduce_io_priority(void)
{
    wchar_t *highprio = _wgetenv(L"IMGTOOLS_HIGHPRIO");
    if (highprio && highprio[0] == L'1') {
        fprintf(logfile,"INFO: Running imgtools at default (high) priority.\n");
        return 0;
    } else {
        fprintf(logfile,"INFO: Running imgtools at low priority.\n");
        if (!SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN)) {
            fprintf(logfile,
                    "INFO: Unable to enter Background priority class, error=%u.\n",
                    (uint32_t)GetLastError());
            if (!SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS)) {
                fprintf(logfile,
                        "INFO: Unable to enter Idle priority class, error=%u\n",
                        (uint32_t)GetLastError());
            }
        }
        if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE)) {
            fprintf(logfile,"INFO: Unable to enter Idle thread mode, error=%u\n",
                    (uint32_t)GetLastError());
        }
        return 1;
    }
}

static
void get_tz_offset(char *s, size_t sz, struct tm *tm)
{
#ifdef _WIN32
    LONG log_timezone_bias = 0;
    int is_behind_utc;
    int abs_bias;

    TIME_ZONE_INFORMATION timezone_info;
    switch (GetTimeZoneInformation(&timezone_info)) {
        case TIME_ZONE_ID_UNKNOWN:
            log_timezone_bias = timezone_info.Bias;
            break;
        case TIME_ZONE_ID_STANDARD:
            log_timezone_bias = timezone_info.Bias + timezone_info.StandardBias;
            break;
        case TIME_ZONE_ID_DAYLIGHT:
            log_timezone_bias = timezone_info.Bias + timezone_info.DaylightBias;
            break;
        default:
            log_timezone_bias = 0;
            break;
    }
    is_behind_utc = log_timezone_bias > 0;
    abs_bias = log_timezone_bias < 0 ? -log_timezone_bias : log_timezone_bias;
    sprintf(s, "%c%02u:%02u", is_behind_utc ? '-' : '+',
            abs_bias / 60, abs_bias % 60);
#else
    /* strftime() does not output a : delimiter, so we add that manually. */
    strftime(s, sz, "%z", tm);
    s[5] = s[4];
    s[4] = s[3];
    s[3] = ':';
    s[6] = '\0';
#endif
}


static
void banner(void)
{
    struct tm _tm, *tm;
    time_t ltime;
    struct timeval tv;
    char prefix[sizeof("YYYY-MM-DDThh:mm:ss.fff+ZZ:ZZ ")];
    char tz[sizeof("+ZZ:ZZ")];

    gettimeofday(&tv, NULL);
    ltime = (time_t)tv.tv_sec;
    tm = localtime_r(&ltime, &_tm);
    if (tm) {
        get_tz_offset(tz, sizeof(tz), tm);
        snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03d%s ",
                1900 + tm->tm_year, 1 + tm->tm_mon, tm->tm_mday,
                tm->tm_hour, tm->tm_min, tm->tm_sec,
                (int)(tv.tv_usec / 1000), tz);
        fprintf(logfile, "\n\n%s", prefix);
    }
    fprintf(logfile, "[%ls]\n\n", GetCommandLineW());
}


static void __attribute__((constructor))
disklib_init(void) {
    wchar_t *logname;

    logname = _wgetenv(L"IMGTOOLS_LOGFILE");

    if (logname)
        logfile = _wfopen(logname, L"a");

    if (!logfile)
        logfile = stdout;

    if (logfile != stdout) {
        /* Make stderr go to logfile too. */
        *stderr = *logfile;
    }

    setvbuf(logfile, NULL, _IONBF, 0);
    LoadLibraryA("uxen-backtrace.dll");
    banner();
}

/* Convert args to wide and then to UTF-8. */
void convert_args(int argc, char **argv)
{
    LPWSTR *argv_w;
    int i;
    int argc_w;

    if (!(argv_w = CommandLineToArgvW(GetCommandLineW(), &argc_w))) {
        fprintf(stderr, "CommandLineToArgvW failed\n");
        exit(-1);
    }
    assert(argc_w == argc);

    for (i = 0; i < argc; ++i) {
        /* First figure out buffer size needed and malloc it. */
        int sz;
        char *s;

        sz = WideCharToMultiByte( CP_UTF8, 0, argv_w[i], -1, NULL, 0, NULL, 0);
        if (!(s = (char*) malloc(sz + sizeof(char))))
            exit(-1);

        s[sz] = 0;
        /* Now perform the actual conversion. */
        sz = WideCharToMultiByte( CP_UTF8, 0, argv_w[i], -1, s, sz, NULL, 0);
        argv[i] = s;
    }
    LocalFree(argv_w);
}
