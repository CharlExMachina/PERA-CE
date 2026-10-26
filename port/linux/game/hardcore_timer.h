#ifndef HARDCORE_TIMER_H
#define HARDCORE_TIMER_H

/* The match clock. Graphics' Match Timer chooses off, on, or on with
   voice. Voice plays this machine's countdown. A map that has its own
   talking_timer announcer keeps that, and power_call is never forced. */

/* 0 off, 1 on, 2 on with voice. */
int hardcore_timer_mode(void);

/* After the map's scripts have run for this tick. On with Voice turns
   talking_timer on when that global exists. Off and On leave it as the
   map authored it, and this does not force power_call. */
void hardcore_timer_after_scripts(void);

/* The loaded map has a talking_timer announcer, and Voice turned it on.
   Otherwise Voice plays the countdown shipped with the game. */
int hardcore_timer_map_announces(void);

/* The match clock, bottom right of the pane being drawn. A split screen
   draws it on every pane. Nothing is drawn over a menu. */
void hardcore_timer_render(void);

/* The top of the clock's digits in the pane being drawn. The random
   percent uses it so the two sit on one line. */
short hardcore_timer_top(void);

#endif
