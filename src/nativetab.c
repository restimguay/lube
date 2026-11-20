/*
 * nativetab.c - the table of functions compiled to machine code. A plain
 * lube has none; lube --build --native compiles its own table (the C that
 * native.c generates) into the executable in place of this file.
 */
#include "lube.h"

const LubeNative lube_natives[1] = {{NULL, NULL, 0, NULL, NULL, NULL}};
const int lube_nnatives = 0;
