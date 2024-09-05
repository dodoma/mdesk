#include <reef.h>

#include "rpi.h"

const char* rpiReadID()
{
    static char output[14] = {0};

    FILE *fp = fopen("/sys/firmware/devicetree/base/serial-number", "r");
    if (fp) {
        char sn[64] = {0}, buf[128] = {0}, ids[13] = {0};
        unsigned char sum[16] = {0};
        fread(sn, 1, sizeof(sn), fp);
        fclose(fp);

        snprintf(buf, sizeof(buf), "RPI%savm", sn);
        mhash_md5_buf((unsigned char*)buf, strlen(buf), sum);
        mstr_bin2hexstr(sum, 6, ids);
        mstr_tolower(ids);
        snprintf(output, sizeof(output), "a%s", ids);
    } else sprintf(output, "unknownID");

    mtc_mt_dbg("CPU ID %s", output);

    return output;
}
