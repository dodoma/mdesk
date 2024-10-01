typedef struct {
    BeeEntry base;
} HardwareEntry;

bool hdw_process(BeeEntry *be, QueueEntry *qe)
{
    //HardwareEntry *me = (HardwareEntry*)be;

    mtc_mt_dbg("process command %d", qe->command);

    switch (qe->command) {
    case CMD_WIFI_SET:
        mtc_mt_dbg("set wifi ... %s", mdf_get_value(qe->nodein, "name", "unknownName"));
        /* TODO business logic */
        MessagePacket *packet = packetMessageInit(qe->client->bufsend, LEN_PACKET_NORMAL);
        size_t sendlen = packetACKFill(packet, qe->seqnum, qe->command, true, NULL);
        packetCRCFill(packet);

        SSEND(qe->client->base.fd, qe->client->bufsend, sendlen);

        break;
    default:
        break;
    }

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
