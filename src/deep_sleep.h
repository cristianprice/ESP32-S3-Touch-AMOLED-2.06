#pragma once

/* Enter deep sleep immediately after preserving the RTC-backed clock state. */
void deep_sleep_immediately(void);
/* Create the one-shot inactivity timer used by the main application. */
void sleep_after_timeout(void);
/* Restart inactivity countdown unless it is paused by an active full-screen tool. */
void sleep_timer_reset(void);
/* Suspend/resume inactivity sleep while Motion Detection or Settings is open. */
void sleep_timer_pause(void);
void sleep_timer_resume(void);
