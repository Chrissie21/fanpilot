#ifndef FANPILOT_CONFIG_H
#define FANPILOT_CONFIG_H

#include "policy.h"
#include <CoreFoundation/CoreFoundation.h>

CFPropertyListRef plist_read(const char *path, bool secure) CF_RETURNS_RETAINED;
bool plist_write(const char *path, CFPropertyListRef value, unsigned permissions);
bool config_load(const char *path, Config *config, bool secure);

#endif
