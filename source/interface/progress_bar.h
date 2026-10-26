/*
PROGRESS_BAR.H

header included in hcex build.
*/

#ifndef __PROGRESS_BAR_H
#define __PROGRESS_BAR_H
#pragma once

/* ---------- headers */

#include "cseries/cseries.h"
#include "real_math.h"

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

/* ---------- prototypes/PROGRESS_BAR.C */

void progress_bar_initialize(
	void);
void progress_bar_dispose(
	void);
void progress_bar_begin(
	boolean skip_frame_capture);
void progress_bar_end(
	void);
boolean progress_bar_is_active(
	void);
void progress_bar_enable(
	boolean enabled);
void progress_bar_eachframe(
	void);
void progress_bar_display(
	real progress);
/* The original loading animation. progress runs from 0 to 1 across the sweep. */
void progress_bar_show_ring(
	real progress);
/* One full frame of the opening film, in place of the sweep. rgba is
width*height tightly packed bytes, top row first. opacity 1 replaces the
frame. Below that, the picture is drawn over what is already there. */
void progress_bar_show_picture(
	long width,
	long height,
	void const *rgba,
	real opacity);
boolean progress_bar_is_stuff_ready(
	void);

/* ---------- globals */

/* ---------- public code */

#endif // __PROGRESS_BAR_H
