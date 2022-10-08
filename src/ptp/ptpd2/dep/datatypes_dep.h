/* SPDX-License-Identifier: BSD-2-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */
/* (c) Copyright prior contributors */

#ifndef DATATYPES_DEP_H_
#define DATATYPES_DEP_H_

#include <sys/ioctl.h>
#include <net/if.h>
#include "sfptpd_time.h"

/**
*\file
* \brief Implementation specific datatype
 */
typedef enum {FALSE=0, TRUE} Boolean;
typedef char Octet;
typedef int8_t Integer8;
typedef int16_t Integer16;
typedef int32_t Integer32;
typedef uint8_t  UInteger8;
typedef uint16_t UInteger16;
typedef uint32_t UInteger24;
typedef uint32_t UInteger32;
typedef uint64_t UInteger48;
typedef int64_t Integer64;
typedef uint16_t Enumeration16;
typedef unsigned char Enumeration8;
typedef unsigned char Enumeration4;
typedef unsigned char Enumeration4Upper;
typedef unsigned char Enumeration4Lower;
typedef unsigned char UInteger4;
typedef unsigned char UInteger4Upper;
typedef unsigned char UInteger4Lower;
typedef unsigned char Nibble;
typedef unsigned char NibbleUpper;
typedef unsigned char NibbleLower;
typedef long double LongDouble;


/**
* \brief Struct used to average the offset from master
*
* The FIR filtering of the offset from master input is a simple, two-sample average
 */
typedef struct {
	sfptpd_time_t y;
	sfptpd_time_t prev;
} offset_from_master_filter;

/**
* \brief Struct used to average the one way delay
*
* It is a variable cutoff/delay low-pass, infinite impulse response (IIR) filter.
*
*  The one-way delay filter has the difference equation: s*y[n] - (s-1)*y[n-1] = x[n]/2 + x[n-1]/2, where increasing the stiffness (s) lowers the cutoff and increases the delay.
 */
typedef struct {
	sfptpd_time_t y;
	sfptpd_time_t prev;
	unsigned int s_exp;
} one_way_delay_filter;


#endif /*DATATYPES_DEP_H_*/
