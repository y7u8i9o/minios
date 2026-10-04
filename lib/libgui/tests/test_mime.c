/* MIME tables: extension lookup, handler fallbacks, editing and saving. */
#include "check.h"
#include <gui/mime.h>
#include <stdio.h>
#include <string.h>

void run_mime_tests(void)
{
    const char *types = "/tmp/minios_test_mime.types", *apps = "/tmp/minios_test_mime.apps";
    FILE *f = fopen(types, "w");
    fprintf(f, "# comment\ntext/plain txt md\nimage/png png\napplication/x-launcher app\n");
    fclose(f);
    f = fopen(apps, "w");
    fprintf(f, "text/* /bin/gedit\nimage/png /bin/view\n* /bin/hexview\n");
    fclose(f);
    CHECK(mime_load(types, apps) == 0, "mime_load(types, apps) == 0");
    CHECK(strcmp(mime_type("/a/b/notes.TXT", 0), "text/plain") == 0, "strcmp(mime_type(\"/a/b/notes.TXT\", 0), \"text/plain\") == 0");
    CHECK(strcmp(mime_type("photo.png", 0), "image/png") == 0, "strcmp(mime_type(\"photo.png\", 0), \"image/png\") == 0");
    CHECK(strcmp(mime_type("Clock.app", 0), MIME_LAUNCHER) == 0, "strcmp(mime_type(\"Clock.app\", 0), MIME_LAUNCHER) == 0");
    CHECK(strcmp(mime_type(".hidden", 0), "application/octet-stream") == 0, "strcmp(mime_type(\".hidden\", 0), \"application/octet-stream\") == 0");
    CHECK(strcmp(mime_type("dir", 1), MIME_DIRECTORY) == 0, "strcmp(mime_type(\"dir\", 1), MIME_DIRECTORY) == 0");
    CHECK(strcmp(mime_handler("text/plain"), "/bin/gedit") == 0, "strcmp(mime_handler(\"text/plain\"), \"/bin/gedit\") == 0");      /* text/* */
    CHECK(strcmp(mime_handler("image/png"), "/bin/view") == 0, "strcmp(mime_handler(\"image/png\"), \"/bin/view\") == 0");
    CHECK(strcmp(mime_handler("video/x"), "/bin/hexview") == 0, "strcmp(mime_handler(\"video/x\"), \"/bin/hexview\") == 0");        /* * */
    CHECK(strcmp(mime_icon(MIME_DIRECTORY), "folder") == 0, "strcmp(mime_icon(MIME_DIRECTORY), \"folder\") == 0");
    CHECK(strcmp(mime_icon("text/plain"), "edit") == 0, "strcmp(mime_icon(\"text/plain\"), \"edit\") == 0");
    mime_set_handler("image/png", "/bin/paint");
    mime_set_handler("audio/x", "/bin/play");
    CHECK(mime_handler_count() == 4, "mime_handler_count() == 4");
    CHECK(mime_save(NULL) == 0, "mime_save(NULL) == 0");
    CHECK(mime_load(types, apps) == 0, "mime_load(types, apps) == 0");
    CHECK(strcmp(mime_handler("image/png"), "/bin/paint") == 0, "strcmp(mime_handler(\"image/png\"), \"/bin/paint\") == 0");
    CHECK(strcmp(mime_handler("audio/x"), "/bin/play") == 0, "strcmp(mime_handler(\"audio/x\"), \"/bin/play\") == 0");
    remove(types);
    remove(apps);
}
