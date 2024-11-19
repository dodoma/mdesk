void onUstickMount(char *name)
{
    BeeEntry *be = beeFind(FRAME_AUDIO);
    if (!be) {
        mtc_mt_warn("can't find audio backend");
        return;
    }

    uint8_t bufsend[LEN_IDIOT];
    packetIdiotFill(bufsend, IDIOT_USTICK_MOUNT);

    NetClientNode *client;
    MLIST_ITERATE(be->users, client) {
        if (!client->base.dropped) SSEND(client->base.fd, bufsend, LEN_IDIOT);
    }
}
