/* SPDX-License-Identifier: BSD-2-Clause */
/* (c) Copyright 2017,2022 Xilinx, Inc. */

#ifndef _PTPD2_CONSTANTS_THIRDPARTY_H
#define _PTPD2_CONSTANTS_THIRDPARTY_H

/**
 *\file
 * \brief Default values and constants used in ptpd2 for third party extensions
 */

#define PTPD_MEINBERG_TLV_ORGANISATION_ID 0xEC4670

/**
 * \brief Meinberg Organisation Extension tlv types (SF-118270-PS-2)
 */
typedef enum {
	PTPD_TLV_PTPMON_REQ = 0x000001,
	PTPD_TLV_PTPMON_RESP = 0x000002,
	PTPD_TLV_MTIE_REQ = 0x000003,
	PTPD_TLV_MTIE_RESP = 0x000004,
} ptpd_meinberg_tlv_type_e;

typedef enum {
	PTPD_NSM_CLOCK_TYPE_PTP = 0,
	PTPD_NSM_CLOCK_TYPE_OS = 1,
	PTPD_NSM_CLOCK_TYPE_USER_0 = 64,
} ptpd_nsm_clock_type_e;

typedef enum {
	PTPD_NSM_CLOCK_STATUS_LOCKED = 0,
	PTPD_NSM_CLOCK_STATUS_SYNCHRONISING = 1,
	PTPD_NSM_CLOCK_STATUS_HOLDOVER = 2,
	PTPD_NSM_CLOCK_STATUS_FREERUN = 3,
	PTPD_NSM_CLOCK_STATUS_DISABLED = 254,
	PTPD_NSM_CLOCK_STATUS_UNKNOWN = 255
} ptpd_nsm_clock_status_e;


#endif /* _PTPD2_CONSTANTS_THIRDPARTY_H */
