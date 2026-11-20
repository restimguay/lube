/* broken.c - a plugin that cannot start (tests/cli.sh) */
#include "../../src/lube_plugin.h"

LUBE_PLUGIN(broken) {
    (void)plugin;
    (void)api;
    return -1;
}
