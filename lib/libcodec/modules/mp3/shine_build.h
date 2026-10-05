#pragma once
/* The settings of the files shine_*.c. Each file compiles one source of
 * the MP3 encoder shine (third_party/shine, GNU Library GPL version 2)
 * into mp3.so. The sources are those of the project with one patch,
 * tools/patches/shine.patch. The headers of shine have no include
 * guards, so each source needs a compilation unit of its own. The sources
 * are compiled without the warnings of minios.
 * Every symbol of shine has hidden visibility, as every symbol of a
 * module has. */
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
