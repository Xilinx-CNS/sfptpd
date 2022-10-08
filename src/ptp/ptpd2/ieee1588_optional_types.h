/* SPDX-License-Identifier: BSD-2-Clause */
/* (c) Copyright 2017-2019 Xilinx, Inc. */

#ifndef IEEE1588_OPTIONAL_TYPES_H_
#define IEEE1588_OPTIONAL_TYPES_H_


/**
*\file
* \brief Data structures defined in optional features of IEEE1588.
*/

typedef struct {
#define OPERATE( name, size, type ) type name;
	#include "def/optional/slave_rx_sync_timing_data_element.def"
} SlaveRxSyncTimingDataElement;

typedef struct {
#define OPERATE( name, size, type ) type name;
	#include "def/optional/slave_rx_sync_timing_data.def"
} SlaveRxSyncTimingData;

typedef struct {
#define OPERATE( name, size, type ) type name;
	#include "def/optional/slave_rx_sync_computed_data_element.def"
} SlaveRxSyncComputedDataElement;

typedef struct {
#define OPERATE( name, size, type ) type name;
	#include "def/optional/slave_rx_sync_computed_data.def"
} SlaveRxSyncComputedData;

typedef struct {
#define OPERATE( name, size, type ) type name;
	#include "def/optional/slave_tx_event_timestamps_element.def"
} SlaveTxEventTimestampsElement;

typedef struct {
#define OPERATE( name, size, type ) type name;
	#include "def/optional/slave_tx_event_timestamps.def"
} SlaveTxEventTimestamps;

typedef struct {
#define OPERATE( name, size, type) type name;
	#include "def/optional/port_communication_capabilities.def"
} PortCommunicationCapabilities;

/* Augmented structures to hold unpacked repeated elements */

typedef struct {
	SlaveRxSyncTimingData preamble;
	int num_elements;
	SlaveRxSyncTimingDataElement *elements;
} SlaveRxSyncTimingDataTLV;

typedef struct {
	SlaveRxSyncComputedData preamble;
	int num_elements;
	SlaveRxSyncComputedDataElement *elements;
} SlaveRxSyncComputedDataTLV;

typedef struct {
	SlaveTxEventTimestamps preamble;
	int num_elements;
	SlaveTxEventTimestampsElement *elements;
} SlaveTxEventTimestampsTLV;

#endif /*IEEE1588_OPTIONAL_TYPES_H_*/
