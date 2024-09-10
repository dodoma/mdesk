#include <reef.h>

#include "rpi.h"
#include "net.h"
#include "client.h"
#include "timer.h"

#define DEFAULT_CONFIGFILE "/home/pi/mdesk/config.json"

MDF *g_config = NULL;
TimerEntry *g_timers = NULL;

time_t g_ctime = 0;
time_t g_starton = 0;
time_t g_elapsed = 0;

char *g_location = NULL;
bool  g_log_tostdout = false;
const char *g_cpuid = NULL;
int g_efd = 0;

int main(int argc, char *argv[])
{
    MERR *err;

    mdf_init(&g_config);

    char *filename = argv[1] ? argv[1] : DEFAULT_CONFIGFILE;
    err = mdf_json_import_file(g_config, filename);
    DIE_NOK(err);

    g_location = mdf_get_value(g_config, "location", "./");
    g_log_tostdout = mdf_get_bool_value(g_config, "trace.tostdout", false);
    int loglevel = mtc_level_str2int(mdf_get_value(g_config, "trace.main", "debug"));

    err = mtc_mt_initf("main", loglevel, g_log_tostdout ? "-" : "%s/log/moc.log", g_location);
    DIE_NOK(err);

    if (mdf_get_bool_value(g_config, "server.daemon", false)) {
        pid_t pid = fork();
        if (pid > 0) return 0;
        else if (pid < 0) {
            mtc_mt_err("error in fork()");
            return 1;
        }

        close(0);
        setsid();
    }

    g_ctime = time(NULL);
    g_starton = g_ctime;
    g_elapsed = 0;
    g_cpuid = rpiReadID();

    signal(SIGPIPE, SIG_IGN);

    clientInit();

    err = netExposeME();
    RETURN_V_NOK(err, 1);

    mdf_destroy(&g_config);

    return 0;
}
