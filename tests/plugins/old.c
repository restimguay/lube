/* old.c - a plugin made for a later Lube than this one (tests/cli.sh) */
#include "../../src/lube_plugin.h"

LUBE_PLUGIN(old) {
    (void)plugin;
    (void)api;
    return LUBE_PLUGIN_API + 1;
}
