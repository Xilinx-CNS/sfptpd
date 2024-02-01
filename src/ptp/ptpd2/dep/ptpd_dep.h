/* SPDX-License-Identifier: BSD-2-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */
/* (c) Copyright prior contributors */

/**
 * @file   ptpd_dep.h
 *
 * @brief  External definitions for inclusion elsewhere.
 *
 *
 */

#ifndef PTPD_DEP_H_
#define PTPD_DEP_H_

#include <assert.h>
#include <endian.h>

#define PERROR(x, ...) ERROR(x " (strerror: %m)\n", ##__VA_ARGS__)

#define DBG(x, ...)  TRACE(SFPTPD_COMPONENT_ID_PTPD2, 1, x, ##__VA_ARGS__)
#define DBG2(x, ...) TRACE(SFPTPD_COMPONENT_ID_PTPD2, 2, x, ##__VA_ARGS__)
#define DBGV(x, ...) TRACE(SFPTPD_COMPONENT_ID_PTPD2, 3, x, ##__VA_ARGS__)

#ifdef PTPD_DUMP
#define DUMP(text, addr, len) dump(text, addr, len)
#else
#define DUMP(text, addr, len)
#endif

/** \name Endian corrections*/
 /**\{*/

#if defined(PTPD_MSBF)
#define shift8(x,y)   ( (x) << ((3-y)<<3) )
#define shift16(x,y)  ( (x) << ((1-y)<<4) )
#elif defined(PTPD_LSBF)
#define shift8(x,y)   ( (x) << ((y)<<3) )
#define shift16(x,y)  ( (x) << ((y)<<4) )
#endif

#define flip16(x) htons(x)
#define flip32(x) htonl(x)
#define flip64(x) htobe64(x)

/** \}*/


/** \name Bit array manipulations*/
 /**\{*/

#define getFlag(x,y)  !!( *(UInteger8*)((x)+((y)<8?1:0)) &   (1<<((y)<8?(y):(y)-8)) )
#define setFlag(x,y)    ( *(UInteger8*)((x)+((y)<8?1:0)) |=   1<<((y)<8?(y):(y)-8)  )
#define clearFlag(x,y)  ( *(UInteger8*)((x)+((y)<8?1:0)) &= ~(1<<((y)<8?(y):(y)-8)) )
/** \}*/

/** \name msg.c
 *-Pack and unpack PTP messages */
 /**\{*/

ssize_t msgUnpackHeader(Octet *buf, size_t length, MsgHeader*);
ssize_t msgUnpackAnnounce (Octet *buf, size_t length, MsgAnnounce*);
ssize_t msgUnpackSync(Octet *buf, size_t length, MsgSync*);
ssize_t msgUnpackFollowUp(Octet *buf, size_t length, MsgFollowUp*);
ssize_t msgUnpackDelayReq(Octet *buf, size_t length, MsgDelayReq*);
ssize_t msgUnpackDelayResp(Octet *buf, size_t length, MsgDelayResp*);
ssize_t msgUnpackPDelayReq(Octet *buf, size_t length, MsgPDelayReq*);
ssize_t msgUnpackPDelayResp(Octet *buf, size_t length, MsgPDelayResp*);
ssize_t msgUnpackPDelayRespFollowUp(Octet *buf, size_t length, MsgPDelayRespFollowUp*);
ssize_t msgUnpackManagement(Octet *buf, size_t length, MsgManagement*, MsgHeader*, PtpClock*);
ssize_t msgUnpackSignaling(Octet *buf, size_t length, MsgSignaling * signaling, MsgHeader * header, PtpClock *ptpClock);
ssize_t msgUnpackTLVHeader(Octet *buf, size_t length, TLV *tlv, PtpClock* ptpClock);
ssize_t msgPackHeader(Octet *buf, size_t space, PtpClock*, unsigned int);
ssize_t msgPackAnnounce(Octet *buf, size_t space, PtpClock*);
ssize_t msgPackSync(Octet *buf, size_t space, PtpClock*);
ssize_t msgPackFollowUp(Octet *buf, size_t space, const struct sfptpd_timespec*, PtpClock*, const UInteger16);
ssize_t msgPackDelayReq(Octet *buf, size_t space, PtpClock*);
ssize_t msgPackDelayResp(Octet *buf, size_t space, MsgHeader*, const struct sfptpd_timespec *, PtpClock*);
ssize_t msgPackPDelayReq(Octet *buf, size_t space, PtpClock*);
ssize_t msgPackPDelayResp(Octet *buf, size_t space, MsgHeader*, const struct sfptpd_timespec *, PtpClock*);
ssize_t msgPackPDelayRespFollowUp(Octet *buf, size_t space, MsgHeader*, const struct sfptpd_timespec*, PtpClock*, const UInteger16);
ssize_t msgPackManagement(Octet *buf, size_t space, MsgManagement*, PtpClock*);
ssize_t msgPackManagementRespAck(Octet *buf, size_t space, MsgManagement*, PtpClock*);
ssize_t msgPackManagementTLV(Octet *buf, size_t space, MsgManagement*, PtpClock*);
ssize_t msgPackManagementErrorStatusTLV(Octet *buf, size_t space, MsgManagement*, PtpClock*);
void msgUpdateHeaderSequenceId(Octet *buf, UInteger16 sequenceId);
void msgUpdateHeaderFlags(Octet *buf, UInteger8 mask, UInteger8 value);

void freeMMErrorStatusTLV(ManagementTLV*);
void freeMMTLV(ManagementTLV*);

void msgDump(PtpInterface *ptpInterface);

void copyClockIdentity( ClockIdentity dest, ClockIdentity src);
void copyPortIdentity( PortIdentity * dest, PortIdentity * src);

ssize_t unpackMsgSignaling(Octet *buf, size_t length, MsgSignaling*, PtpClock*);
ssize_t packMsgSignaling(MsgSignaling*, Octet *, size_t);

ssize_t unpackMsgManagement(Octet *buf, size_t length, MsgManagement*, PtpClock*);
ssize_t packMsgManagement(MsgManagement*, Octet *, size_t);
ssize_t unpackManagementTLV(Octet *buf, size_t length, MsgManagement*, PtpClock*);
ssize_t packManagementTLV(ManagementTLV*, Octet *, size_t);
void freeManagementTLV(MsgManagement*);
ssize_t unpackMMClockDescription( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMClockDescription( MsgManagement*, Octet *, size_t);
void freeMMClockDescription( MMClockDescription*);
ssize_t unpackMMUserDescription( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMUserDescription( MsgManagement*, Octet *, size_t);
void freeMMUserDescription( MMUserDescription*);
ssize_t unpackMMErrorStatus( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMErrorStatus( MsgManagement*, Octet *, size_t);
void freeMMErrorStatus( MMErrorStatus*);
ssize_t unpackMMInitialize( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMInitialize( MsgManagement*, Octet *, size_t);
ssize_t unpackMMDefaultDataSet( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMDefaultDataSet( MsgManagement*, Octet *, size_t);
ssize_t unpackMMCurrentDataSet( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMCurrentDataSet( MsgManagement*, Octet *, size_t);
ssize_t unpackMMParentDataSet( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMParentDataSet( MsgManagement*, Octet *, size_t);
ssize_t unpackMMTimePropertiesDataSet( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMTimePropertiesDataSet( MsgManagement*, Octet *, size_t);
ssize_t unpackMMPortDataSet( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMPortDataSet( MsgManagement*, Octet *, size_t);
ssize_t unpackMMPriority1( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMPriority1( MsgManagement*, Octet *, size_t);
ssize_t unpackMMPriority2( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMPriority2( MsgManagement*, Octet *, size_t);
ssize_t unpackMMDomain( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMDomain( MsgManagement*, Octet *, size_t);
ssize_t unpackMMSlaveOnly( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMSlaveOnly( MsgManagement*, Octet *, size_t);
ssize_t unpackMMLogAnnounceInterval( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMLogAnnounceInterval( MsgManagement*, Octet *, size_t);
ssize_t unpackMMAnnounceReceiptTimeout( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMAnnounceReceiptTimeout( MsgManagement*, Octet *, size_t);
ssize_t unpackMMLogSyncInterval( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMLogSyncInterval( MsgManagement*, Octet *, size_t);
ssize_t unpackMMVersionNumber( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMVersionNumber( MsgManagement*, Octet *, size_t);
ssize_t unpackMMTime( Octet *buf, size_t length, MsgManagement*, PtpClock * );
ssize_t packMMTime( MsgManagement*, Octet *, size_t);
ssize_t unpackMMClockAccuracy( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMClockAccuracy( MsgManagement*, Octet *, size_t);
ssize_t unpackMMUtcProperties( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMUtcProperties( MsgManagement*, Octet *, size_t);
ssize_t unpackMMTraceabilityProperties( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMTraceabilityProperties( MsgManagement*, Octet *, size_t);
ssize_t unpackMMDelayMechanism( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMDelayMechanism( MsgManagement*, Octet *, size_t);
ssize_t unpackMMLogMinPdelayReqInterval( Octet *buf, size_t length, MsgManagement*, PtpClock* );
ssize_t packMMLogMinPdelayReqInterval( MsgManagement*, Octet *, size_t);


ssize_t unpackPortAddress( Octet *buf, size_t length, PortAddress*, PtpClock*);
ssize_t packPortAddress( PortAddress*, Octet *, size_t);
void freePortAddress( PortAddress*);
ssize_t unpackPTPText( Octet *buf, size_t length, PTPText*, PtpClock*);
ssize_t packPTPText( PTPText*, Octet *, size_t);
void freePTPText( PTPText*);
ssize_t unpackPhysicalAddress( Octet *buf, size_t length, PhysicalAddress*, PtpClock*);
ssize_t packPhysicalAddress( PhysicalAddress*, Octet *, size_t);
void freePhysicalAddress( PhysicalAddress*);
ssize_t unpackClockIdentity( Octet *buf, size_t length, ClockIdentity *c, PtpClock*);
ssize_t packClockIdentity( ClockIdentity *c, Octet *, size_t);
void freeClockIdentity( ClockIdentity *c);
ssize_t unpackClockQuality( Octet *buf, size_t length, ClockQuality *c, PtpClock*);
ssize_t packClockQuality( ClockQuality *c, Octet *, size_t);
void freeClockQuality( ClockQuality *c);
ssize_t unpackPortIdentity( Octet *buf, size_t length, PortIdentity *p, PtpClock*);
ssize_t packPortIdentity( PortIdentity *p, Octet *, size_t);
void freePortIdentity( PortIdentity *p);
ssize_t unpackTimestamp( Octet *buf, size_t length, Timestamp *t, PtpClock*);
ssize_t packTimestamp( Timestamp *t, Octet *, size_t);
void freeTimestamp( Timestamp *t);
UInteger16 msgPackManagementResponse(Octet * buf,MsgHeader*,MsgManagement*,PtpClock*);
/** \}*/
ssize_t msgUnpackOrgTLVSubHeader(Octet *buf, size_t length,
				 UInteger24 *org_id, UInteger24 *org_subtype,
				 PtpClock* ptpClock);

UInteger16 getHeaderLength(Octet *buf);

ssize_t unpackSlaveRxSyncTimingDataTLV(Octet *buf, size_t length,
				       SlaveRxSyncTimingDataTLV *data,
				       PtpClock*);
void freeSlaveRxSyncTimingDataTLV(SlaveRxSyncTimingDataTLV *tlv);

ssize_t unpackSlaveRxSyncComputedDataTLV(Octet *buf, size_t length,
				       SlaveRxSyncComputedDataTLV *data,
				       PtpClock*);
void freeSlaveRxSyncComputedDataTLV(SlaveRxSyncComputedDataTLV *tlv);
ssize_t unpackSlaveTxEventTimestampsTLV(Octet *buf, size_t length,
				       SlaveTxEventTimestampsTLV *data,
				       PtpClock*);
void freeSlaveTxEventTimestampsTLV(SlaveTxEventTimestampsTLV *tlv);
ssize_t unpackSlaveStatus(Octet *buf, size_t length, SlaveStatus *data, PtpClock *ptpClock);
ssize_t unpackPortCommunicationCapabilities( Octet *buf, size_t length, PortCommunicationCapabilities *data, PtpClock *ptpClock);

ssize_t appendPTPMonRespTLV(PTPMonRespTLV *data, Octet *buf, size_t space);
ssize_t appendMTIERespTLV(MTIERespTLV *data, Octet *buf, size_t space);
ssize_t appendSlaveRxSyncTimingDataTLV(SlaveRxSyncTimingDataTLV *data,
				       Octet *buf, size_t space);
ssize_t appendSlaveRxSyncComputedDataTLV(SlaveRxSyncComputedData *data,
					 SlaveRxSyncComputedDataElement *elements,
					 int num_elements,
					 Octet *buf, size_t space);
ssize_t appendSlaveTxEventTimestampsTLV(SlaveTxEventTimestamps *data,
					SlaveTxEventTimestampsElement *elements,
					int num_elements,
					Octet *buf, size_t space);
ssize_t appendSlaveStatusTLV(SlaveStatus *data, Octet *buf, size_t space);
ssize_t appendPortCommunicationCapabilitiesTLV(PortCommunicationCapabilities *data,
					       Octet *buf, size_t space);

void populateCurrentDataSet(MMCurrentDataSet *data, PtpClock *ptpClock);
void populateParentDataSet(MMParentDataSet *data, PtpClock *ptpClock);
void populateTimePropertiesDataSet(MMTimePropertiesDataSet *data, PtpClock *ptpClock);

/** \name net.c (Unix API dependent)
 * -Init network stuff, send and receive datas*/
 /**\{*/

static inline bool sfptpd_ts_is_ticket_valid(const struct sfptpd_ts_ticket ticket)
{
	return ticket.slot != TS_CACHE_SIZE;
}

static const struct sfptpd_ts_ticket TS_NULL_TICKET = {
	.slot = TS_CACHE_SIZE,
	.seq = 0,
};

void formatTsPkt(struct sfptpd_ts_user *pkt, char desc[48]);

Boolean testInterface(char* ifaceName);
Boolean netInit(struct ptpd_transport*,InterfaceOpts*,PtpInterface*);
Boolean netInitPort(PtpClock *ptpClock, RunTimeOpts *rtOpts);
Boolean netShutdown(struct ptpd_transport*);
int netSelect(struct sfptpd_timespec*,struct ptpd_transport*,fd_set*);
ssize_t netRecvError(PtpInterface *ptpInterface);
ssize_t netRecvEvent(Octet*,PtpInterface*,struct sfptpd_ts_info*);
ssize_t netRecvGeneral(Octet*,struct ptpd_transport*);
void netCheckTimestampStats(struct sfptpd_ts_cache *cache);
bool netCheckTimestampAlarms(PtpClock *ptpClock);

/* These functions all return 0 for success or an errno in the case of failure */
int netSendEvent(Octet*,UInteger16,PtpClock*,RunTimeOpts*,const struct sockaddr_storage*,socklen_t);
int netSendGeneral(Octet*,UInteger16,PtpClock*,RunTimeOpts*,const struct sockaddr_storage*,socklen_t);
int netSendMonitoring(Octet*,UInteger16,PtpClock*,RunTimeOpts*,const struct sockaddr_storage*,socklen_t);
int netSendPeerGeneral(Octet*,UInteger16,PtpClock*);
int netSendPeerEvent(Octet*,UInteger16,PtpClock*,RunTimeOpts*);

bool netProcessError(PtpInterface *ptpInterface,
		     size_t length,
		     struct sfptpd_ts_user *user,
		     struct sfptpd_ts_ticket *ticket,
		     struct sfptpd_ts_info *info);

static inline size_t getTrailerLength(PtpClock *ptpClock)
{
	/* IPv6 packets are sent with two extra bytes according
	 * to Annex E. These are not included in the pdulen
	 * passed to the timestamp matcher. */
	return ptpClock->interface->ifOpts.transportAF == AF_INET6 ? 2 : 0;
}

/* The type of timestamp used for PTP must match the clock type being used
 * as the local reference clock.
 *   system clock -> use software timestamps
 *   NIC clock -> use hardware timestamps
 */
static inline bool is_suitable_timestamp(PtpInterface *ptpInterface,
					 struct sfptpd_ts_info *info)
{
	switch (ptpInterface->ifOpts.timestampType) {
	case PTPD_TIMESTAMP_TYPE_HW:
		return info->have_hw;
	case PTPD_TIMESTAMP_TYPE_SW:
		return info->have_sw;
	default:
		return false;
	}
}

static inline struct sfptpd_timespec *get_suitable_timestamp(PtpInterface *ptpInterface,
				      struct sfptpd_ts_info *info)
{
	switch (ptpInterface->ifOpts.timestampType) {
	case PTPD_TIMESTAMP_TYPE_HW:
		return &info->hw;
	case PTPD_TIMESTAMP_TYPE_SW:
		return &info->sw;
	default:
		return false;
	}
}

struct sfptpd_ts_ticket netExpectTimestamp(struct sfptpd_ts_cache *cache,
					   struct sfptpd_ts_user *user,
					   Octet *pkt_data,
					   size_t pkt_len,
					   size_t trailer);

Boolean netRefreshIGMP(struct ptpd_transport *, InterfaceOpts *, PtpInterface *);
Boolean hostLookup(const char* hostname, Integer32* addr);


/** \}*/

#if defined PTPD_SNMP
/** \name snmp.c (SNMP subsystem)
 * -Handle SNMP subsystem*/
 /**\{*/

void snmpInit(RunTimeOpts *, PtpClock *);
void snmpShutdown();

/** \}*/
#endif

/** \name servo.c
 * -Clock servo*/
 /**\{*/

bool servo_init(const RunTimeOpts *, ptp_servo_t *, struct sfptpd_clock *);
void servo_shutdown(ptp_servo_t *);
sfptpd_sync_module_alarms_t servo_get_alarms(ptp_servo_t *);
void servo_reset(ptp_servo_t *);
void servo_pid_adjust(const RunTimeOpts *rtOpts, ptp_servo_t *servo, bool reset);
void servo_reset_operator_messages(ptp_servo_t *);
void servo_set_slave_clock(ptp_servo_t *, struct sfptpd_clock *);
void servo_set_interval(ptp_servo_t *, long double interval);

void servo_control(ptp_servo_t *, sfptpd_sync_module_ctrl_flags_t ctrl_flags);

void servo_step_clock(ptp_servo_t *, struct sfptpd_timespec *);

sfptpd_time_t servo_get_offset_from_master(ptp_servo_t *);
struct sfptpd_timespec servo_get_time_of_last_offset(ptp_servo_t *);
sfptpd_time_t servo_get_mean_path_delay(ptp_servo_t *);
long double servo_get_p_term(ptp_servo_t *servo);
long double servo_get_i_term(ptp_servo_t *servo);
long double servo_get_outlier_threshold(ptp_servo_t *servo);
long double servo_get_frequency_adjustment(ptp_servo_t *);
void servo_reset_counters(ptp_servo_t *);
void servo_get_counters(ptp_servo_t *, ptp_servo_counters_t *);

void servo_missing_s2m_ts(ptp_servo_t *);
void servo_missing_p2p_ts(ptp_servo_t *);
void servo_missing_m2s_ts(ptp_servo_t *);

bool servo_provide_s2m_ts(ptp_servo_t *,
			  struct sfptpd_timespec *send_time,
			  struct sfptpd_timespec *recv_time,
			  struct sfptpd_timespec *correction);
bool servo_provide_p2p_ts(ptp_servo_t *,
			  struct sfptpd_timespec *req_send_time,
			  struct sfptpd_timespec *req_recv_time,
			  struct sfptpd_timespec *resp_send_time,
			  struct sfptpd_timespec *resp_recv_time,
			  struct sfptpd_timespec *correction);
bool servo_provide_m2s_ts(ptp_servo_t *,
			  struct sfptpd_timespec *send_time,
			  struct sfptpd_timespec *recv_time,
			  struct sfptpd_timespec *correction);

void servo_update_clock(ptp_servo_t *);



/** \}*/

/** \name sys.c (Unix API dependent)
 * -Manage timing system API*/
 /**\{*/

void dump(const char *text, void *addr, int len);
void displayStatus(PtpClock *ptpClock, const char *prefixMessage);
void displayPortIdentity(PtpClock *ptpClock,
			 PortIdentity *port, const char *prefixMessage);
void getTime(struct sfptpd_timespec*);
long double getRand(void);
#if defined(MOD_TAI) &&  NTP_API == 4
void setKernelUtcOffset(int utc_offset);
#endif /* MOD_TAI */

/** \}*/


/** \name timer.c (Unix API dependent)
 * -Handle with timers*/
 /**\{*/
void initTimer(void);
void timerTick(IntervalTimer*);
void timerStop(UInteger16,IntervalTimer*);

/* R135 patch: we went back to floating point periods (for less than 1s )*/
void timerStart(UInteger16 index, long double interval, IntervalTimer * itimer);

/* Version with randomized backoff */
void timerStart_random(UInteger16 index, long double interval, IntervalTimer * itimer);

Boolean timerExpired(UInteger16,IntervalTimer*);
Boolean timerStopped(UInteger16,IntervalTimer*);
Boolean timerRunning(UInteger16,IntervalTimer*);

/** \}*/

/* Transport-independent address copy */
void copyAddress(struct sockaddr_storage *destAddr,
		 socklen_t *destLen,
		 const struct sockaddr_storage *srcAddr,
		 socklen_t srcLen);

/* Compare addresses */
Boolean hostAddressesEqual(const struct sockaddr_storage *addressA,
			   socklen_t lengthA,
			   const struct sockaddr_storage *addressB,
			   socklen_t lengthB);

/* Transport-independent IEEE1588 protocol address write */
void writeProtocolAddress(PortAddress *protocolAddress,
			  const struct sockaddr_storage *address,
			  socklen_t length);

#endif /*PTPD_DEP_H_*/
