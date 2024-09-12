typedef struct {
    BeeEntry base;
} HardwareEntry;

bool hdw_process(BeeEntry *be, QueueEntry *qe)
{
    HardwareEntry *me = (HardwareEntry*)be;

    mtc_mt_dbg("process command %d", qe->command);

    return true;
}

void hdw_stop(BeeEntry *be)
{
    mtc_mt_dbg("stop worker %s", be->name);
}

BeeEntry* _start_hardware()
{
    HardwareEntry *me = mos_calloc(1, sizeof(HardwareEntry));

    me->base.process = hdw_process;
    me->base.stop = hdw_stop;

    return (BeeEntry*)me;
}

BeeDriver hardware_driver = {
    .id = FRAME_HARDWARE,
    .name = "hardware",
    .init_driver = _start_hardware
};
