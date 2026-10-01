#pragma once

/* The hung task detector (docs/design/debug.md). */

/* Start the hungd thread. hung_task=SECONDS on the command line sets the
 * limit of a bounded wait (30 by default, 0 disables the reports). */
void hung_start_daemon(void);
/* Print the thread table on the console from hungd, within a second.
 * Called by the input core for Alt+SysRq, also from interrupts. */
void hung_request_dump(void);
