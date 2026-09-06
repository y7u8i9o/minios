#pragma once
/* The installation prefix of packages (docs/design/packages.md). The
 * data volume, the only persistent storage, is mounted at /home, so the
 * prefix lies below it. The loader (user/ld/ld.c) repeats LOCAL_LIB. */
#define LOCAL_PREFIX "/home/.local"
#define LOCAL_BIN LOCAL_PREFIX "/bin"
#define LOCAL_LIB LOCAL_PREFIX "/lib"
#define LOCAL_SHARE LOCAL_PREFIX "/share"
#define LOCAL_LAUNCHER LOCAL_SHARE "/launcher"
#define LOCAL_MIME_TYPES LOCAL_SHARE "/mime.types"
#define LOCAL_MIME_APPS LOCAL_SHARE "/mime.apps"
