//
//  compat.h
//  
//
//  Created by grace <3 on 10/4/26.
//
//
#ifndef _compat_h
#define _compat_h

#define clockMonotonic 1
#define clockRealtime 2

int clockGetTime(int var, struct timespec *ts);

#endif
