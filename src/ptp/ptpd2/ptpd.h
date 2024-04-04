/* SPDX-License-Identifier: BSD-2-Clause */
/* (c) Copyright 2012-2023 Xilinx, Inc. */
/* (c) Copyright prior contributors */

/**
 * @file   ptpd.h
 * @mainpage Ptpd v2 Documentation
 * @authors Martin Burnicki, Alexandre van Kempen, Steven Kreuzer,
 *          George Neville-Neil
 * @version 2.0
 * @date   Fri Aug 27 10:22:19 2010
 *
 * @section implementation Implementation
 * PTTdV2 is not a full implementation of 1588 - 2008 standard.
 * It is implemented only with use of Transparent Clock and Peer delay
 * mechanism, according to 802.1AS requierements.
 *
 * This header file includes all others headers.
 * It defines functions which are not dependant of the operating system.
 */

#ifndef PTPD_H_
#define PTPD_H_

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <limits.h>
#include <netdb.h>
#include <inttypes.h>
#include <sys/time.h>
#ifndef __APPLE__
#include <sys/timex.h>
#endif
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <syslog.h>
#include <limits.h>
#include <pthread.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/if_ether.h>

#include "constants.h"
#include "limits.h"
#include "dep/ipv4_acl.h"
#include "dep/constants_dep.h"
#include "dep/datatypes_dep.h"
#include "ieee1588_types.h"
#include "datatypes.h"
#include "dep/ptpd_dep.h"

#include "sfptpd_interface.h"
#include "sfptpd_clock.h"
#include "sfptpd_logging.h"
#include "sfptpd_statistics.h"
#include "sfptpd_filter.h"
#include "sfptpd_sync_module.h"

/* NOTE: this macro can be refactored into a function */
#define XMALLOC(ptr,size) \
	if(!((ptr)=malloc(size))) { \
		PERROR("failed to allocate memory"); \
		kill(0, SIGINT); \
		pthread_exit(NULL); \
	}

#define IS_SET(data, bitpos) \
	((data & ( 0x1 << bitpos )) == (0x1 << bitpos))

#define SET_FIELD(data, bitpos) \
	data << bitpos

#define min(a,b)     (((a)<(b))?(a):(b))
#define max(a,b)     (((a)>(b))?(a):(b))


/** \name arith.c
 * -Timing management and arithmetic*/
 /**\{*/
/* arith.c */

/**
 * \brief Convert struct sfptpd_timespec into Timestamp structure (defined by the spec)
 */
int fromInternalTime(const struct sfptpd_timespec * internal,
		     Timestamp * external,
		     TimeInterval *correction);

/**
 * \brief Convert Timestamp to struct sfptpd_timespec structure (defined by the spec)
 */
static inline void toInternalTime(struct sfptpd_timespec * internal, const Timestamp * external)
{
	internal->sec = external->secondsField;
	internal->nsec = external->nanosecondsField;
	internal->nsec_frac = 0;
}

/** \name bmc.c
 * -Best Master Clock Algorithm functions*/
 /**\{*/
/* bmc.c */
/**
 * \brief Compare data set of foreign masters and local data set
 * \return The recommended state for the port
 */
UInteger8 bmc(ForeignMasterDS*, const RunTimeOpts*, PtpClock*);

/**
 * \brief When recommended state is Master, copy local data into parent and grandmaster dataset
 */
void m1(const RunTimeOpts*, PtpClock*);

/**
 * \brief When recommended state is Slave, copy dataset of master into parent and grandmaster dataset
 */
void s1(ForeignMasterRecord*, PtpClock*, const RunTimeOpts *);

void p1(ForeignMasterRecord *master, PtpClock *ptpClock, const RunTimeOpts *rtOpts);

void ptpd_update_announce_interval(PtpClock *ptpClock, const RunTimeOpts *rtOpts);

/**
 * \brief Initialize datas
 */
void initData(RunTimeOpts*, PtpClock*);
/** \}*/


/** \name protocol.c
 * -Execute the protocol engine*/
 /**\{*/
/**
 * \brief Protocol engine
 */
/* protocol.c */
Boolean doInitGlobal(void);
Boolean doInitPort(RunTimeOpts*, PtpClock*);
Boolean doInitInterface(InterfaceOpts*, PtpInterface*);
void doTimerTick(RunTimeOpts *, PtpClock *);
void doHandleSockets(InterfaceOpts *, PtpInterface *, Boolean event, Boolean general, Boolean error);
void toState(ptpd_state_e, RunTimeOpts*, PtpClock*);
void toStateAllPorts(ptpd_state_e state, PtpInterface *ptpInterface);
void handleSendFailure(RunTimeOpts *rtOpts, PtpClock *ptpClock, const char *message);
int initForeignMasterDS(ForeignMasterDS *ds, int max_records);
void resetForeignMasterDS(ForeignMasterDS *ds);
void freeForeignMasterDS(ForeignMasterDS *ds);
int insertIntoForeignMasterDS(MsgHeader *header,
			      MsgAnnounce *announce,
			      PortCommunicationCapabilities *comm_caps,
			      ForeignMasterDS *foreignMasterDS,
			      const struct sockaddr_storage *senderAddr,
			      socklen_t senderAddrLen);
void addForeign(Octet*, size_t length, MsgHeader*, PtpClock*);
void expireForeignMasterRecords(ForeignMasterDS*, const struct sfptpd_timespec *);
Boolean doesForeignMasterEarliestAnnounceQualify(ForeignMasterRecord *, const struct sfptpd_timespec *);
Boolean doesForeignMasterLatestAnnounceQualify(ForeignMasterRecord *, const struct sfptpd_timespec *);
void getForeignMasterExpiryTime(PtpClock *, struct sfptpd_timespec *);
void recordForeignSync(const MsgHeader *header, PtpClock *ptpClock, const struct sfptpd_timespec *timestamp);
void recordForeignFollowUp(const MsgHeader *header, PtpClock *ptpClock, const MsgFollowUp *payload);

/** \}*/

/** \name management.c
 * -Management message support*/
 /**\{*/
/* management.c */
/**
 * \brief Management message support
 */
void managementInit(RunTimeOpts*, PtpClock*);
void managementShutdown(PtpClock*);
void managementInitOutgoingMsg(MsgManagement *incoming, MsgManagement *outgoing, PtpClock *ptpClock);
ptpd_mgmt_error_e handleMMNullManagement(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMClockDescription(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMSlaveOnly(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMUserDescription(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMSaveInNonVolatileStorage(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMResetNonVolatileStorage(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMInitialize(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMDefaultDataSet(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMCurrentDataSet(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMParentDataSet(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMTimePropertiesDataSet(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMPortDataSet(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMPriority1(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMPriority2(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMDomain(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMLogAnnounceInterval(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMAnnounceReceiptTimeout(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMLogSyncInterval(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMVersionNumber(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMEnablePort(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMDisablePort(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMTime(MsgManagement*, MsgManagement*, PtpClock*, RunTimeOpts*);
ptpd_mgmt_error_e handleMMClockAccuracy(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMUtcProperties(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMTraceabilityProperties(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMDelayMechanism(MsgManagement*, MsgManagement*, PtpClock*);
ptpd_mgmt_error_e handleMMLogMinPdelayReqInterval(MsgManagement*, MsgManagement*, PtpClock*);
void handleMMErrorStatus(MsgManagement*);
void handleErrorManagementMessage(MsgManagement *incoming, MsgManagement *outgoing,
				  PtpClock *ptpClock, ptpd_mgmt_error_e errorId);
/** \}*/

/*
 * \brief Packing and Unpacking macros
 */
#define DECLARE_PACK( type ) ssize_t pack##type( void*, void*, size_t );

DECLARE_PACK( NibbleUpper )
DECLARE_PACK( Enumeration4Lower )
DECLARE_PACK( UInteger4Lower )
DECLARE_PACK( UInteger16 )
DECLARE_PACK( UInteger8 )
DECLARE_PACK( Octet )
DECLARE_PACK( Integer8 )
DECLARE_PACK( UInteger24 )
DECLARE_PACK( UInteger48 )
DECLARE_PACK( Integer64 )
DECLARE_PACK( TimeInterval )

#define DECLARE_UNPACK( type ) ssize_t unpack##type( void*, size_t length, void*, PtpClock *ptpClock );

DECLARE_UNPACK( Boolean )
DECLARE_UNPACK( Enumeration4Lower )
DECLARE_UNPACK( Octet )
DECLARE_UNPACK( UInteger24 )
DECLARE_UNPACK( UInteger48 )
DECLARE_UNPACK( Integer64 )
DECLARE_UNPACK( TimeInterval )

//Diplay functions usefull to debug
void displayRunTimeOpts(const RunTimeOpts*);
void displayDefault (PtpClock*);
void displayCurrent (PtpClock*);
void displayParent (PtpClock*);
void displayGlobal (PtpClock*);
void displayPort (PtpClock*);
void displayForeignMaster (PtpClock*);
void displayForeignMasterRecords (ForeignMasterDS *,const struct sfptpd_timespec *);
void displayOthers (PtpClock*);
void displayBuffer (PtpClock*);
void displayPtpClock (PtpClock*);
void timespec_display(const struct timespec *);
void clockIdentity_display(const ClockIdentity);
void address_display(const char *key,
		     const struct sockaddr_storage *address,
		     socklen_t length, Boolean verbose);
void netPath_display(const struct ptpd_transport*, const PtpClock*);
void intervalTimer_display(const IntervalTimer*);
void integer64_display (const char *, const Integer64*);
void timeInterval_display(const struct sfptpd_timespec*);
void portIdentity_display(const PortIdentity*);
void clockQuality_display (const ClockQuality*);
void PTPText_display(const PTPText*, const PtpClock*);
void iFaceName_display(const Octet*);
void unicast_display(const Octet*);
const char *portState_getName(Enumeration8 portState);
void timestamp_display(const Timestamp * timestamp);

void displayCounters(PtpClock*);
void displayStatistics(PtpClock*);

void msgHeader_display(const MsgHeader*);
void msgAnnounce_display(const MsgAnnounce*);
void msgSync_display(const MsgSync *sync);
void msgFollowUp_display(const MsgFollowUp*);
void msgPDelayReq_display(const MsgPDelayReq*);
void msgDelayReq_display(const MsgDelayReq * req);
void msgDelayResp_display(const MsgDelayResp * resp);
void msgPDelayResp_display(const MsgPDelayResp * presp);
void msgPDelayRespFollowUp_display(const MsgPDelayRespFollowUp * prespfollow);
void msgSignaling_display(const MsgSignaling * signaling);
void msgManagement_display(const MsgManagement * manage);
 

void mMSlaveOnly_display(const MMSlaveOnly*, const PtpClock*);
void mMClockDescription_display(const MMClockDescription*, const PtpClock*);
void mMUserDescription_display(const MMUserDescription*, const PtpClock*);
void mMInitialize_display(const MMInitialize*, const PtpClock*);
void mMDefaultDataSet_display(const MMDefaultDataSet*, const PtpClock*);
void mMCurrentDataSet_display(const MMCurrentDataSet*, const PtpClock*);
void mMParentDataSet_display(const MMParentDataSet*, const PtpClock*);
void mMTimePropertiesDataSet_display(const MMTimePropertiesDataSet*, const PtpClock*);
void mMPortDataSet_display(const MMPortDataSet*, const PtpClock*);
void mMPriority1_display(const MMPriority1*, const PtpClock*);
void mMPriority2_display(const MMPriority2*, const PtpClock*);
void mMDomain_display(const MMDomain*, const PtpClock*);
void mMLogAnnounceInterval_display(const MMLogAnnounceInterval*, const PtpClock*);
void mMAnnounceReceiptTimeout_display(const MMAnnounceReceiptTimeout*, const PtpClock*);
void mMLogSyncInterval_display(const MMLogSyncInterval*, const PtpClock*);
void mMVersionNumber_display(const MMVersionNumber*, const PtpClock*);
void mMTime_display(const MMTime*, const PtpClock*);
void mMClockAccuracy_display(const MMClockAccuracy*, const PtpClock*);
void mMUtcProperties_display(const MMUtcProperties*, const PtpClock*);
void mMTraceabilityProperties_display(const MMTraceabilityProperties*, const PtpClock*);
void mMDelayMechanism_display(const MMDelayMechanism*, const PtpClock*);
void mMLogMinPdelayReqInterval_display(const MMLogMinPdelayReqInterval*, const PtpClock*);
void mMErrorStatus_display(const MMErrorStatus*, const PtpClock*);

/**\brief Initialize outgoing signaling message fields*/
void signalingInitOutgoingMsg(MsgSignaling *outgoing,
			      PtpClock *ptpClock);

void ingressEventMonitor(PtpClock *ptpClock, RunTimeOpts *rtOpts);
void egressEventMonitor(PtpClock *ptpClock, RunTimeOpts *rtOpts, ptpd_msg_id_e type, const struct sfptpd_timespec *time);
void slaveStatusMonitor(PtpClock *ptpClock, RunTimeOpts *rtOpts,
			int missingMessageAlarms, int otherAlarms, int events, int flags);

enum ptpd_tlv_result
slave_rx_sync_timing_data_handler(const MsgHeader *header, ssize_t length,
				  struct sfptpd_timespec *time, Boolean timestampValid,
				  RunTimeOpts *rtOpts, PtpClock *ptpClock,
				  TLV *tlv, size_t tlv_offset);

enum ptpd_tlv_result
slave_rx_sync_computed_data_handler(const MsgHeader *header, ssize_t length,
				    struct sfptpd_timespec *time, Boolean timestampValid,
				    RunTimeOpts *rtOpts, PtpClock *ptpClock,
				    TLV *tlv, size_t tlv_offset);

enum ptpd_tlv_result
slave_tx_event_timestamps_handler(const MsgHeader *header, ssize_t length,
				  struct sfptpd_timespec *time, Boolean timestampValid,
				  RunTimeOpts *rtOpts, PtpClock *ptpClock,
				  TLV *tlv, size_t tlv_offset);

enum ptpd_tlv_result
slave_status_handler(const MsgHeader *header, ssize_t length,
		     struct sfptpd_timespec *time, Boolean timestampValid,
		     RunTimeOpts *rtOpts, PtpClock *ptpClock,
		     TLV *tlv, size_t tlv_offset);

#endif /*PTPD_H_*/
