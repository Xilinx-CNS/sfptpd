/* SPDX-License-Identifier: BSD-2-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */
/* (c) Copyright prior contributors */

#ifndef _PTPD2_CONSTANTS_H
#define _PTPD2_CONSTANTS_H

/**
 *\file
 * \brief Default values and constants used in ptpd2
 *
 * This header file includes all default values used during initialization
 * and enumeration defined in the spec
 */

/* Indentation string used in logs */
#define INFO_PREFIX "=== "

/* default values */
#define PTPD_DEFAULT_KP			0.2
#define PTPD_DEFAULT_KI			0.003
#define PTPD_DEFAULT_KD			0.0
#define PTPD_DEFAULT_TTL		64

#define DEFAULT_ANNOUNCE_INTERVAL    	1       /* 0 in 802.1AS */
#define DEFAULT_SYNC_INTERVAL        	0      /* -7 in 802.1AS */  /* from page 237 of the standard */
#define DEFAULT_DELAYREQ_INTERVAL    	0      /* new value from page 237 of the standard */
#define DEFAULT_PDELAYREQ_INTERVAL   	1      /* -4 in 802.1AS */

/* number of announces we need to lose until a time out occurs. Thus it is 12 seconds */
#define DEFAULT_ANNOUNCE_RECEIPT_TIMEOUT 6     /* 3 by default */
/* multiples of sync interval after which if no sync is received we start reporting warnings */
#define DEFAULT_SYNC_RECEIPT_TIMEOUT 	6
/* timeout for receiving DelayResps, expressed in 2^x seconds (e.g. -2 = 250ms) */
#define DEFAULT_DELAY_RESP_RECEIPT_TIMEOUT -2
/* how many missing delay responses in a row before we raise an alarm */
#define DEFAULT_DELAY_RESP_ALARM_THRESHOLD 5
/* Number of hybrid delay response failures before we revert to multicast mode */
#define DEFAULT_DELAY_RESP_HYBRID_THRESHOLD 3

#define TIMESTAMP_HEALTH_CHECK_INTERVAL 10

/* Values for PTP filters */
#define DEFAULT_MPD_FILTER_SIZE 8
#define DEFAULT_MPD_FILTER_AGEING 2.0
#define DEFAULT_OUTLIER_FILTER_SIZE 60
#define DEFAULT_OUTLIER_FILTER_ADAPTION 1.0
#define DEFAULT_OUTLIER_FILTER_DRIFT false
#define DEFAULT_FIR_FILTER_SIZE 1

/* Value used in logMessageInterval field to indicate an unknown/undefined
 * interval. */
#define PTPD_MESSAGE_INTERVAL_UNDEFINED (0x7f)

/* Valid ranges for the message interval for different message types */
#define PTPD_ANNOUNCE_INTERVAL_MIN -4  /* Telecoms profile [-3,4] */
#define PTPD_ANNOUNCE_INTERVAL_MAX 4   /* Default profile [0,4] */

#define PTPD_SYNC_INTERVAL_MIN -4      /* Telecoms profile is [-7,4] */
#define PTPD_SYNC_INTERVAL_MAX 4       /* Default profile is [-1,1] */

#define PTPD_DELAY_REQ_INTERVAL_MIN -4 /* Telecoms profile [-7,4] */
#define PTPD_DELAY_REQ_INTERVAL_MAX 5  /* Default profile [0,5] */


/*
section 7.6.2.4, page 55:
248     Default. This clockClass shall be used if none of the other clockClass definitions apply.
13      Shall designate a clock that is synchronized to an application-specific source of time. The timescale distributed
        shall be ARB. A clockClass 13 clock shall not be a slave to another clock in the domain.
*/
#define DEFAULT_CLOCK_CLASS					248
#define SLAVE_ONLY_CLOCK_CLASS					255

/*
 * section 7.6.2.5, page 56:
 * 0x20      Time accurate to 25ns
 * ...
 * 0x31      Time accurate to > 10s
 * 0xFE      Unkown accuracy
 */
#define DEFAULT_CLOCK_ACCURACY   0xFE

#define DEFAULT_PRIORITY1		128
#define DEFAULT_PRIORITY2		128        /* page 238, default priority is the midpoint, to allow easy control of the BMC algorithm */

/* page 238:  τ, see 7.6.3.2: The default initialization value shall be 1.0 s.  */
#define DEFAULT_CLOCK_VARIANCE           28768 /* To be determined in 802.1AS. */

#define DEFAULT_MAX_FOREIGN_RECORDS  	16

#define MAX_FOREIGN_NODES 16

#define	FOREIGN_MASTER_THRESHOLD	2
#define	FOREIGN_MASTER_TIME_WINDOW	4
#define	FOREIGN_MASTER_TIME_CHECK	1

#define MAX_SLAVE_EVENT_MONITORING_EVENTS_PER_TLV 10
#define MAX_SLAVE_EVENT_DESTS 6

/* PTP Protocol version number */
#define PTPD_PROTOCOL_VERSION 2
#define PTPD_PROTOCOL_VERSION_MINOR_DEFAULT 0

/* Physical layer protocol - text string reported in management message to
 * get clock description */
#define PTPD2_PHYSICAL_LAYER_PROTOCOL "IEEE 802.3"

/* After a fault occurs, seconds to wait before re-initialising */
#define PTPD_FAULT_RESTART_INTERVAL  5

/* features, only change to refelect changes in implementation */
#define PTPD_TWO_STEP_FLAG   TRUE

/* Maximum length of various management strings */
#define PTPD_MGMT_PRODUCT_DESC_MAX 64
#define PTPD_MGMT_REVISION_DATA_MAX (SFPTPD_VERSION_STRING_MAX)
#define PTPD_MGMT_USER_DESCRIPTION_MAX 128

/**
 * \name Packet length
 * Minimal length values for each message. If TLV used length could be higher
 */
#define PTPD_HEADER_LENGTH			34
#define PTPD_ANNOUNCE_LENGTH			64
#define PTPD_SYNC_LENGTH			44
#define PTPD_FOLLOW_UP_LENGTH			44
#define PTPD_PDELAY_REQ_LENGTH			54
#define PTPD_DELAY_REQ_LENGTH			44
#define PTPD_DELAY_RESP_LENGTH			54
#define PTPD_PDELAY_RESP_LENGTH 		54
#define PTPD_PDELAY_RESP_FOLLOW_UP_LENGTH	54
#define PTPD_MANAGEMENT_LENGTH			48
#define PTPD_SIGNALING_LENGTH			44
#define PTPD_TLV_LENGTH				6
#define PTPD_TLV_MANAGEMENT_ID_LENGTH		2
#define PTPD_TLV_HEADER_LENGTH			4

#define PTPD_SFC_TLV_ORGANISATION_ID (SFPTPD_OUI0 << 16) | (SFPTPD_OUI1 << 8) | SFPTPD_OUI2


/**
 * \brief Network Protocol  (Table 3 in the spec)
 */
typedef enum {
	PTPD_NETWORK_PROTOCOL_UDP_IPV4 = 1,
	PTPD_NETWORK_PROTOCOL_UDP_IPV6,
	PTPD_NETWORK_PROTOCOL_IEEE_802_3,
	PTPD_NETWORK_PROTOCOL_DEVICE_NET,
	PTPD_NETWORK_PROTOCOL_CONTROL_NET,
	PTPD_NETWORK_PROTOCOL_PROFINET
} ptpd_network_protocol_e;

/**
 * \brief Clock accuracy enumeration (Table 6 in the spec)
 */
typedef enum {
	PTPD_ACCURACY_WITHIN_25NS = 0x20,
	PTPD_ACCURACY_WITHIN_100NS = 0x21,
	PTPD_ACCURACY_WITHIN_250NS = 0x22,
	PTPD_ACCURACY_WITHIN_1US = 0x23,
	PTPD_ACCURACY_WITHIN_2US5 = 0x24,
	PTPD_ACCURACY_WITHIN_10US = 0x25,
	PTPD_ACCURACY_WITHIN_25US = 0x26,
	PTPD_ACCURACY_WITHIN_100US = 0x27,
	PTPD_ACCURACY_WITHIN_250US = 0x28,
	PTPD_ACCURACY_WITHIN_1MS = 0x29,
	PTPD_ACCURACY_WITHIN_2MS5 = 0x2A,
	PTPD_ACCURACY_WITHIN_10MS = 0x2B,
	PTPD_ACCURACY_WITHIN_25MS = 0x2C,
	PTPD_ACCURACY_WITHIN_100MS = 0x2D,
	PTPD_ACCURACY_WITHIN_250MS = 0x2E,
	PTPD_ACCURACY_WITHIN_1S = 0x2F,
	PTPD_ACCURACY_WITHIN_10S = 0x30,
	PTPD_ACCURACY_MORE_THAN_10S = 0x31,
	PTPD_ACCURACY_UNKNOWN = 0xFE
} ptpd_clock_accuracy_e;

/**
 * \brief Time source enumeration (Table 8 in the spec)
 */
typedef enum {
	PTPD_TIME_SOURCE_ATOMIC_CLOCK = 0x10,
	PTPD_TIME_SOURCE_GPS = 0x20,
	PTPD_TIME_SOURCE_TERRESTRIAL_RADIO = 0x30,
	PTPD_TIME_SOURCE_PTP = 0x40,
	PTPD_TIME_SOURCE_NTP = 0x50,
	PTPD_TIME_SOURCE_HAND_SET = 0x60,
	PTPD_TIME_SOURCE_OTHER = 0x90,
	PTPD_TIME_SOURCE_INTERNAL_OSCILLATOR = 0xA0
} ptpd_time_source_e;

/**
 * \brief Delay mechanism (Table 9 in the spec)
 */
typedef enum {
	PTPD_DELAY_MECHANISM_E2E = 1,
	PTPD_DELAY_MECHANISM_P2P = 2,
	PTPD_DELAY_MECHANISM_DISABLED = 0xFE
} ptpd_delay_mechanism_e;


/**
 * \brief The type of node
 */
typedef enum {
	PTPD_NODE_CLOCK = 0,
	PTPD_NODE_MONITOR
} ptpd_node_type_e;

/**
 * \brief PTP timers
 */
typedef enum {
	PDELAYREQ_INTERVAL_TIMER=0, /**<\brief Timer handling the PdelayReq Interval */
	PDELAYRESP_RECEIPT_TIMER, /**<\brief Timer handling the PdelayResp receipt timeout */
	DELAYREQ_INTERVAL_TIMER, /**<\brief Timer handling the delayReq Interval */
	DELAYRESP_RECEIPT_TIMER, /**<\brief Timer handling delayResp receipt timeout */
	SYNC_RECEIPT_TIMER, /**<\brief Timer handling sync receipt timeout */
	SYNC_INTERVAL_TIMER, /**<\brief Timer handling Interval between master sends two Syncs messages */
	ANNOUNCE_RECEIPT_TIMER, /**<\brief Timer handling announce receipt timeout */
	ANNOUNCE_INTERVAL_TIMER, /**<\brief Timer handling interval before master sends two announce messages */
	OPERATOR_MESSAGES_TIMER, /**<\brief Timer used to limit the operator messages */
	FAULT_RESTART_TIMER, /**<\brief Timer used to control restart after fault */
	FOREIGN_MASTER_TIMER, /**<\brief Timer used to foreign master data set */
	TIMESTAMP_CHECK_TIMER, /**<\brief Timer used to check timestamp health */
	TIMER_ARRAY_SIZE
} ptpd_timer_id_e;

/**
 * \brief PTP Management Message managementId values (Table 40 in the spec)
 */
/* SLAVE_ONLY conflicts with another constant, so scope with MM_ */
typedef enum {
	/* Applicable to all node types */
	MM_NULL_MANAGEMENT=0x0000,
	MM_CLOCK_DESCRIPTION=0x0001,
	MM_USER_DESCRIPTION=0x0002,
	MM_SAVE_IN_NON_VOLATILE_STORAGE=0x0003,
	MM_RESET_NON_VOLATILE_STORAGE=0x0004,
	MM_INITIALIZE=0x0005,
	MM_FAULT_LOG=0x0006,
	MM_FAULT_LOG_RESET=0x0007,

	/* Reserved: 0x0008 - 0x1FFF */

	/* Applicable to ordinary and boundary clocks */
	MM_DEFAULT_DATA_SET=0x2000,
	MM_CURRENT_DATA_SET=0x2001,
	MM_PARENT_DATA_SET=0x2002,
	MM_TIME_PROPERTIES_DATA_SET=0x2003,
	MM_PORT_DATA_SET=0x2004,
	MM_PRIORITY1=0x2005,
	MM_PRIORITY2=0x2006,
	MM_DOMAIN=0x2007,
	MM_SLAVE_ONLY=0x2008,
	MM_LOG_ANNOUNCE_INTERVAL=0x2009,
	MM_ANNOUNCE_RECEIPT_TIMEOUT=0x200A,
	MM_LOG_SYNC_INTERVAL=0x200B,
	MM_VERSION_NUMBER=0x200C,
	MM_ENABLE_PORT=0x200D,
	MM_DISABLE_PORT=0x200E,
	MM_TIME=0x200F,
	MM_CLOCK_ACCURACY=0x2010,
	MM_UTC_PROPERTIES=0x2011,
	MM_TRACEABILITY_PROPERTIES=0x2012,
	MM_TIMESCALE_PROPERTIES=0x2013,
	MM_UNICAST_NEGOTIATION_ENABLE=0x2014,
	MM_PATH_TRACE_LIST=0x2015,
	MM_PATH_TRACE_ENABLE=0x2016,
	MM_GRANDMASTER_CLUSTER_TABLE=0x2017,
	MM_UNICAST_MASTER_TABLE=0x2018,
	MM_UNICAST_MASTER_MAX_TABLE_SIZE=0x2019,
	MM_ACCEPTABLE_MASTER_TABLE=0x201A,
	MM_ACCEPTABLE_MASTER_TABLE_ENABLED=0x201B,
	MM_ACCEPTABLE_MASTER_MAX_TABLE_SIZE=0x201C,
	MM_ALTERNATE_MASTER=0x201D,
	MM_ALTERNATE_TIME_OFFSET_ENABLE=0x201E,
	MM_ALTERNATE_TIME_OFFSET_NAME=0x201F,
	MM_ALTERNATE_TIME_OFFSET_MAX_KEY=0x2020,
	MM_ALTERNATE_TIME_OFFSET_PROPERTIES=0x2021,

	/* Reserved: 0x2022 - 0x3FFF */

	/* Applicable to transparent clocks */
	MM_TRANSPARENT_CLOCK_DEFAULT_DATA_SET=0x4000,
	MM_TRANSPARENT_CLOCK_PORT_DATA_SET=0x4001,
	MM_PRIMARY_DOMAIN=0x4002,

	/* Reserved: 0x4003 - 0x5FFF */

	/* Applicable to ordinary, boundary, and transparent clocks */
	MM_DELAY_MECHANISM=0x6000,
	MM_LOG_MIN_PDELAY_REQ_INTERVAL=0x6001,

	/* Reserved: 0x6002 - 0xBFFF */
	/* Implementation-specific identifiers: 0xC000 - 0xDFFF */
	/* Assigned by alternate PTP profile: 0xE000 - 0xFFFE */
	/* Reserved: 0xFFFF */
} ptpd_management_msg_id_e;

/**
 * \brief MANAGEMENT MESSAGE INITIALIZE (Table 44 in the spec)
 */
#define PTPD_MGMT_INITIALIZE_EVENT 0x0

/**
 * \brief MANAGEMENT ERROR STATUS managementErrorId (Table 72 in the spec)
 */
typedef enum {
	PTPD_MGMT_OK = 0x0000,
	PTPD_MGMT_ERROR_RESPONSE_TOO_BIG = 0x0001,
	PTPD_MGMT_ERROR_NO_SUCH_ID = 0x0002,
	PTPD_MGMT_ERROR_WRONG_LENGTH = 0x0003,
	PTPD_MGMT_ERROR_WRONG_VALUE = 0x0004,
	PTPD_MGMT_ERROR_NOT_SETABLE = 0x0005,
	PTPD_MGMT_ERROR_NOT_SUPPORTED = 0x0006,
	PTPD_MGMT_ERROR_GENERAL_ERROR = 0xFFFE
} ptpd_mgmt_error_e;

/**
 * \brief PTP tlvType values (Table 34 in the spec)
 */
typedef enum {
	/* Standard TLVs */
	PTPD_TLV_MANAGEMENT=0x0001,
	PTPD_TLV_MANAGEMENT_ERROR_STATUS=0x0002,
	PTPD_TLV_ORGANIZATION_EXTENSION=0x0003,

	/* Optional unicast message negotiation TLVs */
	PTPD_TLV_REQUEST_UNICAST_TRANSMISSION=0x0004,
	PTPD_TLV_GRANT_UNICAST_TRANSMISSION=0x0005,
	PTPD_TLV_CANCEL_UNICAST_TRANSMISSION=0x0006,
	PTPD_TLV_ACKNOWLEDGE_CANCEL_UNICAST_TRANSMISSION=0x0007,

	/* Optional path trace mechanism TLV */
	PTPD_TLV_PATH_TRACE=0x0008,

	/* Optional alternate timescale TLV */
	PTPD_TLV_ALTERNATE_TIME_OFFSET_INDICATOR=0x0009,

	/* @task69245: New Optional TLVs from IEEE1588-Rev draft 14.1.1 */
	PTPD_TLV_CUMULATIVE_RATE_RATIO=0x000A,
	PTPD_TLV_ENHANCED_ACCURACY_METRICS=0x000B,
	PTPD_TLV_SECURITY=0x000D,

	/* Legacy security TLVs */
	PTPD_TLV_AUTHENTICATION=0x2000,
	PTPD_TLV_AUTHENTICATION_CHALLENGE=0x2001,
	PTPD_TLV_SECURITY_ASSOCIATION_UPDATE=0x2002,

	/* Legacy cumulative frequency scale factor offset */
	PTPD_TLV_CUM_FREQ_SCALE_FACTOR_OFFSET=0x2003,

	/* SWPTP-615: Meinberg "NetSync Monitor" extension (rev 6, SF-118270-PS-2)*/
	PTPD_TLV_PTPMON_REQ_OLD=0x21FE,
	PTPD_TLV_PTPMON_RESP_OLD=0x21FF,

	/* SWPTP-668: Meinberg "NetSync Monitor" extension (rev 6, SF-118270-PS-2)*/
	PTPD_TLV_MTIE_REQ_OLD=0x2200,
	PTPD_TLV_MTIE_RESP_OLD=0x2201,
	PTPD_TLV_CLOCK_OFFS_REQ=0x2202,
	PTPD_TLV_CLOCK_OFFS_RESP=0x2203,

	/* @task69245: New Optional forwarding TLVs from IEEE1588-Rev draft 14.1.1 */
	PTPD_TLV_ORGANIZATION_EXTENSION_FORWARDING=0x4000,

	/* @task69245: New Optional non-forwarding TLVs from IEEE1588-Rev draft 14.1.1 */
	PTPD_TLV_ORGANIZATION_EXTENSION_NON_FORWARDING=0x8000,
	PTPD_TLV_L1_SYNC=0x8001,
	PTPD_TLV_PORT_COMMUNICATION_CAPABILITIES=0x8002,
	PTPD_TLV_PROTOCOL_ADDRESS=0x8003,
	PTPD_TLV_PAD=0x8008,

	/* @task71778: Slave event monitoring TLVs (IEEE1588-Rev draft 16.11.4.1). */
	PTPD_TLV_SLAVE_RX_SYNC_TIMING_DATA = 0x8004,
	PTPD_TLV_SLAVE_RX_SYNC_COMPUTED_DATA = 0x8005,
	PTPD_TLV_SLAVE_TX_EVENT_TIMESTAMPS = 0x8006,
} ptpd_tlv_type_e;

/**
 * \brief Solarflare Organisation Extension tlv types
 */
typedef enum {
	PTPD_TLV_SFC_SLAVE_STATUS = 0x000001,
} ptpd_sfc_tlv_type_e;

typedef enum {
	PTPD_SFC_ALARM_NO_TX_TIMESTAMPS = 0,
	PTPD_SFC_ALARM_NO_RX_TIMESTAMPS,
	PTPD_SFC_ALARM_NO_INTERFACE,
	PTPD_SFC_ALARM_SERVO_FAIL,
	PTPD_SFC_ALARM_UNKNOWN,
} ptpd_sfc_alarms_e;

typedef enum {
	PTPD_SFC_EVENT_BOND_CHANGED = 0,
} ptpd_sfc_event_e;

typedef enum {
	PTPD_SFC_FLAG_IN_SYNC = 0,
	PTPD_SFC_FLAG_SELECTED = 1,
} ptpd_sfc_flag_e;

/**
 * \brief Management Message actions (Table 38 in the spec)
 */
typedef enum {
	PTPD_MGMT_ACTION_GET = 0,
	PTPD_MGMT_ACTION_SET,
	PTPD_MGMT_ACTION_RESPONSE,
	PTPD_MGMT_ACTION_COMMAND,
	PTPD_MGMT_ACTION_ACKNOWLEDGE
} ptpd_mgmt_action_e;

/**
 * \brief PTP states
 */
typedef enum {
	PTPD_UNINITIALIZED = 0,
	PTPD_INITIALIZING,
	PTPD_FAULTY,
	PTPD_DISABLED,
	PTPD_LISTENING,
	PTPD_PRE_MASTER,
	PTPD_MASTER,
	PTPD_PASSIVE,
	PTPD_UNCALIBRATED,
	PTPD_SLAVE
} ptpd_state_e;

/**
 * \brief PTP Messages
 */
typedef enum {
	PTPD_MSG_SYNC = 0x0,
	PTPD_MSG_DELAY_REQ,
	PTPD_MSG_PDELAY_REQ,
	PTPD_MSG_PDELAY_RESP,
	PTPD_MSG_FOLLOW_UP = 0x8,
	PTPD_MSG_DELAY_RESP,
	PTPD_MSG_PDELAY_RESP_FOLLOW_UP,
	PTPD_MSG_ANNOUNCE,
	PTPD_MSG_SIGNALING,
	PTPD_MSG_MANAGEMENT,
} ptpd_msg_id_e;

/**
 * \brief flagField0 values (Table 20 in the spec)
 */
typedef enum
{
	PTPD_FLAG_ALTERNATE_MASTER = 0x01,
	PTPD_FLAG_TWO_STEP = 0x02,
	PTPD_FLAG_UNICAST = 0x04,
	PTPD_FLAG_PROFILE_SPECIFIC_1 = 0x20,
	PTPD_FLAG_PROFILE_SPECIFIC_2 = 0x40,
	PTPD_FLAG_SECURITY = 0x80,
} ptpd_flag_field0_e;

/**
 * \brief flagField1 bit position values (Table 20 in the spec)
 */
typedef enum {
	PTPD_LI61 = 0,
	PTPD_LI59 = 1,
	PTPD_UTCV = 2,
	PTPD_PTPT = 3,
	PTPD_TTRA = 4,
	PTPD_FTRA = 5
} ptpd_flag_field1_lsb_e;

/**
 * \brief flagField1 values (Table 20 in the spec)
 */
typedef enum
{
	PTPD_FLAG_LEAP_61 = (1 << PTPD_LI61),
	PTPD_FLAG_LEAP_59 = (1 << PTPD_LI59),
	PTPD_FLAG_UTC_OFFSET_VALID = (1 << PTPD_UTCV),
	PTPD_FLAG_PTP_TIMESCALE = (1 << PTPD_PTPT),
	PTPD_FLAG_TIME_TRACEABLE = (1 << PTPD_TTRA),
	PTPD_FLAG_FREQUENCY_TRACEABLE = (1 << PTPD_FTRA)
} ptpd_flag_field1_e;

/**
 * \brief controlField values (Table 23 in the spec)
 */
typedef enum
{
	PTPD_CONTROL_FIELD_SYNC = 0,
	PTPD_CONTROL_FIELD_DELAY_REQ = 1,
	PTPD_CONTROL_FIELD_FOLLOW_UP = 2,
	PTPD_CONTROL_FIELD_DELAY_RESP = 3,
	PTPD_CONTROL_FIELD_MANAGEMENT = 4,
	PTPD_CONTROL_FIELD_ALL_OTHERS = 5
} ptpd_control_field_e;

/**
 * \brief clockType values (Table 42 in the spec)
 */
typedef enum
{
	PTPD_CLOCK_TYPE_ORDINARY = 0x80,
	PTPD_CLOCK_TYPE_BOUNDARY = 0x40,
	PTPD_CLOCK_TYPE_P2P_TRANSPARENT = 0x20,
	PTPD_CLOCK_TYPE_E2E_TRANSPARENT = 0x10,
	PTPD_CLOCK_TYPE_MANAGEMENT = 0x08
} ptpd_clock_type_e;

enum ptpd_acl_type {
	PTPD_ACL_MANAGEMENT = 1 << 0,
	PTPD_ACL_TIMING = 1 << 1,
	PTPD_ACL_MONITORING = 1 << 2,
};

typedef enum {
	PTPD_SLAVE_TX_TS_DELAY_REQ,
	PTPD_SLAVE_TX_TS_PDELAY_REQ,
	PTPD_SLAVE_TX_TS_PDELAY_RESP,
	PTPD_SLAVE_TX_TS_NUM
} ptpd_slave_tx_ts_msg_e;

/**
 * \brief message capabilities flags
 */
typedef enum {
	PTPD_COMM_MULTICAST_CAPABLE = (1 << 0),
	PTPD_COMM_UNICAST_CAPABLE = (1 << 1),
	PTPD_COMM_UNICAST_NEG_CAPABLE = (1 << 2),
	PTPD_COMM_UNICAST_NEG_REQUIRED = (1 << 3)
} ptpd_flag_message_capabilities_e;

#include "constants_thirdparty.h"

#endif /* _PTPD2_CONSTANTS_H */
