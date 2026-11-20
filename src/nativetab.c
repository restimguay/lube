/*
 * nativetab.c - the table of functions compiled to machine code, and of the
 * plugins compiled in. A plain lube has neither (its plugins are files);
 * lube --build --native or --small compiles its own tables (the C that
 * native.c generates) into the executable in place of this file.
 */
#include "lube.h"

const LubeNative lube_natives[1] = {{NULL, NULL, 0, NULL, NULL, NULL}};
const int lube_nnatives = 0;

#ifdef LUBE_STATIC_PLUGINS  /* every plugin that comes with Lube, inside (make STATIC_PLUGINS=1; checking builds) */
int lube_plugin_init_fs(struct LubePlugin *plugin, const struct LubeAPI *api);
int lube_plugin_init_net(struct LubePlugin *plugin, const struct LubeAPI *api);
int lube_plugin_init_audio(struct LubePlugin *plugin, const struct LubeAPI *api);
const LubeStaticPlugin lube_static_plugins[] = {
    {"fs", lube_plugin_init_fs}, {"net", lube_plugin_init_net}, {"audio", lube_plugin_init_audio}, {NULL, NULL}};
#else
const LubeStaticPlugin lube_static_plugins[] = {{NULL, NULL}};
#endif
