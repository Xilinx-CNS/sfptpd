/* SPDX-License-Identifier: BSD-2-Clause */
/* (c) Copyright 2017-2019 Xilinx, Inc. */

#ifndef IEEE1588_THIRDPARTY_TYPES_H_
#define IEEE1588_THIRDPARTY_TYPES_H_


/**
*\file
* \brief Data structures defined in PTP profiles or extensions
*/

typedef MMCurrentDataSet IncCurrentDataSet;
typedef MMParentDataSet IncParentDataSet;
typedef MMTimePropertiesDataSet IncTimePropertiesDataSet;

typedef struct {
#define OPERATE( name, size, type ) type name;
	#include "def/thirdparty/ptpmon_resp_tlv.def"
} PTPMonRespTLV;

typedef struct {
#define OPERATE( name, size, type ) type name;
	#include "def/thirdparty/mtie_resp_tlv.def"
} MTIERespTLV;

typedef struct {
#define OPERATE( name, size, type ) type name;
	#include "def/thirdparty/clock_offs_resp_tlv.def"
} ClockOffsRespTLV;

#endif /*IEEE1588_THIRDPARTY_TYPES_H_*/
