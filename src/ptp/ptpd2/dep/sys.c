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
* @file   sys.c
* @date   Tue Jul 20 16:19:46 2010
*
* @brief  Code to call kernel time routines and also display server statistics.
*
*
*/

#include "../ptpd.h"

#if defined(linux)
#  include <netinet/ether.h>
#elif defined( __FreeBSD__ )
#  include <net/ethernet.h>
#elif defined( __NetBSD__ )
#  include <net/if_ether.h>
#elif defined( __OpenBSD__ )
#  include <some ether header>  // force build error
#endif


static int
snprint_ClockIdentity(char *s, int max_len, const ClockIdentity id)
{
	int len = snprintf(s, max_len,
			   "%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx",
		           id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
	if (len > max_len) len = max_len;
	return len;
}


/*
 * wrapper that caches the latest value of ether_ntohost
 * this function will NOT check the last accces time of /etc/ethers,
 * so it only have different output on a failover or at restart
 *
 */
static int ether_ntohost_cache(char *hostname, size_t hostname_size,
			       struct ether_addr *addr)
{
	static int valid = 0;
	static struct ether_addr prev_addr;
	static char buf[BUF_SIZE];
 
#if defined(linux) || defined(__NetBSD__)
	if (memcmp(addr->ether_addr_octet, &prev_addr,
		   sizeof(struct ether_addr )) != 0) {
		valid = 0;
	}
#else // e.g. defined(__FreeBSD__)
	if (memcmp(addr->octet, &prev_addr,
		   sizeof(struct ether_addr )) != 0) {
		valid = 0;
	}
#endif

	if (!valid) {
		if(ether_ntohost(buf, addr)){
			snprintf(buf, sizeof(buf), "%s", "unknown");
		}

		/* clean possible commas from the string */
		while (strchr(buf, ',') != NULL) {
			*(strchr(buf, ',')) = '_';
		}

		prev_addr = *addr;
	}

	valid = 1;
	sfptpd_strncpy(hostname, buf, hostname_size);
	return 0;
}


/* Show the hostname configured in /etc/ethers */
static int
snprint_ClockIdentity_ntohost(char *s, int max_len, const ClockIdentity id)
{
	int len = 0;
	int i,j;
	char buf[100];

	struct ether_addr e;

	/* extract mac address */
	for (i = 0, j = 0; i< CLOCK_IDENTITY_LENGTH ; i++ ) {
		/* skip bytes 3 and 4 */
		if (!((i==3) || (i==4))) {
#if defined(linux) || defined(__NetBSD__)
			e.ether_addr_octet[j] = (uint8_t) id[i];
#else // e.g. defined(__FreeBSD__)
			e.octet[j] = (uint8_t) id[i];
#endif
			j++;
		}
	}

	/* convert and print hostname */
	ether_ntohost_cache(buf, sizeof(buf), &e);
	len += snprintf(&s[len], max_len - len, "(%s)", buf);
	if (len > max_len) len = max_len;

	return len;
}


static int
snprint_PortIdentity(char *s, int max_len, const PortIdentity *id)
{
	int len = 0;

	len += snprint_ClockIdentity(&s[len], max_len - len, id->clockIdentity);
	if (len > max_len) len = max_len;

	len += snprint_ClockIdentity_ntohost(&s[len], max_len - len, id->clockIdentity);
	if (len > max_len) len = max_len;

	len += snprintf(&s[len], max_len - len, "/%hx", id->portNumber);
	if (len > max_len) len = max_len;
	return len;
}


/*
* Dumps a data buffer
* This either prints a data buffer to stdout
*/
void
dump(const char *text, void *addr, int len)
{
	uint8_t *address = (uint8_t *)addr;
	int i;
	printf("%s: length %d, data...\n", text, len);
	for (i = 0; i < len; i++) {
		printf("0x%02hhx ", address[i]);
		if ((i % 8) == 7) {
			printf("\n");
		}
	}
	if ((i % 8) != 0) {
		printf("\n");
	}
}


void 
displayStatus(PtpClock *ptpClock, const char *prefixMessage) 
{
	char sbuf[SCREEN_BUFSZ];
	int len = 0;

	len += snprintf(sbuf + len, sizeof(sbuf) - len, "ptp %s: %s",
			ptpClock->rtOpts.name, prefixMessage);
	if (len > sizeof(sbuf)) len = sizeof(sbuf);
	len += snprintf(sbuf + len, sizeof(sbuf) - len, "%s", 
			portState_getName(ptpClock->portState));
	if (len > sizeof(sbuf)) len = sizeof(sbuf);

	if (ptpClock->portState == PTPD_SLAVE ||
	    ptpClock->portState == PTPD_UNCALIBRATED ||
	    ptpClock->portState == PTPD_PASSIVE) {
		len += snprintf(sbuf + len, sizeof(sbuf) - len, ", best master: ");
		if (len > sizeof(sbuf)) len = sizeof(sbuf);
		len += snprint_PortIdentity(sbuf + len, sizeof(sbuf) - len,
					    &ptpClock->parentPortIdentity);
		if (len > sizeof(sbuf)) len = sizeof(sbuf);
	} else if(ptpClock->portState == PTPD_MASTER) {
		len += snprintf(sbuf + len, sizeof(sbuf) - len, " (self)");
		if (len > sizeof(sbuf)) len = sizeof(sbuf);
	}

	len += snprintf(sbuf + len, sizeof(sbuf) - len, "\n");
	NOTICE("ptp %s: %s", ptpClock->rtOpts.name, sbuf);
}


void
displayPortIdentity(PtpClock *ptpClock, PortIdentity *port, const char *prefixMessage)
{
	char sbuf[SCREEN_BUFSZ];
	int len = 0;

	len += snprintf(sbuf + len, sizeof(sbuf) - len, "ptp %s: %s ",
			ptpClock->rtOpts.name, prefixMessage);
	if (len > sizeof(sbuf)) len = sizeof(sbuf);
	len += snprint_PortIdentity(sbuf + len, sizeof(sbuf) - len, port);
	if (len > sizeof(sbuf)) len = sizeof(sbuf);
	len += snprintf(sbuf + len, sizeof(sbuf) - len, "\n");
	INFO("%s",sbuf);
}


void
getTime(struct sfptpd_timespec * time)
{
#if defined(_POSIX_TIMERS) && (_POSIX_TIMERS > 0)

	struct timespec tp;
	if (clock_gettime(CLOCK_REALTIME, &tp) < 0) {
		PERROR("clock_gettime() failed, exiting.");
		exit(0);
	}
	time->sec = tp.tv_sec;
	time->nsec = tp.tv_nsec;
	time->nsec_frac = 0;

#else

	struct timeval tv;
	gettimeofday(&tv, 0);
	time->sec = tv.tv_sec;
	time->nsec = tv.tv_usec * 1000;
	tims->nsec_frac = 0;

#endif /* _POSIX_TIMERS */
}


/* returns a double beween 0.0 and 1.0 */
long double
getRand(void)
{
	return ((rand() * 1.0) / RAND_MAX);
}


#if defined(MOD_TAI) &&  NTP_API == 4
void
setKernelUtcOffset(int utc_offset) {

	struct timex tmx;
	int ret;

	memset(&tmx, 0, sizeof(tmx));

	tmx.modes = MOD_TAI;
	tmx.constant = utc_offset;

	DBG2("Kernel NTP API supports TAI offset. "
	     "Setting TAI offset to %d\n", utc_offset);

	ret = adjtimex(&tmx);

	if (ret < 0) {
		PERROR("Could not set kernel TAI offset: %s", strerror(errno));
	}
}
#endif /* MOD_TAI */
