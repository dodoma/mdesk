#include <reef.h>

#include "timer.h"

TimerEntry* timerAdd(TimerEntry *entry, int timeout, bool rightnow,
                     void *data, bool (*callback)(void *data))
{
    TimerEntry *e = mos_calloc(1, sizeof(TimerEntry));
    e->timeout = timeout;
    e->right_now = rightnow;
    e->pause = false;
    e->data = data;
    e->callback = callback;
    e->next = entry;

    return e;
}
