#pragma once

int mount(const char *source, const char *target, const char *fstype);
/* mount with a comma separated option string for the filesystem, such as
 * "uid=1000,gid=1000,umask=077" for FAT (docs/design/users.md). NULL or
 * an empty string means no options. */
int mount_options(const char *source, const char *target, const char *fstype, const char *options);
int umount(const char *target);
