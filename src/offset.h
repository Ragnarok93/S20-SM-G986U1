#ifndef TARGET_CONFIG_H
#error "TARGET_CONFIG_H is not defined; build with make PROJECT=<project>"
#endif

#define _OFFSET_STRINGIFY(x) #x
#define _OFFSET_EXPAND(x) _OFFSET_STRINGIFY(x)
#include _OFFSET_EXPAND(TARGET_CONFIG_H)
