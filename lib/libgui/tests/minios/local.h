#pragma once
/* The paths of lib/libc/include/minios/local.h for the host tests. */
#define LOCAL_PREFIX "/usr/local"
#define LOCAL_BIN LOCAL_PREFIX "/bin"
#define LOCAL_LIB LOCAL_PREFIX "/lib"
#define LOCAL_SHARE LOCAL_PREFIX "/share"
/* The tables of packages are fixtures of the tests (test_appchooser.c). */
#define PKG_DB "/tmp/minios_test_pkg"
#define PKG_LAUNCHER PKG_DB "/launcher"
#define PKG_MIME_TYPES PKG_DB "/mime.types"
#define PKG_MIME_APPS PKG_DB "/mime.apps"
