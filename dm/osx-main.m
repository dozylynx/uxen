/*
 * Copyright 2012-2016, Bromium, Inc.
 * Author: Michael Dales <michael@digitalflapjack.com>
 * SPDX-License-Identifier: ISC
 */

#include "config.h"

#import <Carbon/Carbon.h>
#include <pthread.h>
#include <libgen.h>
#include <libproc.h>
#include <sys/resource.h>

#import "osx-app-delegate.h"

#include "dm.h"
#include "vm.h"

static pthread_t dm_main_thread = NULL;

#ifdef DEBUG
static void
enable_core_dumps(void)
{
    struct rlimit limit;

    limit.rlim_cur = RLIM_INFINITY;
    limit.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_CORE, &limit);
}
#endif /* DEBUG */

// this is dm_main per -Dmain=dm_main
int main(int argc, char **argv);

@interface UXENThread: NSObject
@property (assign) int argc;
@property (assign) char** argv;
- (void)run: (id)param;
@end

@implementation UXENThread
- (void)run: (id)param
{

    dm_main_thread = pthread_self();
    main(self.argc, self.argv);
}
@end

#undef main
int
main(int argc, char **argv)
{
    char *path;
    int console_headless = 0;

#ifdef DEBUG
    enable_core_dumps();
#endif /* DEBUG */

    path = realpath(argv[0], NULL);
    if (path) {
        char *dir = dirname(path);
        if (dir)
            dm_path = strdup(dir);
        free(path);
        /* do not free dir */
    }

    /*
     * Are we running headless? Do a quick param scan for that.
     *
     * dirty hack to support the legacy --headless option.
     */
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) {
            extern char *console_type;

            argv[i] = "";
            console_headless = 1;
            console_type = "remote";
        }
    }
    if (!console_headless) {
        for (int i = 0; i < (argc - 1); i++) {
            if ((!strcmp(argv[i], "-G") || !strcmp(argv[i], "--gui")) &&
                strcmp(argv[i + 1], "osx"))
                    console_headless = 1;
        }
    }

    /*
     * if we're not headless, call the Carbon API to register this
     * process as a UI process
     */
    if (!console_headless) {
        static const ProcessSerialNumber thePSN = { 0, kCurrentProcess };
        TransformProcessType(&thePSN, kProcessTransformToForegroundApplication);
        SetFrontProcess(&thePSN);
    }

    if (console_headless == 0) {
        // if not headless, create the Cocoa UI shizzle
        @autoreleasepool {
            NSLog(@"kicking off");
            
            // Kick off AppKit
            NSApplication *application = [NSApplication sharedApplication];

            // Build the app kit portion of what we need to do
            UXENAppDelegate *delegate = [[UXENAppDelegate alloc] init];
            [application setDelegate:delegate];


            UXENThread *thread = [[UXENThread alloc] init];
            thread.argc = argc;
            thread.argv = argv;

            [NSThread detachNewThreadSelector:@selector(run:)
                                     toTarget:thread withObject:nil];
            assert([NSThread isMultiThreaded]);

            // now enter the UI section's main loop
            [application run];
        }
    } else {
        // running headless, so just launch straight into uxen
        dm_main_thread = pthread_self();
        dm_main(argc, argv);
    }

    return 0;
}
