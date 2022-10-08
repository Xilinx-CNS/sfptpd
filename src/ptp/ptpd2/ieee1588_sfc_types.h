/* SPDX-License-Identifier: BSD-2-Clause */
/* (c) Copyright 2017-2019 Xilinx, Inc. */

#ifndef IEEE1588_SFC_TYPES_H_
#define IEEE1588_SFC_TYPES_H_


/**
*\file
* \brief Data structures defined in Solarflare extensions to IEEE1588.
*/

typedef struct {
#define OPERATE( name, size, type ) type name;
	#include "def/sfc/slave_status.def"
} SlaveStatus;


#endif /*IEEE1588_SFC_TYPES_H_*/
