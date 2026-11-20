/* other.c - built as noinit: a library whose entry point has another name (tests/cli.sh) */
#include "../../src/lube_plugin.h"

LUBE_PLUGIN(other) {
    (void)plugin;
    (void)api;
    return LUBE_PLUGIN_API;
}
