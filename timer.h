#ifndef __TIMER_H__
#define __TIMER_H__

typedef struct _timer_entry {
    int timeout;
    bool right_now;             /* 无需等待timeout, 立即触发，执行后会被置否，需要时请在callback中置真 */
    bool pause;
    void *data;
    bool (*callback)(void *data);

    struct _timer_entry *next;
} TimerEntry;

TimerEntry* timerAdd(TimerEntry *entry, int timeout, bool rightnow,
                     void *data, bool (*callback)(void *data));

#endif  /* __TIMER_H__ */
