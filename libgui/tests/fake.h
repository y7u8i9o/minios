#pragma once
#include <gui/gfx.h>
#include <gui/client.h>
extern struct rect fake_last_damage;
extern int fake_damage_count;
void fake_push(const struct wmsg *m);
