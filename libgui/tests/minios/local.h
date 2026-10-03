#pragma once
/* The installation prefix of packages (docs/design/packages.md), as in
 * libc/include/minios/local.h. */
#define LOCAL_PREFIX "/usr/local"
#define LOCAL_BIN LOCAL_PREFIX "/bin"
#define LOCAL_LIB LOCAL_PREFIX "/lib"
#define LOCAL_SHARE LOCAL_PREFIX "/share"
#define LOCAL_LAUNCHER LOCAL_SHARE "/launcher"
#define LOCAL_MIME_TYPES LOCAL_SHARE "/mime.types"
#define LOCAL_MIME_APPS LOCAL_SHARE "/mime.apps"
