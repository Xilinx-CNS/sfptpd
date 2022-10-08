/*-
 * Copyright (c) 2019      Xilinx, Inc.
 * Copyright (c) 2014-2018 Solarflare Communications Inc.
 * Copyright (c) 2013      Harlan Stenn,
 *                         George N. Neville-Neil,
 *                         Wojciech Owczarek
 *                         Solarflare Communications Inc.
 * Copyright (c) 2011-2012 George V. Neville-Neil,
 *                         Steven Kreuzer, 
 *                         Martin Burnicki, 
 *                         Jan Breuer,
 *                         Wojciech Owczarek,
 *                         Gael Mace, 
 *                         Alexandre Van Kempen,
 *                         Inaqui Delgado,
 *                         Rick Ratzel,
 *                         National Instruments.
 *                         Solarflare Communications Inc.
 * Copyright (c) 2009-2010 George V. Neville-Neil, 
 *                         Steven Kreuzer, 
 *                         Martin Burnicki, 
 *                         Jan Breuer,
 *                         Gael Mace, 
 *                         Alexandre Van Kempen
 *
 * Copyright (c) 2005-2008 Kendall Correll, Aidan Williams
 *
 * All Rights Reserved
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file   timer.c
 * @date   Wed Jun 23 09:41:26 2010
 *
 * @brief  The timers which run the state machine.
 *
 * Timers in the PTP daemon are run off of the signal system.
 */

#include "../ptpd.h"

#define US_TIMER_INTERVAL (62500)

uint32_t timer_ticks;

/*
 * original code calls sigalarm every fixed 1ms. This highly pollutes the debug_log, and causes more interrupted instructions
 * This was later modified to have a fixed granularity of 1s.
 *
 * Currently this has a configured granularity, and timerStart() guarantees that clocks expire ASAP when the granularity is too small.
 * Timers must now be explicitelly canceled with timerStop (instead of timerStart(0.0))
 */

void
initTimer(void)
{
	DBG("initTimer\n");

	timer_ticks = 0;
}

void
timerTick(IntervalTimer * itimer)
{
	int i;
	const int32_t delta = 1;

	timer_ticks += delta;

	/*
	 * if time actually passed, then decrease every timer left
	 * the one(s) that went to zero or negative are:
	 *  a) rearmed at the original time (ignoring the time that may have passed ahead)
	 *  b) have their expiration latched until timerExpired() is called
	 */
	for (i = 0; i < TIMER_ARRAY_SIZE; ++i) {
		if ((itimer[i].interval) > 0 && ((itimer[i].left) -= delta) <= 0) {
			itimer[i].left = itimer[i].interval;
			itimer[i].expire = TRUE;
			DBG2("TimerUpdate:    Timer %u has now expired.   (Re-armed again with interval %d, left %d)\n", i, itimer[i].interval, itimer[i].left );
		}
	}
}

void
timerStop(UInteger16 index, IntervalTimer * itimer)
{
	if (index >= TIMER_ARRAY_SIZE)
		return;

	itimer[index].interval = 0;
	DBG2("timerStop:      Stopping timer %d.   (New interval: %d; New left: %d)\n", index, itimer[index].left , itimer[index].interval);
}

void
timerStart(UInteger16 index, long double interval, IntervalTimer * itimer)
{
	if (index >= TIMER_ARRAY_SIZE)
		return;

	itimer[index].expire = FALSE;

	/*
	 *  US_TIMER_INTERVAL defines the minimum interval between sigalarms.
	 *  timerStart has a float parameter for the interval, which is casted to integer.
	 *  very small amounts are forced to expire ASAP by setting the interval to 1
	 */
	itimer[index].left = (int)((interval * 1E6) / US_TIMER_INTERVAL);
	if(itimer[index].left == 0){
		/* Using random uniform timers it is pratically guaranteed that
		 * we hit the possible minimum timer. This is because the
		 * current timer model based on a period alarm, irrespective of
		 * whether the next event is close or far away in time.
		 * Having events that expire immediately (ie, delayreq 
		 * invocations using random timers) can lead to messages
		 * appearing in unexpected ordering, so the protocol
		 * implementation must check more conditions and not assume a
		 * certain ususal ordering.
		 * Therefore we do not allow this.
		 *
		 * the interval is too small, raise it to 1 to make sure it expires ASAP
		 * Timer cancelation is done explicitelly with stopTimer()
		 */
		itimer[index].left = 1;
	}
	itimer[index].interval = itimer[index].left;

	DBG2("timerStart:     Set timer %d to %0.3Lf.  New interval: %d; new left: %d\n", index, interval, itimer[index].left , itimer[index].interval);
}



/*
 * This function arms the timer with a uniform range, as requested by page 105 of the standard (for sending delayReqs.)
 * actual time will be U(0, interval * 2.0);
 *
 * PTPv1 algorithm was:
 *    ptpClock->R = getRand(&ptpClock->random_seed) % (PTP_DELAY_REQ_INTERVAL - 2) + 2;
 *    R is the number of Syncs to be received, before sending a new request
 *
 */
void timerStart_random(UInteger16 index, long double interval, IntervalTimer * itimer)
{
	long double new_value;

	new_value = getRand() * interval * 2.0;
	DBG2(" timerStart_random: requested %0.3Lf, got %0.3Lf\n", interval, new_value);

	timerStart(index, new_value, itimer);
}


Boolean
timerExpired(UInteger16 index, IntervalTimer * itimer)
{
	if (index >= TIMER_ARRAY_SIZE)
		return FALSE;

	if (!itimer[index].expire)
		return FALSE;

	itimer[index].expire = FALSE;


	DBG2("timerExpired:   Timer %d expired, taking actions.   current interval: %d; current left: %d\n", index, itimer[index].left , itimer[index].interval);

	return TRUE;
}


Boolean
timerStopped(UInteger16 index, IntervalTimer * itimer)
{
	if (index >= TIMER_ARRAY_SIZE)
		return FALSE;

	if (itimer[index].interval == 0) {
		return TRUE;
		DBG2("timerStopped:   Timer %d is stopped\n", index);
	}

	return FALSE;
}


Boolean
timerRunning(UInteger16 index, IntervalTimer * itimer)
{
	if (index >= TIMER_ARRAY_SIZE)
		return FALSE;

	if ((itimer[index].interval != 0) &&
	    (itimer[index].expire == FALSE)) {
		return TRUE;
		DBG2("timerRunning:   Timer %d is running\n", index);
	}

	return FALSE;
}

