#pragma once
/* /usr/local holds software that the package installer does not manage.
 * Until the development disk of docs/plan/packaging.md (P8), it is also a
 * symbolic link on the root image to /home/.local on the data volume,
 * which holds the account databases and the state that survives a
 * rebuild of the root image (docs/design/users.md). */
#define LOCAL_PREFIX "/usr/local"
#define LOCAL_BIN LOCAL_PREFIX "/bin"
#define LOCAL_LIB LOCAL_PREFIX "/lib"
#define LOCAL_SHARE LOCAL_PREFIX "/share"

/* The records of the package installer and the launcher and MIME tables
 * it writes from them (docs/design/packages.md). */
#define PKG_DB "/var/lib/pkg"
#define PKG_LAUNCHER PKG_DB "/launcher"
#define PKG_MIME_TYPES PKG_DB "/mime.types"
#define PKG_MIME_APPS PKG_DB "/mime.apps"
