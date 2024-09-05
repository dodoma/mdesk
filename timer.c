#include <reef.h>

#include "timer.h"

TimerEntry* timerAdd(TimerEntry *entry, int timeout, void *data, bool (*callback)(void *data))
{
    TimerEntry *e = mos_calloc(1, sizeof(TimerEntry));
    e->timeout = timeout;
    e->pause = false;
    e->data = data;
    e->callback = callback;
    e->next = entry;

    return e;
}
