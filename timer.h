#ifndef __TIMER_H__
#define __TIMER_H__

typedef struct _timer_entry {
    int timeout;
    bool pause;
    void *data;
    bool (*callback)(void *data);

    struct _timer_entry *next;
} TimerEntry;

TimerEntry* timerAdd(TimerEntry *entry, int timeout, void *data, bool (*callback)(void *data));

#endif  /* __TIMER_H__ */
