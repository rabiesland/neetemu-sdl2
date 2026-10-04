//
//  compat.c
//  
//
//  Created by grace <3 on 10/4/26.
//
//

#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "compat.h"

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

int clockGetTime(int var, struct timespec *ts) {
#ifdef __MACH__
    if (var == clockMonotonic) { //https://gist.github.com/jbenet/1087739
        clock_serv_t cclock;
        mach_timespec_t mts;
        host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
        clock_get_time(cclock, &mts);
        mach_port_deallocate(mach_task_self(), cclock);
        ts->tv_sec = mts.tv_sec;
        ts->tv_nsec = mts.tv_nsec;
    } else {
        clock_serv_t cclock;
        mach_timespec_t mts;
        host_get_clock_service(mach_host_self(), SYSTEM_CLOCK, &cclock);
        clock_get_time(cclock, &mts);
        mach_port_deallocate(mach_task_self(), cclock);
        ts->tv_sec = mts.tv_sec;
        ts->tv_nsec = mts.tv_nsec;
    }
#else
    if (var == clockMonotonic) {
        clock_gettime(CLOCK_MONOTONIC,ts);
    } else {
        clock_gettime(CLOCK_REALTIME,ts);
    }
#endif
}
