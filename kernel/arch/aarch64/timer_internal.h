#pragma once

/* Program the next tick of the calling CPU's virtual timer; called by the
 * interrupt dispatch before the tick handler (gic.c). */
void timer_rearm(void);
