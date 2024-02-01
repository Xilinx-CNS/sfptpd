/* SPDX-License-Identifier: BSD-2-Clause */
/* (c) Copyright 2012-2023 Advanced Micro Devices, Inc. */
/* (c) Copyright prior contributors */

#ifndef DATATYPES_H_
#define DATATYPES_H_


/**
*\file
* \brief Data structures for ptpd
*
* This header file defines structures for ptpd internals.
*/


#include <stdio.h>
#include "sfptpd_statistics.h"
#include "sfptpd_filter.h"
#include "sfptpd_ptp_timestamp_dataset.h"
#include "sfptpd_time.h"
#include "sfptpd_sync_module.h"
#include "sfptpd_general_config.h"


typedef struct {
	int min;
	int max;
	int def;
} sfptpd_int_range_t;


/* PTP Profiles */
typedef struct sfptpd_ptp_profile_def {
	const char *name;
	const char *uri;
	const char *version;
	uint8_t id[6];
	sfptpd_int_range_t announce_interval;
	sfptpd_int_range_t sync_interval;
	sfptpd_int_range_t delayreq_interval;
	sfptpd_int_range_t announce_timeout;
	uint8_t delay_mechanisms; /*!< bitfield of supported delay mechanisms */
} sfptpd_ptp_profile_def_t;


/**
* \brief Structure used as a timer
*/
typedef struct {
	Integer32  interval;
	Integer32  left;
	Boolean expire;
} IntervalTimer;

typedef struct {
	struct sfptpd_timespec timestamp;
	sfptpd_time_t offset;
	bool have_timestamp;
	bool have_offset;
	UInteger16 seq;
} ForeignSyncSnapshot;

/**
* \brief ForeignMasterRecord is used to manage foreign masters
*/
typedef struct
{
	PortIdentity foreignMasterPortIdentity;

	//This one is not in the spec
	MsgAnnounce  announce;
	MsgHeader    header;

	/* The announce multicast/unicast capabilities */
	PortCommunicationCapabilities comm_caps;

	/* The Master's IP address - used for hybrid mode */
	struct sockaddr_storage address;
	socklen_t addressLen;

	/* The last times announcements were received from this
	   foreign master according to CLOCK_MONOTONIC. */
	struct sfptpd_timespec announceTimes[FOREIGN_MASTER_THRESHOLD];
	int announceTimesWriteIdx;
	int announceTimesCount;

	/* Snapshot of Sync for use with discriminator */
	ForeignSyncSnapshot syncSnapshot;
} ForeignMasterRecord;

typedef struct {
	ForeignMasterRecord *records;
	UInteger16 number_records;
	Integer16 max_records;
	Integer16 write_index;
	Integer16 best_index;
} ForeignMasterDS;

enum ptpd_tlv_result {
	PTPD_TLV_RESULT_CONTINUE,
	PTPD_TLV_RESULT_DROP,
	PTPD_TLV_RESULT_ERROR,
};

/**
 * \struct PtpdCounters
 * \brief Ptpd engine counters per port
 */
typedef struct ptpd_counters
{
	/*
	 * message sent/received counters:
	 * - sent only incremented on success,
	 * - received only incremented when message valid and accepted,
	 * - looped messages to self don't increment received,
	 */
	uint32_t announceMessagesSent;
	uint32_t announceMessagesReceived;
	uint32_t syncMessagesSent;
	uint32_t syncMessagesReceived;
	uint32_t followUpMessagesSent;
	uint32_t followUpMessagesReceived;
	uint32_t delayReqMessagesSent;
	uint32_t delayReqMessagesReceived;
	uint32_t delayRespMessagesSent;
	uint32_t delayRespMessagesReceived;
	uint32_t pdelayReqMessagesSent;
	uint32_t pdelayReqMessagesReceived;
	uint32_t pdelayRespMessagesSent;
	uint32_t pdelayRespMessagesReceived;
	uint32_t pdelayRespFollowUpMessagesSent;
	uint32_t pdelayRespFollowUpMessagesReceived;
	uint32_t signalingMessagesSent;
	uint32_t signalingMessagesReceived;
	uint32_t managementMessagesSent;
	uint32_t managementMessagesReceived;

	uint32_t monitoringTLVsReceived;
	uint32_t monitoringTLVsSent;
	uint32_t monitoringTLVsDiscarded;
	uint32_t monitoringTLVsSyncsSent;
	uint32_t monitoringTLVsFollowUpsSent;

	/* protocol engine counters */
	uint32_t stateTransitions;	  /* number of state changes */
	uint32_t masterChanges;		  /* number of BM changes as result of BMC */
	uint32_t announceTimeouts;	  /* number of announce receipt timeouts */
	uint32_t syncTimeouts;            /* number of sync message receipt timeouts */
	uint32_t followUpTimeouts;        /* number of follow up receipt timeouts */
	uint32_t outOfOrderFollowUps;     /* number of out of order follow-ups */
	uint32_t delayRespTimeouts;       /* number of sync response receipt timeouts */
	uint32_t clockSteps;              /* number of times that the clock has been stepped */
	uint32_t outliers;	          /* number of outliers */
	uint32_t outliersNumSamples;      /* number of outlier samples taken */

	/* discarded / uknown / ignored */
	uint32_t discardedMessages;	  /* only messages we shouldn't be receiving - ignored from self don't count */
	uint32_t unknownMessages;	  /* unknown type - also increments discarded */
	uint32_t ignoredAnnounce;	  /* ignored Announce messages: acl / security / preference */
	uint32_t aclTimingDiscardedMessages;	  /* Timing messages discarded by access lists */
	uint32_t aclManagementDiscardedMessages;  /* Timing messages discarded by access lists */

	/* error counters */
	uint32_t messageRecvErrors;	  /* message receive errors */
	uint32_t messageSendErrors;	  /* message send errors */
	uint32_t messageFormatErrors;	  /* headers or messages too short etc. */
	uint32_t protocolErrors;	  /* conditions that shouldn't happen */
	uint32_t versionMismatchErrors;	  /* V1 received, V2 expected - also increments discarded */
	uint32_t domainMismatchErrors;	  /* different domain than configured - also increments discarded */
	uint32_t sequenceMismatchErrors;  /* mismatched sequence IDs - also increments discarded */
	uint32_t delayModeMismatchErrors; /* P2P received, E2E expected or vice versa - increments discarded */
	uint32_t txPktNoTimestamp;        /* transmitted packet for which no timestamp could be retrieved */
	uint32_t rxPktNoTimestamp;        /* received packet for which no timestamp available */

#ifdef PTPD_STATISTICS
	uint32_t delayMSOutliersFound;	  /* Number of outliers found by the delayMS filter */
	uint32_t delaySMOutliersFound;	  /* Number of outliers found by the delaySM filter */
#endif /* PTPD_STATISTICS */

} PtpdCounters;

/**
 * \brief Whether the input ran out while unpacking data
 */
inline bool UNPACK_OK(ssize_t result) {
	return result >= 0;
}

inline size_t UNPACK_GET_SIZE(ssize_t result) {
	assert(result >= 0);
	return result;
}

inline ssize_t UNPACK_SIZE(size_t size) {
	return ((ssize_t) size);
}

/**
 * \brief Whether the output space ran out while packing data
 */
inline bool PACK_OK(ssize_t result) {
	return result >= 0;
}

inline size_t PACK_GET_SIZE(ssize_t result) {
	assert(result >= 0);
	return result;
}

inline ssize_t PACK_SIZE(size_t size) {
	return ((ssize_t) size);
}

#define UNPACK_INIT ((ssize_t) 0)
#define UNPACK_ERROR ((ssize_t) -1)

#define PACK_INIT ((ssize_t) 0)
#define PACK_ERROR ((ssize_t) -1)

/**
*  \brief Timestamp method in use
*/
typedef enum {
	TS_METHOD_SYSTEM,
	TS_METHOD_SO_TIMESTAMPING
} TsMethod;


/** Forward declaration of clock and interface structures */
struct sfptpd_clock;
struct sfptpd_interface;

typedef struct ptpd_global_context PtpGlobal;
typedef struct ptpd_intf_context PtpInterface;
typedef struct ptpd_port_context PtpClock;

typedef struct ptpd_critical_stats_logger PtpCriticalStatsLogger;
typedef struct ptpd_remote_stats_logger PtpRemoteStatsLogger;
typedef struct sfptpd_clustering_evaluator PtpClusteringEvaluator;

typedef struct ptpd_critical_stats {
	bool valid;
	struct sfptpd_timespec sync_time;
	sfptpd_time_t ofm_ns;
	sfptpd_time_t owd_ns;
	long double freq_adj;
} PtpCriticalStats;

typedef struct ptpd_remote_stats {
	const PortIdentity *port_identity;
	struct sockaddr_storage *address;
	socklen_t address_len;
	int domain;
	const PortIdentity *ref_port_identity;
} PtpRemoteStats;

struct ptpd_critical_stats_logger {
	void (*log_fn)(struct ptpd_critical_stats_logger *logger,
		       const struct ptpd_critical_stats critical_stats);
	void *private;
};

struct ptpd_remote_stats_logger {
	void (*log_rx_sync_timing_data_fn)(struct ptpd_remote_stats_logger *logger,
					   const struct ptpd_remote_stats remote_stats,
					   int num_timing_data,
					   SlaveRxSyncTimingDataElement *timing_data);
	void (*log_rx_sync_computed_data_fn)(struct ptpd_remote_stats_logger *logger,
					     const struct ptpd_remote_stats remote_stats,
					     int num_computed_data,
					     SlaveRxSyncComputedDataElement *computeddata);
	void (*log_tx_event_timestamps_fn)(struct ptpd_remote_stats_logger *logger,
					   const struct ptpd_remote_stats remote_stats,
					   ptpd_msg_id_e message_type,
					   int num_timestamps,
					   SlaveTxEventTimestampsElement *timestamps);
	void (*log_slave_status_fn)(struct ptpd_remote_stats_logger *logger,
				    const struct ptpd_remote_stats remote_stats,
				    SlaveStatus *status);
	void *context;
};

typedef struct ptp_servo_counters {
	/* Number of times that the clock has been stepped */
	uint32_t clock_steps;
	/* Number of samples for outliers */
	uint32_t outliers_num_samples;
	/* Number of outliers seen */
	uint32_t outliers;
} ptp_servo_counters_t;

typedef struct ptp_servo {
	/* Magic number to indicate structure validity */
	uint32_t magic;

	/* Handle of the local clock */
	struct sfptpd_clock *clock;

	/* Configuration */
	sfptpd_sync_module_ctrl_flags_t ctrl_flags;
	enum sfptpd_clock_ctrl clock_ctrl;

	/* Flag indicating that the clock has been updated at least once */
	Boolean clock_first_updated;

	int warned_operator_slow_slewing;
	int warned_operator_fast_slewing;

	/* Timestamp set */
	struct sfptpd_ptp_tsd timestamps;

	/* Path delay and offset from master filters */
	struct sfptpd_peirce_filter *peirce_filt;
	struct sfptpd_smallest_filter *smallest_filt;
	struct sfptpd_fir_filter fir_filter;

	/* Filtered offset from master and mean path delay */
	sfptpd_time_t offset_from_master;
	sfptpd_time_t mean_path_delay;

	/* Step threshold */
	LongDouble step_threshold;

	/* PID filter */
	sfptpd_pid_filter_t pid_filter;

	/* Frequency correction and current adjustment */
	LongDouble frequency_correction;
	LongDouble frequency_adjustment;

	/* Critical stats logger */
	struct ptpd_critical_stats_logger *critical_stats_logger;

        /* Clustering evaluator */
        struct sfptpd_clustering_evaluator *clustering_evaluator;

	/* Alarms */
	sfptpd_sync_module_alarms_t alarms;

	/* Counters and statistics */
	struct ptp_servo_counters counters;
} ptp_servo_t;


/**
* \enum Timestamp type enumeration
* \brief Type of timestamping required
*/
typedef enum {
	/** Software timestamps */
	PTPD_TIMESTAMP_TYPE_SW,
	/** Unmodified hardware timestamps */
	PTPD_TIMESTAMP_TYPE_HW,
	/** Automatically select timestamp type. Only valid at configuration
	    stage; should be resolved to one of the others. */
	PTPD_TIMESTAMP_TYPE_AUTO,
} ptpd_timestamp_type_e;

enum ptpd_ts_fmt {
	PTPD_TS_LINUX,
	PTPD_TS_ONLOAD_EXT,
};

/**
* \enum Bad timestamp types enumeration
* \brief Types of bad timestamp cycled through each time signal is sent
* Ordered by how often they add jitter so that they can be
* cycled through with minimal impact on the system.
*/
enum bad_timestamp_types {
	BAD_TIMESTAMP_TYPE_OFF,
	BAD_TIMESTAMP_TYPE_CORRUPTED,
	BAD_TIMESTAMP_TYPE_DEFAULT,
	BAD_TIMESTAMP_TYPE_MILD,
	BAD_TIMESTAMP_TYPE_MAX	/**< Count of types, not a value */
};


/**
* \brief Struct containing interface information and capabilities
 */
typedef struct {
        unsigned int flags;
        unsigned int ifindex;
        int addressFamily;
        Boolean hasHwAddress;
        Boolean hasAfAddress;
        unsigned char hwAddress[14];
        struct sockaddr_storage afAddress;
	socklen_t afAddressLen;
	int ifIndex;
} InterfaceInfo;


/**
 * \struct InterfaceOpts
 * \brief Interface-level options set at run-time
 */
typedef struct ptpd_intf_config {

	Octet ifaceName[IF_NAMESIZE];
	struct sfptpd_interface *physIface;
	sfptpd_clock_id_t clock_id;

	int transportAF;
	Boolean linkLocalScope;

	ptpd_timestamp_type_e timestampType;
	int dscpValue;
	int ttl;

	Boolean masterRefreshIgmp;
	unsigned int masterIgmpRefreshInterval;

	Boolean use_onload_ext;
	Boolean multicast_needed;
	Boolean snmp_enabled; /* SNMP subsystem enabled / disabled even if compiled in */
	Boolean displayPackets;

	/* Access list settings */
	Boolean timingAclEnabled;
	Boolean managementAclEnabled;
	Boolean monitoringAclEnabled;
	char timingAclAllowText[PATH_MAX];
	char timingAclDenyText[PATH_MAX];
	char managementAclAllowText[PATH_MAX];
	char managementAclDenyText[PATH_MAX];
	char monitoringAclAllowText[PATH_MAX];
	char monitoringAclDenyText[PATH_MAX];
	PtpdAclOrder timingAclOrder;
	PtpdAclOrder managementAclOrder;
	PtpdAclOrder monitoringAclOrder;

	char user_description[PTPD_MGMT_USER_DESCRIPTION_MAX + 1];
} InterfaceOpts;

/* @task71778: Configuration properties for Slave Event Monitoring.
 * References are to IEEE1588-Rev draft 1.2 */
typedef struct {
	/* 15.5.3.2.5 SLAVE_EVENT_MONITORING_ENABLE management TLV
	   bits to enable logging and TLV output */
	Boolean logging_enable;
	Boolean tlv_enable;

	/* 16.11.3 DefaultDS.slaveEventMonitoringLoggingingSkip<type>
	 * Skip this many events between samples */
	Integer16 logging_skip;

	/* 16.11.3 DefaultDS.slaveEventMonitoringEventsPer<type>
	 * Include this many samples in each TLV */
	Integer16 events_per_tlv;
} SlaveEventMonitoringConfig;

typedef struct {
	int skip_count;
	int num_events;
	PortIdentity source_port;
} SlaveEventMonitoringState;


/**
 * \struct RunTimeOpts
 * \brief Program options set at run-time
 */
typedef struct ptpd_port_config {

	InterfaceOpts *ifOpts;

	const char *name;
	struct sfptpd_interface *physIface;

	PtpCriticalStatsLogger criticalStatsLogger;
	PtpRemoteStatsLogger remoteStatsLogger;
        PtpClusteringEvaluator clusteringEvaluator;

	Integer8 announceInterval;
	Integer8 announceReceiptTimeout;
	Boolean slaveOnly;
	Boolean masterOnly;
	Integer8 syncInterval;
	Integer8 syncReceiptTimeout;
	Integer8 minDelayReqInterval;
	Integer8 delayRespReceiptTimeout;
	Integer8 minPdelayReqInterval;

	Integer8 delayRespAlarmThreshold;
	Integer8 delayRespHybridThreshold;

	ClockQuality clockQuality;
	TimePropertiesDS timeProperties;
	UInteger8 priority1;
	UInteger8 priority2;
	UInteger8 domainNumber;
	UInteger16 stepsRemoved;

	unsigned int path_delay_filter_size;
	long double path_delay_filter_ageing;
	unsigned int outlier_filter_size;
	long double outlier_filter_adaption;
	unsigned int fir_filter_size;
	long double step_threshold;

	Integer32 maxReset; /* Maximum number of nanoseconds to reset */
	enum sfptpd_clock_ctrl clock_ctrl;

	Octet unicastAddress[MAXHOSTNAMELEN];
	struct sfptpd_timespec inboundLatency, outboundLatency;
	Integer16 max_foreign_records;
	ptpd_delay_mechanism_e delayMechanism;

	Boolean alwaysRespectUtcOffset;
	Boolean preferUtcValid;
	Boolean requireUtcValid;
	Boolean overrideUtcOffset;
	Integer16 overrideUtcOffsetSeconds;
	Boolean missingInterfaceTolerance;
	Boolean ignore_delayreq_interval_master;

	PortCommunicationCapabilities comm_caps;
	Boolean comm_caps_tlv_enabled;
	ptpd_node_type_e node_type;

	long double servoKP;
	long double servoKI;
	long double servoKD;

	Boolean managementEnabled;
	Boolean managementSetEnable;

	Boolean monMeinbergNetSync;

	Boolean delay_resp_ignore_port_id;

	/* Optional features and extensions */
	SlaveEventMonitoringConfig rx_sync_timing_data_config;
	SlaveEventMonitoringConfig rx_sync_computed_data_config;
	SlaveEventMonitoringConfig tx_event_timestamps_config;

	/* @task65531: Slave Status Monitoring (Solarflare extension). */
	bool slave_status_monitoring_enable;
	int num_monitor_dests;
	struct sockaddr_storage monitor_address[MAX_SLAVE_EVENT_DESTS];
	socklen_t monitor_address_len[MAX_SLAVE_EVENT_DESTS];

	/* SWPTP-906: external clock discriminator for BMCA */
	char discriminator_name[SFPTPD_CONFIG_SECTION_NAME_MAX];
	long double discriminator_threshold;

	/* SWPTP-975: PTP profile */
	const struct sfptpd_ptp_profile_def *profile;

	/* SWPTP-591: PTP version */
	UInteger4 ptp_version_minor;

	/* SWPTP-212: User-configured timestamping preference */
	ptpd_timestamp_type_e timestamp_pref;

	/* Test stimuli */
	struct {
		struct {
			enum bad_timestamp_types type;
			int interval_pkts;
			int max_jitter;
		} bad_timestamp;

		struct {
			Boolean enable;
			int max_correction;
		} xparent_clock;

		Boolean no_announce_pkts;
		Boolean no_sync_pkts;
		Boolean no_follow_ups;
		Boolean no_delay_resps;
	} test;
} RunTimeOpts;


/**
* \struct PtpInterface
* \brief State shared between instances on the same interface
*/
/* main program data structure */
struct ptpd_global_context {
	int ports_created;

	/* Linked list of objects representing interfaces at the PTPD level */
	PtpInterface *interfaces;
};

/* A structure containing IP transport information. There is one of these
   per interface object. It is defined separately because different
   types of transport implementation may in future be required so it is
   useful to retain references in the code to this object as distinct
   from the containing interface object. */
struct ptpd_transport {
	/* Socket fds for sending and receiving PTP packets */
	int eventSock;
	int generalSock;

	/* Socket available for sending PTP packets that is not
	   bound to an interface, e.g. for unicast signalling monitoring
	   messages */
	int monitoringSock;

	/* Listening event address */
	struct sockaddr_storage eventAddr;
	socklen_t eventAddrLen;

	/* Listening general address */
	struct sockaddr_storage generalAddr;
	socklen_t generalAddrLen;

	/* Multicast address */
	struct sockaddr_storage multicastAddr;
	socklen_t multicastAddrLen;

	/* Peer multicast address */
	struct sockaddr_storage peerMulticastAddr;
	socklen_t peerMulticastAddrLen;

	/* Interface address and capability descriptor */
	InterfaceInfo interfaceInfo;

	/* used by IGMP refresh */
	struct sockaddr_storage interfaceAddr;
	socklen_t interfaceAddrLen;

	/* Typically MAC address - outer 6 octets of ClockIdentity */
	Octet interfaceID[ETHER_ADDR_LEN];

	/* used for Hybrid mode */
	struct sockaddr_storage lastRecvAddr;
	socklen_t lastRecvAddrLen;

	/* reported to the user */
	char lastRecvHost[NI_MAXHOST];

	uint64_t sentPackets;
	uint64_t receivedPackets;

	/* used for tracking the last TTL set */
	int ttlGeneral;
	int ttlEvent;

	Ipv4AccessList *timingAcl;
	Ipv4AccessList *managementAcl;
	Ipv4AccessList *monitoringAcl;
};

/**
* \struct ptp_intf_counters
* \brief Counters associated with the interface
*/
typedef struct ptp_intf_counters {
	uint32_t discardedMessages;	  /* only messages we shouldn't be receiving - ignored from self don't count */
	uint32_t aclTimingDiscardedMessages;	  /* Timing messages discarded by access lists */
	uint32_t aclManagementDiscardedMessages;	  /* Timing messages discarded by access lists */
	uint32_t messageRecvErrors;	  /* message receive errors */
	uint32_t messageFormatErrors;	  /* headers or messages too short etc. */
	uint32_t versionMismatchErrors;	  /* V1 received, V2 expected - also increments discarded */
	uint32_t domainMismatchErrors;	  /* different domain than configured - also increments discarded */
} ptp_intf_counters_t;

/* Recovered packet timestamp and other associated packet info */
struct sfptpd_ts_info {
	struct sfptpd_timespec sw;
	struct sfptpd_timespec hw;
	unsigned int if_index; /**< physical intf used in transmission or 0 */
	bool have_sw:1;
	bool have_hw:1;
};

/* Information needed to make use of recovered timestamp */
struct sfptpd_ts_user {
	PtpClock *port;
	enum {
		TS_SYNC,
		TS_DELAY_REQ,
		TS_PDELAY_REQ,
		TS_PDELAY_RESP,
		TS_MONITORING_SYNC,
	} type;
	UInteger16 seq_id;
};

/* Ticket for user code to identify timestamp being awaited. */
struct sfptpd_ts_ticket {
	uint32_t seq;
	uint32_t slot;
};

#define TS_MAX_PDU PTPD_PDELAY_REQ_LENGTH

/* Information required for matching a timestamp to the packet awaiting that
 * that timestamp */
struct sfptpd_ts_pkt {
	union {
		struct {
			char data[TS_MAX_PDU];
			size_t len;
			size_t trailer;
		} pdu;
	} match;
	struct sfptpd_ts_user user;
	struct sfptpd_timespec sent_monotime;
	uint64_t seq;
};

#define TS_QUANTILE_E10_MIN -4
#define TS_QUANTILE_E10_MAX 1
#define TS_QUANTILES (TS_QUANTILE_E10_MAX) - (TS_QUANTILE_E10_MIN) + 2
#define TS_TIME_TO_ALARM_E10 0

/* Structure defining short term stats for timestamp cache */
struct sfptpd_ts_stats {
	struct sfptpd_timespec start;
	struct sfptpd_timespec quantile_bounds[TS_QUANTILES];
	unsigned int resolved_quantile[TS_QUANTILES];
	unsigned int pending_quantile[TS_QUANTILES];
	unsigned int evicted;
	unsigned int total;
};

#define TS_CACHE_SIZE (8 * sizeof(unsigned int) / sizeof(uint8_t))

/* Structure holding packets waiting for a timestamp */
struct sfptpd_ts_cache {
	/* Descriptors for packets awaiting timestamp. */
	struct sfptpd_ts_pkt packet[TS_CACHE_SIZE];

	/* Reverse bitmap indicating which cache slots are filled. */
	unsigned int free_bitmap;

	/* Sequence number for cached packets */
	uint64_t seq;

	/* Short term statistics */
	struct sfptpd_ts_stats stats;
};

/**
* \struct PtpInterface
* \brief State shared between instances on the same interface
*/
/* main program data structure */
struct ptpd_intf_context {

	PtpGlobal *global;
	InterfaceOpts ifOpts;
	struct ptpd_transport transport;
	struct sfptpd_interface *interface;
	PtpClock *ports;

	TsMethod tsMethod;
	enum ptpd_ts_fmt ts_fmt;
	struct sfptpd_ts_cache ts_cache;
	struct msghdr msgEbuf;

	/*Foreign node data set*/
	struct sfptpd_hash_table *nodeSet;

	Octet msgIbuf[PACKET_SIZE];
	MsgHeader msgTmpHeader;
	union {
		MsgSync sync;
		MsgFollowUp follow;
		MsgDelayReq req;
		MsgDelayResp resp;
		MsgPDelayReq preq;
		MsgPDelayResp presp;
		MsgPDelayRespFollowUp prespfollow;
		MsgManagement manage;
		MsgAnnounce announce;
		MsgSignaling signaling;
	} msgTmp;
	MsgManagement outgoingManageTmp;

	/* Interface-level timers */
	IntervalTimer itimer[TIMER_ARRAY_SIZE];

	/* These need to be added to port-level counters for reporting */
	struct ptp_intf_counters counters;

	/* Pointer to a struct of the same type in the linked list */
	PtpInterface *next;
};


/**
* \struct PtpClock
* \brief Main program data structure
*/
/* main program data structure */
struct ptpd_port_context {

	PtpInterface *interface;
	RunTimeOpts rtOpts;

	/* Default data set */

	/* Static members */
	Boolean twoStepFlag;
	ClockIdentity clockIdentity;

	/* Dynamic members */
	Boolean boundaryGrandmasterDefined;
	ClockIdentity boundaryGrandmasterIdentity;
	ClockQuality clockQuality;
	struct sfptpd_interface *physIface; /* The preferred physical interface, not necessarily used for PTP traffic */
	struct sfptpd_clock *clock;

	/* Configurable members */
	UInteger8 priority1;
	UInteger8 priority2;
	UInteger8 domainNumber;
	Boolean slaveOnly;
	Boolean masterOnly; /**< 1588-2019 8.2.15.5.2 */

	/* Current data set */

	/* Dynamic members */
	UInteger16 stepsRemoved;

	/* Transparent clock flags */
	Boolean syncXparent;
	Boolean followXparent;
	Boolean delayRespXparent;
	Boolean pDelayRespFollowXparent;

	/* Parent data set */

	/* Dynamic members */
	PortIdentity parentPortIdentity;
	Boolean parentStats;
	UInteger16 observedParentOffsetScaledLogVariance;
	Integer32 observedParentClockPhaseChangeRate;
	ClockIdentity grandmasterIdentity;
	ClockQuality grandmasterClockQuality;
	UInteger8 grandmasterPriority1;
	UInteger8 grandmasterPriority2;
	struct sockaddr_storage parentAddress;
	socklen_t parentAddressLen;

	/* Global time properties data set */
	TimePropertiesDS timePropertiesDS;

	/* Leap second related flags */
	Boolean leapSecondInProgress;
	Boolean leapSecondWaitingForAnnounce;

	/* Port configuration data set */

	/* Static members */
	PortIdentity portIdentity;

	/* Dynamic members */
	ptpd_state_e portState;
	UInteger32 portAlarms;
	Integer8 logMinDelayReqInterval;
	Integer8 logDelayRespReceiptTimeout;
	UInteger32 lastSyncIfindex;

	/* Configurable members */
	Integer8 logAnnounceInterval;
	UInteger8 announceReceiptTimeout;
	Integer8 logSyncInterval;
	UInteger8 syncReceiptTimeout;
	Enumeration8 delayMechanism;
	Integer8 logMinPdelayReqInterval;

	/* Foreign master data set */
	ForeignMasterDS foreign;

	/* Other things we need for the protocol */
	UInteger32 random_seed;
	Boolean record_update;    /* should we run bmc() after receiving an announce message? */

	Octet msgObuf[PACKET_SIZE];

	/* Used to store header so response can be issued more easily */
	MsgHeader PdelayReqHeader;
	MsgHeader delayReqHeader;

	/* Stored packet timestamps */
	struct sfptpd_timespec pdelay_req_send_time;
	struct sfptpd_timespec pdelay_req_receive_time;
	struct sfptpd_timespec pdelay_resp_send_time;
	struct sfptpd_timespec pdelay_resp_receive_time;
	struct sfptpd_timespec pdelay_correction_field;
	struct sfptpd_timespec sync_send_time;
	struct sfptpd_timespec sync_receive_time;
	struct sfptpd_timespec sync_correction_field;
	struct sfptpd_timespec delay_req_send_time;
	struct sfptpd_timespec delay_req_receive_time;
	struct sfptpd_timespec delay_correction_field;

	Boolean sentPDelayReq;
	UInteger16 sentPDelayReqSequenceId;
	UInteger16 sentDelayReqSequenceId;
	UInteger16 sentSyncSequenceId;
	UInteger16 sentAnnounceSequenceId;
	UInteger16 sentSignalingSequenceId;
	UInteger16 recvPDelayReqSequenceId;
	UInteger16 recvSyncSequenceId;
	UInteger16 recvPDelayRespSequenceId;
	Boolean waitingForFollow;
	Boolean waitingForDelayResp;
	Boolean waitingForPDelayResp;
	Boolean waitingForPDelayRespFollow;

	struct sfptpd_ts_ticket sync_ticket;
	struct sfptpd_ts_ticket delayreq_ticket;
	struct sfptpd_ts_ticket pdelayreq_ticket;
	struct sfptpd_ts_ticket pdelayresp_ticket;
	struct sfptpd_ts_ticket monsync_ticket;

	/* how many DelayResps we've failed to receive in a row */
	int sequentialMissingDelayResps;

	/* Used to store a follow-up in case the sync is received out-of-order */
	MsgHeader outOfOrderFollowUpHeader;
	MsgFollowUp outOfOrderFollowUpPayload;

	IntervalTimer itimer[TIMER_ARRAY_SIZE];

	struct ptp_servo servo;
	
	int resetCount;

	char char_last_msg;                             /* representation of last message processed by servo */

	int waiting_for_first_sync;                     /* we'll only start the delayReq timer after the first sync */
	int waiting_for_first_delayresp;                /* Just for information purposes */

	/* management text values are max size + 1 to leave space for a null terminator */
	Octet product_desc[PTPD_MGMT_PRODUCT_DESC_MAX + 1];
	Octet revision_data[PTPD_MGMT_REVISION_DATA_MAX + 1];
	Octet user_description[PTPD_MGMT_USER_DESCRIPTION_MAX + 1];

	/*
	 * counters - useful for debugging and monitoring,
	 * should be exposed through management messages
	 * and SNMP eventually
	 */
	PtpdCounters counters;

	/* How many hybrid mode failures we have seen */
	Integer32   unicast_delay_resp_failures;

	/* The partner's declared (or assumed) capabilities */
	PortCommunicationCapabilities partner_comm_caps;

	/* The effective capabilities based on both sides with impairment */
	PortCommunicationCapabilities effective_comm_caps;

	LongDouble sync_missing_interval;
	LongDouble sync_missing_next_warning;

	struct sockaddr_storage unicastAddr;
	socklen_t unicastAddrLen;

	/* @task70154: address of Meinberg NetSync monitoring station */
	struct sockaddr_storage nsmMonitorAddr;
	socklen_t nsmMonitorAddrLen;

	/* State that is cleared for each packet */
	struct {
		/* @task71437: extension TLV state */
		bool mtie_tlv_requested;
		bool clock_offs_tlv_requested;
		bool port_comm_caps_provided;

		PortCommunicationCapabilities port_comm_caps;
	} transient_packet_state;

	/* task 71437: MTIE window */
	struct {
		Boolean mtie_valid;
		UInteger16 mtie_window_number;
		UInteger16 mtie_window_duration;
		TimeInterval min_offs_from_master;
		TimeInterval max_offs_from_master;
		Timestamp min_offs_from_master_at;
		Timestamp max_offs_from_master_at;
	} mtie_window;

	/* @task71778: Slave Event Monitoring (IEEE1588-Rev draft 16.11.4.1) */
	SlaveRxSyncTimingDataElement slave_rx_sync_timing_data_records[MAX_SLAVE_EVENT_MONITORING_EVENTS_PER_TLV];
	SlaveEventMonitoringState slave_rx_sync_timing_data_state;

	/* @task71778: Slave Event Monitoring (IEEE1588-Rev draft 16.11.4.2) */
	SlaveRxSyncComputedDataElement slave_rx_sync_computed_data_records[MAX_SLAVE_EVENT_MONITORING_EVENTS_PER_TLV];
	SlaveEventMonitoringState slave_rx_sync_computed_data_state;

	/* @task71778: Slave Event Monitoring (IEEE1588-Rev draft 16.11.5.1).
	 * Organised by event message type (1-3) */
	SlaveTxEventTimestampsElement slave_tx_event_timestamps_records[MAX_SLAVE_EVENT_MONITORING_EVENTS_PER_TLV][PTPD_SLAVE_TX_TS_NUM];
	SlaveEventMonitoringState slave_tx_event_timestamps_state[PTPD_SLAVE_TX_TS_NUM];

	/* @SWPTP-906: external clock discriminator for BMCA */
	bool discriminator_valid;
	sfptpd_time_t discriminator_offset;

	/* Compensation for leap second to maintain TAI in master test mode */
	Integer16 fakeUtcAdjustment;

	/* Pointer to a struct of the same type in the linked list */
	PtpClock *next;
};


typedef enum ptpd_tlv_result (*tlv_handler_fn)(const MsgHeader* header,
					       ssize_t length,
					       struct sfptpd_timespec * time,
					       Boolean timestampValid,
					       Boolean isFromSelf,
					       RunTimeOpts *rtOpts,
					       PtpClock *ptpClock,
					       TLV *tlv,
					       size_t tlv_offset);

typedef UInteger16 msgtype_bitmap_t;
typedef UInteger16 acl_bitmap_t;

struct tlv_handling {
	ptpd_tlv_type_e tlv_type;
	UInteger24 organization_id;
	UInteger24 organization_sub_type;
	const char *name;
	msgtype_bitmap_t permitted_message_types_mask;
	acl_bitmap_t required_acl_types_mask;
	tlv_handler_fn pass1_handler_fn;
	tlv_handler_fn pass2_handler_fn;
};


#endif /*DATATYPES_H_*/
