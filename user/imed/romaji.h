#pragma once
/* Romaji to hiragana (romaji.c). */
#include <stddef.h>

/* romaji_feed adds the letter c to the pending romaji and appends to kana
 * what it can convert. */
void romaji_feed(char *kana, size_t size, char *roma, size_t rsize, char c);
/* romaji_flush converts the rest at the end of the input: n is ん, and
 * other letters remain as they are. */
void romaji_flush(char *kana, size_t size, char *roma);
