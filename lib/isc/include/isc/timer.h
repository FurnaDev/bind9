/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

#pragma once

/*****
***** Module Info
*****/

/*! \file isc/timer.h
 * \brief Provides timers which are event sources in the task system.
 *
 * Three types of timers are supported:
 *
 *\li	'ticker' timers generate a periodic tick event.
 *
 *\li	'once' timers generate an idle timeout event if they are idle for too
 *	long, and generate a life timeout event if their lifetime expires.
 *	They are used to implement both (possibly expiring) idle timers and
 *	'one-shot' timers.
 *
 *\li	'inactive' timers generate no events.
 *
 * Timers can change type.  It is typical to create a timer as
 * an 'inactive' timer and then change it into a 'ticker' or
 * 'once' timer.
 *
 *\li MP:
 *	The module ensures appropriate synchronization of data structures it
 *	creates and manipulates.
 *	Clients of this module must not be holding a timer's task's lock when
 *	making a call that affects that timer.  Failure to follow this rule
 *	can result in deadlock.
 *	The caller must ensure that isc_timermgr_destroy() is called only
 *	once for a given manager.
 *
 * \li Reliability:
 *	No anticipated impact.
 *
 * \li Resources:
 *	TBS
 *
 * \li Security:
 *	No anticipated impact.
 *
 * \li Standards:
 *	None.
 */

/***
 *** Imports
 ***/

#include <stdbool.h>

#include <isc/event.h>
#include <isc/eventclass.h>
#include <isc/lang.h>
#include <isc/time.h>
#include <isc/types.h>

ISC_LANG_BEGINDECLS

/***
 *** Types
 ***/

/*% Timer Type */
typedef enum {
	isc_timertype_undefined = -1, /*%< Undefined */
	isc_timertype_ticker = 0,     /*%< Ticker */
	isc_timertype_once = 1,	      /*%< Once */
	isc_timertype_inactive = 3    /*%< Inactive */
} isc_timertype_t;

typedef struct isc_timerevent {
	struct isc_event common;
	isc_time_t	 due;
} isc_timerevent_t;

#define ISC_TIMEREVENT_FIRSTEVENT (ISC_EVENTCLASS_TIMER + 0)
#define ISC_TIMEREVENT_TICK	  (ISC_EVENTCLASS_TIMER + 1)
#define ISC_TIMEREVENT_IDLE	  (ISC_EVENTCLASS_TIMER + 2)
#define ISC_TIMEREVENT_LIFE	  (ISC_EVENTCLASS_TIMER + 3)
#define ISC_TIMEREVENT_LASTEVENT  (ISC_EVENTCLASS_TIMER + 65535)

/***
 *** Timer and Timer Manager Functions
 ***
 *** Note: all Ensures conditions apply only if the result is success for
 *** those functions which return an isc_result_t.
 ***/

void
isc_timer_create(isc_timermgr_t *manager, isc_task_t *task,
		 isc_taskaction_t action, void *arg, isc_timer_t **timerp);
/*%<
 * Create a new 'type' timer managed by 'manager'.  The timers parameters
 * are specified by 'expires' and 'interval'.  Events will be posted to
 * 'task' and when dispatched 'action' will be called with 'arg' as the
 * arg value.  The new timer is returned in 'timerp'.
 *
 * Notes:
 *
 *\li	For ticker timers, the timer will generate a 'tick' event every
 *	'interval' seconds.  The value of 'expires' is ignored.
 *
 *\li	For once timers, 'expires' specifies the time when a life timeout
 *	event should be generated.  If 'expires' is 0 (the epoch), then no life
 *	timeout will be generated.  'interval' specifies how long the timer
 *	can be idle before it generates an idle timeout.  If 0, then no
 *	idle timeout will be generated.
 *
 *\li	If 'expires' is NULL, the epoch will be used.
 *
 *	If 'interval' is NULL, the zero interval will be used.
 *
 * Requires:
 *
 *\li	'manager' is a valid manager
 *
 *\li	'task' is a valid task
 *
 *\li	'action' is a valid action
 *
 *\li	'expires' points to a valid time, or is NULL.
 *
 *\li	'interval' points to a valid interval, or is NULL.
 *
 *\li	type == isc_timertype_inactive ||
 *	('expires' and 'interval' are not both 0)
 *
 *\li	'timerp' is a valid pointer, and *timerp == NULL
 *
 * Ensures:
 *
 *\li	'*timerp' is attached to the newly created timer
 *
 *\li	The timer is attached to the task
 *
 *\li	An idle timeout will not be generated until at least Now + the
 *	timer's interval if 'timer' is a once timer with a non-zero
 *	interval.
 *
 * Returns:
 *
 *\li	Success
 *\li	No memory
 *\li	Unexpected error
 */

isc_result_t
isc_timer_reset(isc_timer_t *timer, isc_timertype_t type,
		const isc_time_t *expires, const isc_interval_t *interval,
		bool purge);
/*%<
 * Change the timer's type, expires, and interval values to the given
 * values.  If 'purge' is TRUE, any pending events from this timer
 * are purged from its task's event queue.
 *
 * Notes:
 *
 *\li	If 'expires' is NULL, the epoch will be used.
 *
 *\li	If 'interval' is NULL, the zero interval will be used.
 *
 * Requires:
 *
 *\li	'timer' is a valid timer
 *
 *\li	The same requirements that isc_timer_create() imposes on 'type',
 *	'expires' and 'interval' apply.
 *
 * Ensures:
 *
 *\li	An idle timeout will not be generated until at least Now + the
 *	timer's interval if 'timer' is a once timer with a non-zero
 *	interval.
 *
 * Returns:
 *
 *\li	Success
 *\li	No memory
 *\li	Unexpected error
 */

void
isc_timer_attach(isc_timer_t *timer, isc_timer_t **timerp);
/*%<
 * Attach *timerp to timer.
 *
 * Requires:
 *
 *\li	'timer' is a valid timer.
 *
 *\li	'timerp' points to a NULL timer.
 *
 * Ensures:
 *
 *\li	*timerp is attached to timer.
 */

void
isc_timer_detach(isc_timer_t **timerp);
/*%<
 * Detach *timerp from its timer.
 *
 * Requires:
 *
 *\li	'timerp' points to a valid timer.
 *
 * Ensures:
 *
 *\li	*timerp is NULL.
 *
 *\li	If '*timerp' is the last reference to the timer,
 *	then:
 *
 *\code
 *		The timer will be shutdown
 *
 *		The timer will detach from its task
 *
 *		All resources used by the timer have been freed
 *
 *		Any events already posted by the timer will be purged.
 *		Therefore, if isc_timer_detach() is called in the context
 *		of the timer's task, it is guaranteed that no more
 *		timer event callbacks will run after the call.
 *\endcode
 */

isc_timertype_t
isc_timer_gettype(isc_timer_t *timer);
/*%<
 * Return the timer type.
 *
 * Requires:
 *
 *\li	'timer' to be a valid timer.
 */

ISC_LANG_ENDDECLS
