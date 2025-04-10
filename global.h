#ifndef __GLOBAL_H__
#define __GLOBAL_H__

#include "timer.h"

extern MDF *g_config;
extern MDF *g_runtime;
extern TimerEntry *g_timers;

extern time_t g_ctime;
extern time_t g_starton;
extern time_t g_elapsed;

extern char *g_location;
extern bool  g_log_tostdout;
extern bool  g_dumpsend;
extern bool  g_dumprecv;
extern const char *g_cpuid;
extern int g_efd;

#endif  /* __GLOBAL_H__ */
