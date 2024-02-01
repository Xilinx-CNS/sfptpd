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
 * @file   msg.c
 * @author George Neville-Neil <gnn@neville-neil.com>
 * @date   Tue Jul 20 16:17:05 2010
 *
 * @brief  Functions to pack and unpack messages.
 *
 * See spec annex d
 */

#include "../ptpd.h"

extern RunTimeOpts rtOpts;

extern inline bool UNPACK_OK(ssize_t result);
extern inline size_t UNPACK_GET_SIZE(ssize_t result);
extern inline ssize_t UNPACK_SIZE(size_t size);
extern inline bool PACK_OK(ssize_t result);
extern inline size_t PACK_GET_SIZE(ssize_t result);
extern inline ssize_t PACK_SIZE(size_t size);

static void msgDebugHeader(MsgHeader *header, const char *time);
static void msgDebugSync(MsgSync *sync, const char *time);
static void msgDebugAnnounce(MsgAnnounce *announce, const char *time);
static void msgDebugDelayReq(MsgDelayReq *req, const char *time);
static void msgDebugFollowUp(MsgFollowUp *follow, const char *time);
static void msgDebugDelayResp(MsgDelayResp *resp, const char *time);
static void msgDebugPDelayReq(MsgPDelayReq *req, const char *time);
static void msgDebugPDelayResp(MsgPDelayResp *resp, const char *time);
static void msgDebugPDelayRespFollowUp(MsgPDelayRespFollowUp *follow, const char *time);
static void msgDebugManagement(MsgManagement *manage, const char *time);

#define PACK_SIMPLE( type ) \
ssize_t pack##type( void* from, void* to, size_t space )	\
{ \
	*(type *)to = *(type *)from;					\
	return PACK_SIZE(sizeof(type));					\
}									\
ssize_t unpack##type( void* from, size_t length, void* to, PtpClock *ptpClock ) \
{ \
	pack##type( from, to, length );		\
	return UNPACK_SIZE(sizeof(type));	\
}

#define PACK_ENDIAN( type, size )				\
ssize_t pack##type( void* from, void* to, size_t space )	\
{								\
	*(type *)to = flip##size( *(type *)from );	\
	return PACK_SIZE(sizeof(type));			\
} \
ssize_t unpack##type( void* from, size_t length, void* to, PtpClock *ptpClock ) \
{ \
	pack##type( from, to, length );		\
	return UNPACK_SIZE(sizeof(type));	\
}

#define PACK_LOWER_AND_UPPER( type ) \
ssize_t pack##type##Lower( void* from, void* to, size_t space ) \
{ \
	*(char *)to = *(char *)to & 0xF0; \
	*(char *)to = *(char *)to | *(type *)from; \
	return PACK_SIZE(sizeof(type));		   \
} \
\
ssize_t pack##type##Upper( void* from, void* to, size_t space) \
{ \
	*(char *)to = *(char *)to & 0x0F; \
	*(char *)to = *(char *)to | (*(type *)from << 4); \
	return PACK_SIZE(sizeof(type));			  \
} \
\
ssize_t unpack##type##Lower( void* from, size_t length, void* to, PtpClock *ptpClock ) \
{ \
	*(type *)to = *(char *)from & 0x0F; \
	return UNPACK_SIZE(sizeof(type));   \
} \
\
ssize_t unpack##type##Upper( void* from, size_t length, void* to, PtpClock *ptpClock ) \
{ \
        *(type *)to = (*(char *)from >> 4) & 0x0F; \
	return UNPACK_SIZE(sizeof(type));	   \
}

PACK_SIMPLE( Boolean )
PACK_SIMPLE( UInteger8 )
PACK_SIMPLE( Octet )
PACK_SIMPLE( Enumeration8 )
PACK_SIMPLE( Integer8 )

PACK_ENDIAN( Enumeration16, 16 )
PACK_ENDIAN( Integer16, 16 )
PACK_ENDIAN( UInteger16, 16 )
PACK_ENDIAN( Integer32, 32 )
PACK_ENDIAN( UInteger32, 32 )
PACK_ENDIAN( Integer64, 64 )
PACK_ENDIAN( TimeInterval, 64 )

PACK_LOWER_AND_UPPER( Enumeration4 )
PACK_LOWER_AND_UPPER( UInteger4 )
PACK_LOWER_AND_UPPER( Nibble )

/* The free function is intentionally empty. However, this simplifies
 * the procedure to deallocate complex data types
 */
#define FREE( type ) \
void free##type( void* x) \
{}

FREE ( Boolean )
FREE ( UInteger8 )
FREE ( Octet )
FREE ( Enumeration8 )
FREE ( Integer8 )
FREE ( Enumeration16 )
FREE ( Integer16 )
FREE ( UInteger16 )
FREE ( Integer32 )
FREE ( UInteger32 )
FREE ( Integer64 )
FREE ( Enumeration4 )
FREE ( UInteger4 )
FREE ( Nibble )

#ifndef STRINGIFY
#define STRINGIFY(x) _STRINGIFY(x)
#define _STRINGIFY(x) #x
#endif

/* Error checking macro to be called by each unpacking function for each
   field to be unpacked.
   This is called by composite type unpackers while the length is
   ignored by the macros implementing simple type unpacking.
   The result is set to the number of bytes unpacked or UNPACK_ERROR.
 */
#define CHECK_INPUT_LENGTH(offset, size, length, name, result, failure_label) \
	assert(UNPACK_OK(result)); \
	if ((offset) + (size) > (length)) {				\
		ERROR("attempt to unpack incoming message field %s beyond received data (%d + %d > %d)\n", \
		      STRINGIFY(name), offset, size, length);		\
		result = UNPACK_ERROR; \
		goto failure_label;    \
	} else { \
		result = UNPACK_SIZE(UNPACK_GET_SIZE(result) + size);	\
	}

/* Error checking macro to be called by packing functions to check that
   a field will fit.
   The result is set to the number of bytes unpacked or UNPACK_ERROR.
*/
#define CHECK_OUTPUT_LENGTH(offset, size, length, name, result, failure_label) \
	assert(PACK_OK(result)); \
	if ((offset) + (size) > (length)) {				\
		ERROR("attempt to pack outgoing message field %s beyond output buffer (%d + %d > %d)\n", \
		      STRINGIFY(name), offset, size, length);		\
		result = PACK_ERROR; \
		goto failure_label;    \
	} else { \
		result = PACK_SIZE(PACK_GET_SIZE(result) + size); \
	}

/* Assumes standard usage of
 *   variables: buf, length, result, offset, ptpClock
 *   label: finish
 */
#define STANDARD_UNPACKING_OPERATION(name, size, type)			\
	CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
	unpack##type( buf + offset, length - offset, &data->name, ptpClock); \
	offset = offset + size;

/* Assumes standard usage of
 *   variables: buf, space, result, offset
 *   label: finish
 */
#define STANDARD_PACKING_OPERATION(name, size, type)			\
	CHECK_OUTPUT_LENGTH(offset, size, space, name, result, finish); \
	pack##type( &data->name, buf + offset, space - offset);		\
	offset = offset + size;

/* Macro to check boundaries for TLV */
#define TLV_BOUNDARY_CHECK(offset, space) \
	assert(space > 4); \
	assert(offset < space); \
	assert((offset & 1) == 0); \
	assert((space & 1) == 0);

/* Macro to pad TLV to even length if odd, as per 5.3.8, table 41 */
#define PAD_TO_EVEN_LENGTH(buf, offset, space, result, failure_label)	\
	assert(PACK_OK(result));					\
	if ((offset) % 2 != 0) {					\
		if ((offset) + 1 > space) {				\
			ERROR("no space to pad TLV to even length\n");	\
			result = PACK_ERROR;				\
		} else {						\
			Octet pad = 0;					\
			packOctet(&pad, buf + offset, space - offset);	\
			offset = offset + 1;				\
			result = PACK_SIZE(PACK_GET_SIZE(result) + 1);	\
		} \
	}


ssize_t
unpackUInteger48( void *buf, size_t length, void *i, PtpClock *ptpClock)
{
	UInteger16 msb;
	UInteger32 lsb;

	if (length >= 48 / 8) {
		unpackUInteger16(buf, length, &msb, ptpClock);
		unpackUInteger32((uint8_t *) buf + 2, length - 2, &lsb, ptpClock);
		*((UInteger48 *) i) = (((UInteger48) msb) << 32) | lsb;
		return UNPACK_SIZE(48 / 8);
	} else {
		return UNPACK_ERROR;
	}
}

ssize_t
packUInteger48( void *i, void *buf, size_t space)
{
	UInteger48 num = *((UInteger48 *) i);
	UInteger16 msb = num >> 32;
	UInteger32 lsb = num;

	if (space >= 48 / 8) {
		packUInteger16(&msb, buf, 2);
		packUInteger32(&lsb, (char *)buf + 2, 4);
		return PACK_SIZE(48 / 8);
	} else {
		return PACK_ERROR;
	}
}


ssize_t
unpackUInteger24( void *buf, size_t length, void *i, PtpClock *ptpClock)
{
	UInteger8 msb;
	UInteger16 lsb;

	if (length >= 24 / 8) {
		unpackUInteger8(buf, length, &msb, ptpClock);
		unpackUInteger16((char *)buf + 1, length - 1, &lsb, ptpClock);
		*((UInteger24 *) i) = (((UInteger24) msb) << 16) | lsb;
		return UNPACK_SIZE(24 / 8);
	} else {
		return UNPACK_ERROR;
	}
}


ssize_t
packUInteger24( void *i, void *buf, size_t space)
{
	UInteger24 num = *((UInteger24 *) i);
	UInteger8 msb = num >> 16;
	UInteger16 lsb = num;

	if (space >= 24 / 8) {
		packUInteger8(&msb, buf, 1);
		packUInteger16(&lsb, (char *)buf + 1, 2);
		return PACK_SIZE(24 / 8);
	} else {
		return PACK_ERROR;
	}
}


/* NOTE: the unpack functions for management messages can probably be refactored into a macro */
ssize_t
unpackMMSlaveOnly( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
	XMALLOC(m->tlv->dataField, sizeof(MMSlaveOnly));
	MMSlaveOnly* data = (MMSlaveOnly*)m->tlv->dataField;
	/* see src/def/README for a note on this X-macro */
	#define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
		offset = offset + size;
	#include "../def/managementTLV/slaveOnly.def"
 finish:
	mMSlaveOnly_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

/* NOTE: the pack functions for management messsages can probably be refactored into a macro */
ssize_t
packMMSlaveOnly( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
	int offset = base;
	MMSlaveOnly* data = (MMSlaveOnly*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/managementTLV/slaveOnly.def"
 finish:
	/* return length */
	return offset - base;
}

ssize_t
unpackMMClockDescription( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
	XMALLOC(m->tlv->dataField, sizeof(MMClockDescription));
	MMClockDescription* data = (MMClockDescription*)m->tlv->dataField;
	memset(data, 0, sizeof(MMClockDescription));
	#define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
		offset = offset + size;
	#include "../def/managementTLV/clockDescription.def"
 finish:
	mMClockDescription_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMClockDescription( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
	int offset = base;
	MMClockDescription* data = (MMClockDescription*)m->tlv->dataField;
	data->reserved = 0;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/managementTLV/clockDescription.def"
	PAD_TO_EVEN_LENGTH(buf, offset, space, result, finish);
 finish:
	/* return length */
	return offset - base;
}

void
freeMMClockDescription( MMClockDescription* data)
{
	#define OPERATE( name, size, type ) \
		free##type( &data->name);
	#include "../def/managementTLV/clockDescription.def"
}

ssize_t
unpackMMUserDescription( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
	XMALLOC(m->tlv->dataField, sizeof(MMUserDescription));
	MMUserDescription* data = (MMUserDescription*)m->tlv->dataField;
	memset(data, 0, sizeof(MMUserDescription));
	#define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
		offset = offset + size;
	#include "../def/managementTLV/userDescription.def"
 finish:
	/* mMUserDescription_display(data, ptpClock); */
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMUserDescription( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
	int offset = base;
	MMUserDescription* data = (MMUserDescription*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/managementTLV/userDescription.def"
	PAD_TO_EVEN_LENGTH(buf, offset, space, result, finish);
 finish:
	/* return length */
	return offset - base;
}

void
freeMMUserDescription( MMUserDescription* data)
{
	#define OPERATE( name, size, type ) \
		free##type( &data->name);
	#include "../def/managementTLV/userDescription.def"
}

ssize_t unpackMMInitialize( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMInitialize));
        MMInitialize* data = (MMInitialize*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/initialize.def"
 finish:
        mMInitialize_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMInitialize( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
	int offset = base;
        MMInitialize* data = (MMInitialize*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/initialize.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMDefaultDataSet( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMDefaultDataSet));
        MMDefaultDataSet* data = (MMDefaultDataSet*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/defaultDataSet.def"
 finish:
        mMDefaultDataSet_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMDefaultDataSet( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
	int offset = base;
        MMDefaultDataSet* data = (MMDefaultDataSet*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/defaultDataSet.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMCurrentDataSet( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMCurrentDataSet));
        MMCurrentDataSet* data = (MMCurrentDataSet*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/currentDataSet.def"
 finish:
        mMCurrentDataSet_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMCurrentDataSet( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
	int offset = base;
        MMCurrentDataSet* data = (MMCurrentDataSet*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/currentDataSet.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMParentDataSet( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
       XMALLOC(m->tlv->dataField, sizeof(MMParentDataSet));
        MMParentDataSet* data = (MMParentDataSet*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/parentDataSet.def"
 finish:
        mMParentDataSet_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packIncParentDataSet( IncParentDataSet *data, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int offset = 0;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/parentDataSet.def"
 finish:
        /* return length*/
        return offset;
}

ssize_t
packIncCurrentDataSet( IncCurrentDataSet *data, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
        int offset = 0;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/currentDataSet.def"
 finish:
        /* return length*/
        return offset;
}

ssize_t
packIncTimePropertiesDataSet( IncTimePropertiesDataSet *data, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
        int offset = 0;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/timePropertiesDataSet.def"
 finish:
        /* return length*/
        return offset;
}

ssize_t
packMMParentDataSet( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
	int offset = base;
        MMParentDataSet* data = (MMParentDataSet*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/parentDataSet.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMTimePropertiesDataSet( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMTimePropertiesDataSet));
        MMTimePropertiesDataSet* data = (MMTimePropertiesDataSet*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/timePropertiesDataSet.def"
 finish:
        mMTimePropertiesDataSet_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMTimePropertiesDataSet( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMTimePropertiesDataSet* data = (MMTimePropertiesDataSet*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/timePropertiesDataSet.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMPortDataSet( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMPortDataSet));
        MMPortDataSet* data = (MMPortDataSet*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/portDataSet.def"
 finish:
        mMPortDataSet_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMPortDataSet( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMPortDataSet* data = (MMPortDataSet*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/portDataSet.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMPriority1( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMPriority1));
        MMPriority1* data = (MMPriority1*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/priority1.def"
 finish:
        mMPriority1_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMPriority1( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMPriority1* data = (MMPriority1*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/priority1.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMPriority2( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMPriority2));
        MMPriority2* data = (MMPriority2*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/priority2.def"
 finish:
        mMPriority2_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMPriority2( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
        int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
	int offset = base;
        MMPriority2* data = (MMPriority2*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/priority2.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMDomain( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMDomain));
        MMDomain* data = (MMDomain*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/domain.def"
 finish:
        mMDomain_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMDomain( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMDomain* data = (MMDomain*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/domain.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMLogAnnounceInterval( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMLogAnnounceInterval));
        MMLogAnnounceInterval* data = (MMLogAnnounceInterval*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/logAnnounceInterval.def"
 finish:
        mMLogAnnounceInterval_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMLogAnnounceInterval( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMLogAnnounceInterval* data = (MMLogAnnounceInterval*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/logAnnounceInterval.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMAnnounceReceiptTimeout( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField,sizeof(MMAnnounceReceiptTimeout));
        MMAnnounceReceiptTimeout* data = (MMAnnounceReceiptTimeout*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/announceReceiptTimeout.def"
 finish:
        mMAnnounceReceiptTimeout_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMAnnounceReceiptTimeout( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMAnnounceReceiptTimeout* data = (MMAnnounceReceiptTimeout*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/announceReceiptTimeout.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMLogSyncInterval( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMLogSyncInterval));
        MMLogSyncInterval* data = (MMLogSyncInterval*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/logSyncInterval.def"
 finish:
        mMLogSyncInterval_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMLogSyncInterval( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMLogSyncInterval* data = (MMLogSyncInterval*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/logSyncInterval.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMVersionNumber( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMVersionNumber));
        MMVersionNumber* data = (MMVersionNumber*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/versionNumber.def"
 finish:
        mMVersionNumber_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMVersionNumber( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMVersionNumber* data = (MMVersionNumber*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/versionNumber.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMTime( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMTime));
        MMTime* data = (MMTime*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/time.def"
 finish:
        mMTime_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMTime( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMTime* data = (MMTime*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/time.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMClockAccuracy( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMClockAccuracy));
        MMClockAccuracy* data = (MMClockAccuracy*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/clockAccuracy.def"
 finish:
        mMClockAccuracy_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMClockAccuracy( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMClockAccuracy* data = (MMClockAccuracy*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/clockAccuracy.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMUtcProperties( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMUtcProperties));
        MMUtcProperties* data = (MMUtcProperties*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/utcProperties.def"
 finish:
        mMUtcProperties_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMUtcProperties( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMUtcProperties* data = (MMUtcProperties*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/utcProperties.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMTraceabilityProperties( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMTraceabilityProperties));
        MMTraceabilityProperties* data = (MMTraceabilityProperties*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/traceabilityProperties.def"
 finish:
        mMTraceabilityProperties_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMTraceabilityProperties( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMTraceabilityProperties* data = (MMTraceabilityProperties*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/traceabilityProperties.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMDelayMechanism( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMDelayMechanism));
        MMDelayMechanism* data = (MMDelayMechanism*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/delayMechanism.def"
 finish:
        mMDelayMechanism_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMDelayMechanism( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMDelayMechanism* data = (MMDelayMechanism*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/delayMechanism.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMLogMinPdelayReqInterval( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMLogMinPdelayReqInterval));
        MMLogMinPdelayReqInterval* data = (MMLogMinPdelayReqInterval*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/logMinPdelayReqInterval.def"
 finish:
        mMLogMinPdelayReqInterval_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

ssize_t
packMMLogMinPdelayReqInterval( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMLogMinPdelayReqInterval* data = (MMLogMinPdelayReqInterval*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/logMinPdelayReqInterval.def"
 finish:
        /* return length*/
        return offset - base;
}

ssize_t unpackMMErrorStatus( Octet *buf, size_t length, MsgManagement* m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        XMALLOC(m->tlv->dataField, sizeof(MMErrorStatus));
        MMErrorStatus* data = (MMErrorStatus*)m->tlv->dataField;
        #define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type( buf + offset, length - offset, &data->name, ptpClock ); \
                offset = offset + size;
        #include "../def/managementTLV/errorStatus.def"
 finish:
        mMErrorStatus_display(data, ptpClock);
	if (!UNPACK_OK(result)) {
		free(m->tlv->dataField);
	}
	return result;
}

void
freeMMErrorStatus( MMErrorStatus* data)
{
	#define OPERATE( name, size, type ) \
		free##type( &data->name);
	#include "../def/managementTLV/errorStatus.def"
}

ssize_t
packMMErrorStatus( MsgManagement* m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int base = PTPD_MANAGEMENT_LENGTH + PTPD_TLV_LENGTH;
        int offset = base;
        MMErrorStatus* data = (MMErrorStatus*)m->tlv->dataField;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/managementTLV/errorStatus.def"
	PAD_TO_EVEN_LENGTH(buf, offset, space, result, finish);
 finish:
	/* return length*/
	return offset - base;
}



ssize_t
unpackClockIdentity( Octet *buf, size_t length, ClockIdentity *c, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int i;

	CHECK_INPUT_LENGTH(0, CLOCK_IDENTITY_LENGTH, length, "clock identity", result, finish);
	for(i = 0; i < CLOCK_IDENTITY_LENGTH; i++) {
		unpackOctet((buf+i), length - i, &((*c)[i]), ptpClock);
	}
 finish:
	return result;
}

ssize_t packClockIdentity( ClockIdentity *c, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int i;

	CHECK_OUTPUT_LENGTH(0, CLOCK_IDENTITY_LENGTH, space, "clock identity", result, finish);
	for(i = 0; i < CLOCK_IDENTITY_LENGTH; i++) {
		packOctet(&((*c)[i]),(buf+i), space - i);
	}
 finish:
	return result;
}

void
freeClockIdentity( ClockIdentity *c) {
	/* nothing to free */
}

ssize_t
unpackClockQuality( Octet *buf, size_t length, ClockQuality *c, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;
	ClockQuality* data = c;
	#define OPERATE( name, size, type) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type (buf + offset, length - offset, &data->name, ptpClock); \
		offset = offset + size;
	#include "../def/derivedData/clockQuality.def"
 finish:
	return result;
}

ssize_t
packClockQuality( ClockQuality *c, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int offset = 0;
	ClockQuality *data = c;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/derivedData/clockQuality.def"
 finish:
	return result;
}

void
freeClockQuality( ClockQuality *c)
{
	/* nothing to free */
}

ssize_t
unpackTimestamp( Octet *buf, size_t length, Timestamp *t, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = 0;
        Timestamp* data = t;
        #define OPERATE( name, size, type) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
                unpack##type (buf + offset, length - offset, &data->name, ptpClock); \
                offset = offset + size;
        #include "../def/derivedData/timestamp.def"
 finish:
	return result;
}

ssize_t
packTimestamp( Timestamp *t, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
        int offset = 0;
        Timestamp *data = t;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
        #include "../def/derivedData/timestamp.def"
 finish:
	return result;
}

void
freeTimestamp( Timestamp *t)
{
        /* nothing to free */
}


ssize_t
unpackPortIdentity( Octet *buf, size_t length, PortIdentity *p, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;
	PortIdentity* data = p;
	#define OPERATE( name, size, type) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type (buf + offset, length - offset, &data->name, ptpClock); \
		offset = offset + size;
	#include "../def/derivedData/portIdentity.def"
 finish:
	return result;
}

ssize_t
packPortIdentity( PortIdentity *p, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int offset = 0;
	PortIdentity *data = p;
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/derivedData/portIdentity.def"
 finish:
	return result;
}

void
freePortIdentity( PortIdentity *p)
{
	/* nothing to free */
}

ssize_t
unpackPortAddress( Octet *buf, size_t length, PortAddress *p, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	CHECK_INPUT_LENGTH(0, 2, length, "port network protocol", result, finish);
	unpackEnumeration16( buf, length, &p->networkProtocol, ptpClock);

	CHECK_INPUT_LENGTH(2, 2, length, "port address length", result, finish);
	unpackUInteger16( buf+2, length - 2, &p->addressLength, ptpClock);

	if (p->addressLength != 0) {
		CHECK_INPUT_LENGTH(4, p->addressLength, length - 4, "port address", result, finish);
		XMALLOC(p->addressField, p->addressLength);
		memcpy( p->addressField, buf+4, p->addressLength);
	} else {
		p->addressField = NULL;
	}

 finish:
	return result;
}

ssize_t
packPortAddress(PortAddress *p, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;

	CHECK_OUTPUT_LENGTH(0, 2, space, "port network protocol", result, finish);
	packEnumeration16(&p->networkProtocol, buf, space);

	CHECK_OUTPUT_LENGTH(2, 2, space, "port address length", result, finish);
	packUInteger16(&p->addressLength, buf+2, space - 2);

	CHECK_OUTPUT_LENGTH(4, p->addressLength, space - 4, "port address", result, finish);
	memcpy( buf+4, p->addressField, p->addressLength);
 finish:
	return result;
}

void
freePortAddress(PortAddress *p)
{
	if(p->addressField) {
		free(p->addressField);
		p->addressField = NULL;
	}
}

ssize_t
unpackPTPText( Octet *buf, size_t length, PTPText *s, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	CHECK_INPUT_LENGTH(0, 1, length, "PTP text length", result, finish);
	unpackUInteger8( buf, length, &s->lengthField, ptpClock);

	if (s->lengthField != 0) {
		CHECK_INPUT_LENGTH(1, s->lengthField, length, "PTP text", result, finish);
		XMALLOC(s->textField, s->lengthField);
		memcpy( s->textField, buf+1, s->lengthField);
	} else {
		s->textField = NULL;
	}

 finish:
	return result;
}

ssize_t
packPTPText(PTPText *s, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	CHECK_OUTPUT_LENGTH(0, 1, space, "PTP text length", result, finish);
	packUInteger8(&s->lengthField, buf, space);

	CHECK_OUTPUT_LENGTH(1, s->lengthField, space - 1, "PTP text", result, finish);
	memcpy( buf+1, s->textField, s->lengthField);
 finish:
	return result;
}

void
freePTPText(PTPText *s)
{
	if(s->textField) {
		free(s->textField);
		s->textField = NULL;
	}
}

ssize_t
unpackPhysicalAddress( Octet *buf, size_t length, PhysicalAddress *p, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	CHECK_INPUT_LENGTH(0, 2, length, "physical address length", result, finish);
	unpackUInteger16( buf, length, &p->addressLength, ptpClock);

	if(p->addressLength) {
		CHECK_INPUT_LENGTH(2, p->addressLength, length, "physical address", result, finish);
		XMALLOC(p->addressField, p->addressLength);
		memcpy( p->addressField, buf+2, p->addressLength);
	} else {
		p->addressField = NULL;
	}
 finish:
	return result;
}

ssize_t
packPhysicalAddress(PhysicalAddress *p, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;

	CHECK_OUTPUT_LENGTH(0, 2, space, "physical address length", result, finish);
	packUInteger16(&p->addressLength, buf, space);

	CHECK_OUTPUT_LENGTH(2, p->addressLength, space - 2, "physical address", result, finish);
	memcpy( buf+2, p->addressField, p->addressLength);
 finish:
	return result;
}

void
freePhysicalAddress(PhysicalAddress *p)
{
	if(p->addressField) {
		free(p->addressField);
		p->addressField = NULL;
	}
}

void
copyClockIdentity( ClockIdentity dest, ClockIdentity src)
{
	memcpy(dest, src, CLOCK_IDENTITY_LENGTH);
}

void
copyPortIdentity( PortIdentity *dest, PortIdentity *src)
{
	copyClockIdentity(dest->clockIdentity, src->clockIdentity);
	dest->portNumber = src->portNumber;
}

ssize_t
unpackMsgHeader(Octet *buf, size_t length, MsgHeader *header, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;
	MsgHeader* data = header;
	#define OPERATE( name, size, type) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type (buf + offset, length - offset, &data->name, ptpClock); \
		offset = offset + size;
	#include "../def/message/header.def"
 finish:
	return result;
}

UInteger16 getHeaderLength(Octet *buf) {
	return flip16(*(UInteger16 *) (buf + 2));
}

static void setHeaderLength(Octet *buf, UInteger16 length) {
	*((UInteger16 *) (buf + 2)) = flip16(length);
}

ssize_t
packMsgHeader(MsgHeader *h, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int offset = 0;
	MsgHeader *data = h;

	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/message/header.def"
 finish:
	return result;
}

ssize_t
unpackManagementTLV(Octet *buf, size_t length, MsgManagement *m, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = PTPD_MANAGEMENT_LENGTH;
	XMALLOC(m->tlv, sizeof(ManagementTLV));
	/* read the management TLV */
	#define OPERATE( name, size, type ) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type( buf + offset, length - offset, &m->tlv->name, ptpClock ); \
		offset = offset + size;
	#include "../def/managementTLV/managementTLV.def"
 finish:
	if (!UNPACK_OK(result)) {
		free(m->tlv);
	}
	return result;
}

ssize_t
packManagementTLV(ManagementTLV *tlv, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int offset = PTPD_MANAGEMENT_LENGTH;
	ManagementTLV *data = tlv;

	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/managementTLV/managementTLV.def"
 finish:
	return result;
}

void
freeManagementTLV(MsgManagement *m)
{
        /* cleanup outgoing managementTLV */
        if(m->tlv) {
                if(m->tlv->dataField) {
                        if(m->tlv->tlvType == PTPD_TLV_MANAGEMENT) {
                                freeMMTLV(m->tlv);
                        } else if(m->tlv->tlvType == PTPD_TLV_MANAGEMENT_ERROR_STATUS) {
                                freeMMErrorStatusTLV(m->tlv);
                        }
                        free(m->tlv->dataField);
			m->tlv->dataField = NULL;
                }
                free(m->tlv);
		m->tlv = NULL;
        }
}

ssize_t
packMsgManagement(MsgManagement *m, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int offset = 0;
	MsgManagement *data = m;

	/* set unitialized bytes to zero */
	m->reserved0 = 0;
	m->reserved1 = 0;

	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/message/management.def"
 finish:
	return result;
}

ssize_t unpackMsgManagement(Octet *buf, size_t length, MsgManagement *m, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;
	MsgManagement* data = m;
	#define OPERATE( name, size, type) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type (buf + offset, length - offset, &data->name, ptpClock); \
		offset = offset + size;
	#include "../def/message/management.def"
 finish:
	msgManagement_display(data);
	return result;
}

/*Unpack Header from IN buffer to msgTmpHeader field */
ssize_t
msgUnpackHeader(Octet *buf, size_t length, MsgHeader * header)
{
	ssize_t result = UNPACK_INIT;
	CHECK_INPUT_LENGTH(0, 34, length, "header", result, finish);

	header->majorSdoId = (*(Nibble *) (buf + 0)) >> 4;
	header->messageType = (*(Enumeration4 *) (buf + 0)) & 0x0F;
	header->minorVersionPTP = (*(UInteger4 *) (buf + 1)) >> 4;
	header->versionPTP = (*(UInteger4 *) (buf + 1)) & 0x0F;
	header->messageLength = flip16(*(UInteger16 *) (buf + 2));
	header->domainNumber = (*(UInteger8 *) (buf + 4));
	header->minorSdoId = (*(UInteger8 *) (buf + 5));
	header->flagField0 = (*(Octet *) (buf + 6));
	header->flagField1 = (*(Octet *) (buf + 7));
	memcpy(&header->correctionField, (buf + 8), 8);
	header->correctionField = flip64(header->correctionField);
	header->messageTypeSpecific = (*(UInteger32 *) (buf + 16));
	copyClockIdentity(header->sourcePortIdentity.clockIdentity, (buf + 20));
	header->sourcePortIdentity.portNumber =
		flip16(*(UInteger16 *) (buf + 28));
	header->sequenceId = flip16(*(UInteger16 *) (buf + 30));
	header->controlField = (*(UInteger8 *) (buf + 32));
	header->logMessageInterval = (*(Integer8 *) (buf + 33));
 finish:
	msgHeader_display(header);
	return result;
}

/*Pack header message into OUT buffer of ptpClock*/
ssize_t
msgPackHeader(Octet *buf, size_t space, PtpClock * ptpClock, unsigned int messageType)
{
	ssize_t result = PACK_INIT;
	const UInteger4 majorSdoId = 0x0;
	const UInteger8 minorSdoId = 0x00;
	UInteger8 octet0 = (majorSdoId << 4) | (UInteger8)messageType;

	CHECK_OUTPUT_LENGTH(0, 34, space, "header", result, finish);

	/* (spec annex D) */
	*(UInteger8 *) (buf + 0) = octet0;
	*(UInteger8 *) (buf + 1) = (ptpClock->rtOpts.ptp_version_minor << 4) | PTPD_PROTOCOL_VERSION;
	*(UInteger8 *) (buf + 2) = 0; /* messageLength */
	*(UInteger8 *) (buf + 3) = 0; /* messageLength */
	*(UInteger8 *) (buf + 4) = ptpClock->domainNumber;
	*(UInteger8 *) (buf + 5) = minorSdoId;

	if (((messageType == PTPD_MSG_SYNC) || (messageType == PTPD_MSG_PDELAY_RESP)) &&
	    (ptpClock->twoStepFlag)) {
		*(UInteger8 *) (buf + 6) = PTPD_FLAG_TWO_STEP;
	} else {
		*(UInteger8 *) (buf + 6) = 0;
	}
	*(UInteger8 *) (buf + 7) = 0;

	memset((buf + 8), 0, 12); /* correctionField; messageTypeSpecific */
	copyClockIdentity((buf + 20), ptpClock->portIdentity.clockIdentity);
	*(UInteger16 *) (buf + 28) = flip16(ptpClock->portIdentity.portNumber);
	*(UInteger8 *) (buf + 30) = 0; /* sequenceId */
	*(UInteger8 *) (buf + 31) = 0; /* sequenceId */
	*(UInteger8 *) (buf + 32) = 0; /* controlField */
	*(UInteger8 *) (buf + 33) = 0x7F;
	/* Default value(spec Table 24) */
 finish:
	return result;
}

/* Update the flags in a header */
void
msgUpdateHeaderSequenceId(Octet *buf, UInteger16 sequenceId)
{
	*((UInteger16 *) (buf + 30)) = flip16(sequenceId);
}


/* Update the flags in a header */
void
msgUpdateHeaderFlags(Octet *buf, UInteger8 mask, UInteger8 value)
{
	UInteger8 *ptr = (UInteger8 *) (buf + 6);

	*ptr = (*ptr & mask) | value;
}


/*Pack SYNC message into OUT buffer of ptpClock*/
ssize_t
msgPackSync(Octet *buf, size_t space, PtpClock *ptpClock)
{
	ssize_t result = msgPackHeader(buf, space, ptpClock, PTPD_MSG_SYNC);
	
	CHECK_OUTPUT_LENGTH(34, 10, space, "sync", result, finish);

	/* changes in header */
	*(UInteger16 *) (buf + 2) = flip16(PTPD_SYNC_LENGTH);
	*(UInteger16 *) (buf + 30) = flip16(ptpClock->sentSyncSequenceId);
	*(UInteger8 *) (buf + 32) = PTPD_CONTROL_FIELD_SYNC;
	/* Table 23 */
	*(Integer8 *) (buf + 33) = ptpClock->logSyncInterval;
	memset((buf + 8), 0, 8);

	/* Sync message. Note that we use zero for the timestamp as the
	 * real transmit time is determined later */
	*(UInteger16 *) (buf + 34) = 0;
	*(UInteger32 *) (buf + 36) = 0;
	*(UInteger32 *) (buf + 40) = 0;
 finish:
	return result;
}

/*Unpack Sync message from IN buffer */
ssize_t
msgUnpackSync(Octet *buf, size_t length, MsgSync * sync)
{
	ssize_t result = UNPACK_INIT;
	CHECK_INPUT_LENGTH(34, 10, length, "sync", result, finish);
	unpackUInteger48(buf + 34, length - 34, &sync->originTimestamp.secondsField, NULL);
	unpackUInteger32(buf + 40, length - 40, &sync->originTimestamp.nanosecondsField, NULL);
 finish:
	msgSync_display(sync);
	return result;
}



/*Pack Announce message into OUT buffer of ptpClock*/
ssize_t
msgPackAnnounce(Octet *buf, size_t space, PtpClock * ptpClock)
{
	ssize_t result = msgPackHeader(buf, space, ptpClock, PTPD_MSG_ANNOUNCE);
	CHECK_OUTPUT_LENGTH(34, 30, space, "announce", result, finish);

	/* changes in header */
	/* Table 19 */
	*(UInteger16 *) (buf + 2) = flip16(PTPD_ANNOUNCE_LENGTH);
	*(UInteger16 *) (buf + 30) = flip16(ptpClock->sentAnnounceSequenceId);
	*(UInteger8 *) (buf + 32) = PTPD_CONTROL_FIELD_ALL_OTHERS;
	/* Table 23 */
	*(Integer8 *) (buf + 33) = ptpClock->logAnnounceInterval;

	/* Announce message */
	memset((buf + 34), 0, 10);
	*(Integer16 *) (buf + 44) = flip16(ptpClock->timePropertiesDS.currentUtcOffset);
	*(UInteger8 *) (buf + 47) = ptpClock->grandmasterPriority1;
	*(UInteger8 *) (buf + 48) = ptpClock->clockQuality.clockClass;
	*(Enumeration8 *) (buf + 49) = ptpClock->clockQuality.clockAccuracy;
	*(UInteger16 *) (buf + 50) = 
		flip16(ptpClock->clockQuality.offsetScaledLogVariance);
	*(UInteger8 *) (buf + 52) = ptpClock->grandmasterPriority2;
	copyClockIdentity((buf + 53), ptpClock->grandmasterIdentity);
	*(UInteger16 *) (buf + 61) = flip16(ptpClock->stepsRemoved);
	*(Enumeration8 *) (buf + 63) = ptpClock->timePropertiesDS.timeSource;

	/*
	 * TimePropertiesDS in FlagField, 2nd octet - spec 13.3.2.6 table 20
	 * Could / should have used constants here PTP_LI_61 etc, but this is clean
	 */
	if (ptpClock->timePropertiesDS.leap59)
		*(UInteger8 *) (buf + 7) |= SET_FIELD(1, PTPD_LI59);
	if (ptpClock->timePropertiesDS.leap61)
		*(UInteger8 *) (buf + 7) |= SET_FIELD(1, PTPD_LI61);
	if (ptpClock->timePropertiesDS.currentUtcOffsetValid)
		*(UInteger8 *) (buf + 7) |= SET_FIELD(1, PTPD_UTCV);
	if (ptpClock->timePropertiesDS.ptpTimescale)
		*(UInteger8 *) (buf + 7) |= SET_FIELD(1, PTPD_PTPT);
	if (ptpClock->timePropertiesDS.timeTraceable)
		*(UInteger8 *) (buf + 7) |= SET_FIELD(1, PTPD_TTRA);
	if (ptpClock->timePropertiesDS.frequencyTraceable)
		*(UInteger8 *) (buf + 7) |= SET_FIELD(1, PTPD_FTRA);
 finish:
	return result;
}

/*Unpack Announce message from IN buffer of ptpClock to msgtmp.Announce*/
ssize_t
msgUnpackAnnounce(Octet *buf, size_t length, MsgAnnounce * announce)
{
	ssize_t result = UNPACK_INIT;
	CHECK_INPUT_LENGTH(34, 30, length, "announce", result, finish);

	unpackUInteger48(buf + 34, length - 34, &announce->originTimestamp.secondsField, NULL);
	unpackUInteger32(buf + 40, length - 40, &announce->originTimestamp.nanosecondsField, NULL);
	announce->currentUtcOffset = flip16(*(UInteger16 *) (buf + 44));
	announce->grandmasterPriority1 = *(UInteger8 *) (buf + 47);
	announce->grandmasterClockQuality.clockClass = 
		*(UInteger8 *) (buf + 48);
	announce->grandmasterClockQuality.clockAccuracy = 
		*(Enumeration8 *) (buf + 49);
	announce->grandmasterClockQuality.offsetScaledLogVariance = 
		flip16(*(UInteger16 *) (buf + 50));
	announce->grandmasterPriority2 = *(UInteger8 *) (buf + 52);
	copyClockIdentity(announce->grandmasterIdentity, (buf + 53));
	announce->stepsRemoved = flip16(*(UInteger16 *) (buf + 61));
	announce->timeSource = *(Enumeration8 *) (buf + 63);
 finish:
	msgAnnounce_display(announce);
	return result;
}

/* Set the in-payload timestamp for the following message types and the
 * in-header correctionField to allow for sub-nanosecond precision:
 *   Follow_Up
 *   Delay_Resp
 *   PDelay_Resp_Follow_Up
 * These are the precise timestamps used for two-step clocks.
 * Receipt timetamps being returned to the origin have the fractional
 * part subtracted from the correctionField rather than added. */
static int msgSetPreciseTimestamp(Octet *buf,
				  size_t space,
				  const struct sfptpd_timespec *preciseTimestamp,
				  bool subtract_correction,
				  TimeInterval extra_correction)
{
	Timestamp timestamp;
	TimeInterval correction;
	int rc = 0;

	if (space < 44)
		return ENOSPC;

	rc = fromInternalTime(preciseTimestamp, &timestamp, &correction);
	if (subtract_correction)
		correction = -correction;
	correction += extra_correction;
	packUInteger48(&timestamp.secondsField, buf + 34, space - 34);
	packUInteger32(&timestamp.nanosecondsField, buf + 40, space - 40);
	packTimeInterval(&correction, buf + 8, space - 8);

	return rc;
}

/*pack Follow_up message into OUT buffer of ptpClock*/
ssize_t
msgPackFollowUp(Octet *buf, size_t space,
		const struct sfptpd_timespec *preciseOriginTimestamp,
		PtpClock * ptpClock, const UInteger16 sequenceId)
{
	ssize_t result = msgPackHeader(buf, space, ptpClock, PTPD_MSG_FOLLOW_UP);
	
	CHECK_OUTPUT_LENGTH(34, 10, space, "follow-up", result, finish);

	/* changes in header */
	/* Table 19 */
	*(UInteger16 *) (buf + 2) = flip16(PTPD_FOLLOW_UP_LENGTH);
	*(UInteger16 *) (buf + 30) = flip16(sequenceId);
	*(UInteger8 *) (buf + 32) = PTPD_CONTROL_FIELD_FOLLOW_UP;
	/* Table 23 */
	*(Integer8 *) (buf + 33) = ptpClock->logSyncInterval;

	/* Follow_up message includes the subnanosecond component of
	 * of our own high precision timestamps in the correctionField. */
	if (msgSetPreciseTimestamp(buf, space, preciseOriginTimestamp, false, 0) != 0)
		result = PACK_ERROR;
 finish:
	return result;
}

/*Unpack Follow_up message from IN buffer of ptpClock to msgtmp.follow*/
ssize_t
msgUnpackFollowUp(Octet *buf, size_t length, MsgFollowUp * follow)
{
	ssize_t result = UNPACK_INIT;
	CHECK_INPUT_LENGTH(34, 10, length, "follow-up", result, finish);
	unpackUInteger48(buf + 34, length - 34, &follow->preciseOriginTimestamp.secondsField, NULL);
	unpackUInteger32(buf + 40, length - 40, &follow->preciseOriginTimestamp.nanosecondsField, NULL);
 finish:
	msgFollowUp_display(follow);
	return result;
}


/*pack PdelayReq message into OUT buffer of ptpClock*/
ssize_t
msgPackPDelayReq(Octet *buf, size_t space, PtpClock *ptpClock)
{
	ssize_t result = msgPackHeader(buf, space, ptpClock, PTPD_MSG_PDELAY_REQ);

	CHECK_OUTPUT_LENGTH(34, 10, space, "P delay req", result, finish);

	/* changes in header */
	/* Table 19 */
	*(UInteger16 *) (buf + 2) = flip16(PTPD_PDELAY_REQ_LENGTH);
	*(UInteger16 *) (buf + 30) = flip16(ptpClock->sentPDelayReqSequenceId);
	*(UInteger8 *) (buf + 32) = PTPD_CONTROL_FIELD_ALL_OTHERS;
	/* Table 23 */
	*(Integer8 *) (buf + 33) = 0x7F;
	/* Table 24 */
	memset((buf + 8), 0, 8);

	/* PDelayReq message. Note that we use zero for the timestamp as the
	 * real transmit time is determined later */
	*(UInteger16 *) (buf + 34) = 0;
	*(UInteger32 *) (buf + 36) = 0;
	*(UInteger32 *) (buf + 40) = 0;

	memset((buf + 44), 0, 10);
	/* RAZ reserved octets */
 finish:
	return result;
}

/*pack delayReq message into OUT buffer of ptpClock*/
ssize_t
msgPackDelayReq(Octet *buf, size_t space, PtpClock *ptpClock)
{
	ssize_t result = msgPackHeader(buf, space, ptpClock, PTPD_MSG_DELAY_REQ);
	CHECK_OUTPUT_LENGTH(34, 10, space, "delay req", result, finish);
	
	/* changes in header */
	/* Table 19 */
	*(UInteger16 *) (buf + 2) = flip16(PTPD_DELAY_REQ_LENGTH);
	*(UInteger16 *) (buf + 30) = flip16(ptpClock->sentDelayReqSequenceId);
	*(UInteger8 *) (buf + 32) = PTPD_CONTROL_FIELD_DELAY_REQ;
	/* Table 23 */
	*(Integer8 *) (buf + 33) = 0x7F;
	/* Table 24 */
	memset((buf + 8), 0, 8);

	/* DelayReq message. Note that we use zero for the timestamp as the
	 * real transmit time is determined later */
	*(UInteger16 *) (buf + 34) = 0;
	*(UInteger32 *) (buf + 36) = 0;
	*(UInteger32 *) (buf + 40) = 0;
 finish:
	return result;
}

/*pack delayResp message into OUT buffer of ptpClock*/
ssize_t
msgPackDelayResp(Octet *buf, size_t space, MsgHeader * header,
		 const struct sfptpd_timespec * receiveTimestamp,
		 PtpClock * ptpClock)
{
	ssize_t result = msgPackHeader(buf, space, ptpClock, PTPD_MSG_DELAY_RESP);
	CHECK_OUTPUT_LENGTH(34, 20, space, "delay resp", result, finish);
	
	/* changes in header */
	/* Table 19 */
	*(UInteger16 *) (buf + 2) = flip16(PTPD_DELAY_RESP_LENGTH);
	*(UInteger8 *) (buf + 4) = header->domainNumber;

	memset((buf + 8), 0, 8);

	*(UInteger16 *) (buf + 30) = flip16(header->sequenceId);

	*(UInteger8 *) (buf + 32) = 0x03;
	/* Table 23 */
	*(Integer8 *) (buf + 33) = ptpClock->logMinDelayReqInterval;
	/* Table 24 */

	/* Delay_Resp message includes the correctionField
         * value from the received Delay_Req message MINUS the
	 * subnanosecond component we wish to add from our own
         * high precision timetamps. */
	if (msgSetPreciseTimestamp(buf, space,
				   receiveTimestamp,
				   true,
				   header->correctionField) != 0)
		result = PACK_ERROR;

	copyClockIdentity((buf + 44), header->sourcePortIdentity.clockIdentity);
	*(UInteger16 *) (buf + 52) =
		flip16(header->sourcePortIdentity.portNumber);
 finish:
	return result;
}





/*pack PdelayResp message into OUT buffer of ptpClock*/
ssize_t
msgPackPDelayResp(Octet *buf, size_t space, MsgHeader * header,
		  const struct sfptpd_timespec * timestamp,
		  PtpClock * ptpClock)
{
	ssize_t result = msgPackHeader(buf, space, ptpClock, PTPD_MSG_PDELAY_RESP);
	struct sfptpd_timespec requestReceiptTimestamp = *timestamp;
	CHECK_OUTPUT_LENGTH(34, 20, space, "P delay resp", result, finish);
	
	/* changes in header */
	/* Table 19 */
	*(UInteger16 *) (buf + 2) = flip16(PTPD_PDELAY_RESP_LENGTH);
	*(UInteger8 *) (buf + 4) = header->domainNumber;
	memset((buf + 8), 0, 8);

	*(UInteger16 *) (buf + 30) = flip16(header->sequenceId);

	*(UInteger8 *) (buf + 32) = PTPD_CONTROL_FIELD_ALL_OTHERS;
	/* Table 23 */
	*(Integer8 *) (buf + 33) = 0x7F;
	/* Table 24 */

	/* PDelay_Resp_follow_up message includes the fractional ns t2 receipt,
	 * deducted from the otherwise 0 correctionField.
	 * (1588-2019 11.4.2.c.7.Option B.i) */
	if (msgSetPreciseTimestamp(buf, space,
				   &requestReceiptTimestamp,
				   true, 0) != 0)
		result = PACK_ERROR;

	copyClockIdentity((buf + 44), header->sourcePortIdentity.clockIdentity);
	*(UInteger16 *) (buf + 52) = flip16(header->sourcePortIdentity.portNumber);
 finish:
	return result;
}


/*Unpack delayReq message from IN buffer of ptpClock to msgtmp.req*/
ssize_t
msgUnpackDelayReq(Octet *buf, size_t length, MsgDelayReq * delayreq)
{
	ssize_t result = UNPACK_INIT;
	CHECK_INPUT_LENGTH(34, 10, length, "delay req", result, finish);
	unpackUInteger48(buf + 34, length - 34, &delayreq->originTimestamp.secondsField, NULL);
	unpackUInteger32(buf + 40, length - 40, &delayreq->originTimestamp.nanosecondsField, NULL);
 finish:
	msgDelayReq_display(delayreq);
	return result;
}


/*Unpack PdelayReq message from IN buffer of ptpClock to msgtmp.req*/
ssize_t
msgUnpackPDelayReq(Octet *buf, size_t length, MsgPDelayReq * pdelayreq)
{
	ssize_t result = UNPACK_INIT;
	CHECK_INPUT_LENGTH(34, 10, length, "P delay req", result, finish);
	unpackUInteger48(buf + 34, length - 34, &pdelayreq->originTimestamp.secondsField, NULL);
	unpackUInteger32(buf + 40, length - 40, &pdelayreq->originTimestamp.nanosecondsField, NULL);
 finish:
	msgPDelayReq_display(pdelayreq);
	return result;
}


/*Unpack delayResp message from IN buffer of ptpClock to msgtmp.presp*/
ssize_t
msgUnpackDelayResp(Octet *buf, size_t length, MsgDelayResp * resp)
{
	ssize_t result = UNPACK_INIT;
	CHECK_INPUT_LENGTH(34, 20, length, "delay resp", result, finish);
	unpackUInteger48(buf + 34, length - 34, &resp->receiveTimestamp.secondsField, NULL);
	unpackUInteger32(buf + 40, length - 40, &resp->receiveTimestamp.nanosecondsField, NULL);
	copyClockIdentity(resp->requestingPortIdentity.clockIdentity,
	       (buf + 44));
	resp->requestingPortIdentity.portNumber = 
		flip16(*(UInteger16 *) (buf + 52));
 finish:
	msgDelayResp_display(resp);
	return result;
}


/*Unpack PdelayResp message from IN buffer of ptpClock to msgtmp.presp*/
ssize_t
msgUnpackPDelayResp(Octet *buf, size_t length, MsgPDelayResp * presp)
{
	ssize_t result = UNPACK_INIT;
	CHECK_INPUT_LENGTH(34, 20, length, "P delay resp", result, finish);
	unpackUInteger48(buf + 34, length - 34, &presp->requestReceiptTimestamp.secondsField, NULL);
	unpackUInteger32(buf + 40, length - 40, &presp->requestReceiptTimestamp.nanosecondsField, NULL);
	copyClockIdentity(presp->requestingPortIdentity.clockIdentity,
	       (buf + 44));
	presp->requestingPortIdentity.portNumber = 
		flip16(*(UInteger16 *) (buf + 52));
 finish:
	msgPDelayResp_display(presp);
	return result;
}

/*pack PdelayRespfollowup message into OUT buffer of ptpClock*/
ssize_t
msgPackPDelayRespFollowUp(Octet *buf, size_t space, MsgHeader * header,
			  const struct sfptpd_timespec * responseOriginTimestamp,
			  PtpClock * ptpClock, const UInteger16 sequenceId)
{
	ssize_t result;

	result = msgPackHeader(buf, space, ptpClock, PTPD_MSG_PDELAY_RESP_FOLLOW_UP);

	CHECK_OUTPUT_LENGTH(34, 20, space, "P delay resp follow-up", result, finish);
	
	/* Table 19 */
	*(UInteger16 *) (buf + 2) = flip16(PTPD_PDELAY_RESP_FOLLOW_UP_LENGTH);
	*(UInteger16 *) (buf + 30) = flip16(sequenceId);
	*(UInteger8 *) (buf + 32) = PTPD_CONTROL_FIELD_ALL_OTHERS;
	/* Table 23 */
	*(Integer8 *) (buf + 33) = 0x7F;
	/* Table 24 */

	/* PDelay_Resp_Follow_Up message includes the correctionField
         * value from the received PDelay_Req message PLUS the
	 * subnanosecond component we wish to add from our own
         * high precision timetamps. */
	if (msgSetPreciseTimestamp(buf, space,
				   responseOriginTimestamp,
				   false,
				   header->correctionField) != 0)
		result = PACK_ERROR;

	copyClockIdentity((buf + 44), header->sourcePortIdentity.clockIdentity);
	*(UInteger16 *) (buf + 52) = 
		flip16(header->sourcePortIdentity.portNumber);
 finish:
	return result;
}

/*Unpack PdelayResp message from IN buffer of ptpClock to msgtmp.presp*/
ssize_t
msgUnpackPDelayRespFollowUp(Octet *buf, size_t length, MsgPDelayRespFollowUp * prespfollow)
{
	ssize_t result = UNPACK_INIT;
	CHECK_INPUT_LENGTH(34, 20, length, "P delay resp follow-up", result, finish);
	unpackUInteger48(buf + 34, length - 34, &prespfollow->responseOriginTimestamp.secondsField, NULL);
	unpackUInteger32(buf + 40, length - 40, &prespfollow->responseOriginTimestamp.nanosecondsField, NULL);
	copyClockIdentity(prespfollow->requestingPortIdentity.clockIdentity,
	       (buf + 44));
	prespfollow->requestingPortIdentity.portNumber = 
		flip16(*(UInteger16 *) (buf + 52));
 finish:
	msgPDelayRespFollowUp_display(prespfollow);
	return result;
}

/* Pack Management message into OUT buffer */
ssize_t
msgPackManagementTLV(Octet *buf, size_t space, MsgManagement *outgoing, PtpClock *ptpClock)
{
        DBGV("packing ManagementTLV message \n");

	UInteger16 dataLength = 0;

	switch(outgoing->tlv->managementId)
	{
	case MM_NULL_MANAGEMENT:
	case MM_SAVE_IN_NON_VOLATILE_STORAGE:
	case MM_RESET_NON_VOLATILE_STORAGE:
	case MM_ENABLE_PORT:
	case MM_DISABLE_PORT:
		dataLength = 0;
		break;
	case MM_CLOCK_DESCRIPTION:
		dataLength = packMMClockDescription(outgoing, buf, space);
		mMClockDescription_display(
				(MMClockDescription*)outgoing->tlv->dataField, ptpClock);
		break;
        case MM_USER_DESCRIPTION:
                dataLength = packMMUserDescription(outgoing, buf, space);
                mMUserDescription_display(
                                (MMUserDescription*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_INITIALIZE:
                dataLength = packMMInitialize(outgoing, buf, space);
                mMInitialize_display(
                                (MMInitialize*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_DEFAULT_DATA_SET:
                dataLength = packMMDefaultDataSet(outgoing, buf, space);
                mMDefaultDataSet_display(
                                (MMDefaultDataSet*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_CURRENT_DATA_SET:
                dataLength = packMMCurrentDataSet(outgoing, buf, space);
                mMCurrentDataSet_display(
                                (MMCurrentDataSet*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_PARENT_DATA_SET:
                dataLength = packMMParentDataSet(outgoing, buf, space);
                mMParentDataSet_display(
                                (MMParentDataSet*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_TIME_PROPERTIES_DATA_SET:
                dataLength = packMMTimePropertiesDataSet(outgoing, buf, space);
                mMTimePropertiesDataSet_display(
                                (MMTimePropertiesDataSet*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_PORT_DATA_SET:
                dataLength = packMMPortDataSet(outgoing, buf, space);
                mMPortDataSet_display(
                                (MMPortDataSet*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_PRIORITY1:
                dataLength = packMMPriority1(outgoing, buf, space);
                mMPriority1_display(
                                (MMPriority1*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_PRIORITY2:
                dataLength = packMMPriority2(outgoing, buf, space);
                mMPriority2_display(
                                (MMPriority2*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_DOMAIN:
                dataLength = packMMDomain(outgoing, buf, space);
                mMDomain_display(
                                (MMDomain*)outgoing->tlv->dataField, ptpClock);
                break;
	case MM_SLAVE_ONLY:
		dataLength = packMMSlaveOnly(outgoing, buf, space);
		mMSlaveOnly_display(
				(MMSlaveOnly*)outgoing->tlv->dataField, ptpClock);
		break;
        case MM_LOG_ANNOUNCE_INTERVAL:
                dataLength = packMMLogAnnounceInterval(outgoing, buf, space);
                mMLogAnnounceInterval_display(
                                (MMLogAnnounceInterval*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_ANNOUNCE_RECEIPT_TIMEOUT:
                dataLength = packMMAnnounceReceiptTimeout(outgoing, buf, space);
                mMAnnounceReceiptTimeout_display(
                                (MMAnnounceReceiptTimeout*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_LOG_SYNC_INTERVAL:
                dataLength = packMMLogSyncInterval(outgoing, buf, space);
                mMLogSyncInterval_display(
                                (MMLogSyncInterval*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_VERSION_NUMBER:
                dataLength = packMMVersionNumber(outgoing, buf, space);
                mMVersionNumber_display(
                                (MMVersionNumber*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_TIME:
                dataLength = packMMTime(outgoing, buf, space);
                mMTime_display(
                                (MMTime*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_CLOCK_ACCURACY:
                dataLength = packMMClockAccuracy(outgoing, buf, space);
                mMClockAccuracy_display(
                                (MMClockAccuracy*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_UTC_PROPERTIES:
                dataLength = packMMUtcProperties(outgoing, buf, space);
                mMUtcProperties_display(
                                (MMUtcProperties*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_TRACEABILITY_PROPERTIES:
                dataLength = packMMTraceabilityProperties(outgoing, buf, space);
                mMTraceabilityProperties_display(
                                (MMTraceabilityProperties*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_DELAY_MECHANISM:
                dataLength = packMMDelayMechanism(outgoing, buf, space);
                mMDelayMechanism_display(
                                (MMDelayMechanism*)outgoing->tlv->dataField, ptpClock);
                break;
        case MM_LOG_MIN_PDELAY_REQ_INTERVAL:
                dataLength = packMMLogMinPdelayReqInterval(outgoing, buf, space);
                mMLogMinPdelayReqInterval_display(
                                (MMLogMinPdelayReqInterval*)outgoing->tlv->dataField, ptpClock);
                break;
	default:
		DBGV("packing management msg: unsupported id \n");
	}

	/* set the outgoing tlv lengthField to 2 + N where 2 is the managementId field
         * and N is dataLength, the length of the management tlv dataField field.
	 * See Table 39 of the spec.
	 */
	outgoing->tlv->lengthField = 2 + dataLength;

	return packManagementTLV((ManagementTLV*)outgoing->tlv, buf, space);
}

/* Pack ManagementErrorStatusTLV message into OUT buffer */
ssize_t
msgPackManagementErrorStatusTLV(Octet *buf, size_t space, MsgManagement *outgoing,
				PtpClock *ptpClock)
{
	DBGV("packing ManagementErrorStatusTLV message \n");

	UInteger16 dataLength = 0;

	dataLength = packMMErrorStatus(outgoing, buf, space);
	mMErrorStatus_display((MMErrorStatus*)outgoing->tlv->dataField, ptpClock);

	/* set the outgoing tlv lengthField to 2 + (6 + N) where 2 is the
	 * managementErrorId field and (6 + N) is dataLength, where 6 is
	 * the managementId and reserved field and N is the displayData field
	 * and optional pad field. See Table 71 of the spec.
	 */
	outgoing->tlv->lengthField = 2 + dataLength;

	return packManagementTLV((ManagementTLV*)outgoing->tlv, buf, space);
}

void
freeMMTLV(ManagementTLV* tlv) {
	DBGV("cleanup managementTLV data\n");
	switch(tlv->managementId)
	{
	case MM_CLOCK_DESCRIPTION:
		DBGV("cleanup clock description \n");
		freeMMClockDescription((MMClockDescription*)tlv->dataField);
		break;
	case MM_USER_DESCRIPTION:
		DBGV("cleanup user description \n");
		freeMMUserDescription((MMUserDescription*)tlv->dataField);
		break;
	case MM_NULL_MANAGEMENT:
	case MM_SAVE_IN_NON_VOLATILE_STORAGE:
	case MM_RESET_NON_VOLATILE_STORAGE:
	case MM_INITIALIZE:
	case MM_DEFAULT_DATA_SET:
	case MM_CURRENT_DATA_SET:
	case MM_PARENT_DATA_SET:
	case MM_TIME_PROPERTIES_DATA_SET:
	case MM_PORT_DATA_SET:
	case MM_PRIORITY1:
	case MM_PRIORITY2:
	case MM_DOMAIN:
	case MM_SLAVE_ONLY:
	case MM_LOG_ANNOUNCE_INTERVAL:
	case MM_ANNOUNCE_RECEIPT_TIMEOUT:
	case MM_LOG_SYNC_INTERVAL:
	case MM_VERSION_NUMBER:
	case MM_ENABLE_PORT:
	case MM_DISABLE_PORT:
	case MM_TIME:
	case MM_CLOCK_ACCURACY:
	case MM_UTC_PROPERTIES:
	case MM_TRACEABILITY_PROPERTIES:
	case MM_DELAY_MECHANISM:
	case MM_LOG_MIN_PDELAY_REQ_INTERVAL:
	default:
		DBGV("no managementTLV data to cleanup \n");
	}
}

void
freeMMErrorStatusTLV(ManagementTLV *tlv) {
	DBGV("cleanup managementErrorStatusTLV data \n");
	freeMMErrorStatus((MMErrorStatus*)tlv->dataField);
}

ssize_t
msgPackManagement(Octet *buf, size_t space, MsgManagement *outgoing, PtpClock *ptpClock)
{
	DBGV("packing management message \n");
	return packMsgManagement(outgoing, buf, space);
}

/*Unpack Management message from IN buffer of ptpClock to msgtmp.manage*/
ssize_t
msgUnpackManagement(Octet *buf, size_t length, MsgManagement * manage, MsgHeader * header, PtpClock *ptpClock)
{
	ssize_t result = unpackMsgManagement(buf, length, manage, ptpClock);

	/* Default outcome is that no TLV is attached to this message */
	manage->tlv = NULL;

	if ( UNPACK_OK(result) &&
	     manage->header.messageLength > PTPD_MANAGEMENT_LENGTH )
	{
		ssize_t resultTLV = unpackManagementTLV(buf, length, manage, ptpClock);

		if (UNPACK_OK(resultTLV)) {

			/* At this point, we know what managementTLV we have, so return and
			 * let someone else handle the data */
			manage->tlv->dataField = NULL;

			result = UNPACK_SIZE(UNPACK_GET_SIZE(result) +
					     UNPACK_GET_SIZE(resultTLV)) - PTPD_HEADER_LENGTH;
		}
	}

	return result;
}

/*Unpack Signaling message from IN buffer of ptpClock to msgtmp.signaling*/
ssize_t
msgUnpackSignaling(Octet *buf, size_t length, MsgSignaling * signaling, MsgHeader * header, PtpClock *ptpClock)
{
	ssize_t result = unpackMsgSignaling(buf, length, signaling, ptpClock);
	return UNPACK_OK(result) ? result - PTPD_HEADER_LENGTH : result;
}


ssize_t
msgUnpackTLVHeader(Octet *buf, size_t length, TLV *tlv, PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;

	CHECK_INPUT_LENGTH(0, 2, length, "tlv type", result, finish);
	unpackEnumeration16(buf, length, &tlv->tlvType, ptpClock);

	if (tlv->tlvType == 0 && length < 4) {
		/* In practice TLV types of zero are padding so don't raise noisy error */
		tlv->lengthField = 0;
		result = UNPACK_ERROR;
	} else {
		CHECK_INPUT_LENGTH(2, 2, length, "tlv length", result, finish);
		unpackUInteger16(buf + 2, length - 2, &tlv->lengthField, ptpClock);
	}
 finish:
	return result;
}


ssize_t
msgPackTLVHeader(Octet *buf, size_t space, TLV *tlv)
{
	ssize_t result = PACK_INIT;

	CHECK_OUTPUT_LENGTH(0, 4, space, "tlv header", result, finish);
	packEnumeration16(&tlv->tlvType, buf, space);
	packUInteger16(&tlv->lengthField, buf + 2, space - 2);
 finish:
	return result;
}


ssize_t
msgUnpackOrgTLVSubHeader(Octet *buf, size_t length,
			 UInteger24 *org_id, UInteger24 *org_subtype,
			 PtpClock* ptpClock)
{
	ssize_t result = UNPACK_INIT;

	CHECK_INPUT_LENGTH(0, 6, length, "org tlv subheader", result, finish);
	unpackUInteger24(buf, length, org_id, ptpClock);
	unpackUInteger24(buf + 3, length - 3, org_subtype, ptpClock);
 finish:
	return result;
}


ssize_t
msgPackOrgTLVHeader(Octet *buf,
		    size_t space,
		    bool forwarding,
		    UInteger24 organizationId,
		    UInteger24 organizationSubType)
{
	ssize_t result = PACK_INIT;
	uint16_t emptyLength = 6;
	Enumeration16 tlvType = forwarding ?
		PTPD_TLV_ORGANIZATION_EXTENSION_FORWARDING :
		PTPD_TLV_ORGANIZATION_EXTENSION_NON_FORWARDING;

	CHECK_OUTPUT_LENGTH(0, 10, space, "org tlv header", result, finish);
	packEnumeration16(&tlvType, buf, space);
	packUInteger16(&emptyLength, buf + 2, space - 2);
	packUInteger24(&organizationId, buf + 4, space - 4);
	packUInteger24(&organizationSubType, buf + 7, space - 7);
 finish:
	return result;
}


ssize_t
appendPTPMonRespTLV(PTPMonRespTLV *data, Octet *buf, size_t space)
{
	ssize_t result = UNPACK_INIT;
	int tlv_start = getHeaderLength(buf);
	int offset = tlv_start;

	data->reserved = 0;

	TLV_BOUNDARY_CHECK(offset, space);

	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/thirdparty/ptpmon_resp_tlv.def"
	PAD_TO_EVEN_LENGTH(buf, offset, space, result, finish);

	/* Set TLV length */
	setHeaderLength(buf + tlv_start, offset - tlv_start - PTPD_TLV_HEADER_LENGTH);

	/* Set message length */
	setHeaderLength(buf, offset);
	result = PACK_SIZE(offset);

 finish:
	/* return length */
	return result;
}

ssize_t
appendMTIERespTLV(MTIERespTLV *data, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int tlv_start = getHeaderLength(buf);
	int offset = tlv_start;

	data->reserved = 0;

	TLV_BOUNDARY_CHECK(offset, space);

	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/thirdparty/mtie_resp_tlv.def"
	PAD_TO_EVEN_LENGTH(buf, offset, space, result, finish);

	/* Set TLV length */
	setHeaderLength(buf + tlv_start, offset - tlv_start - PTPD_TLV_HEADER_LENGTH);

	/* Set message length */
	setHeaderLength(buf, offset);
	result = PACK_SIZE(offset);

 finish:
	/* return length */
	return result;
}

ssize_t
unpackPortCommunicationCapabilities( Octet *buf, size_t length, PortCommunicationCapabilities *data, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
        int offset = 0;

	#define OPERATE( name, size, type) STANDARD_UNPACKING_OPERATION(name, size, type)
        #include "../def/optional/port_communication_capabilities.def"
 finish:
	return result;
}

ssize_t
packPortCommunicationCapabilities(PortCommunicationCapabilities *data, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int offset = 0;

	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/optional/port_communication_capabilities.def"
 finish:
	return result;
}

ssize_t
appendPortCommunicationCapabilitiesTLV(PortCommunicationCapabilities *data, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int tlv_start = getHeaderLength(buf);
	int offset = tlv_start;
	int base;
	TLV tlv = { .tlvType = PTPD_TLV_PORT_COMMUNICATION_CAPABILITIES };

	TLV_BOUNDARY_CHECK(offset, space);

	result = msgPackTLVHeader(buf + tlv_start, space - tlv_start, &tlv);
	if (!PACK_OK(result)) goto finish;
	offset += result;
	base = offset;

	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/optional/port_communication_capabilities.def"

	PAD_TO_EVEN_LENGTH(buf, offset, space, result, finish);

	/* Set TLV length */
	setHeaderLength(buf + tlv_start, offset - base);

	/* Set message length */
	setHeaderLength(buf, offset);
	result = PACK_SIZE(offset);

 finish:
	/* return length */
	return result;
}

/**\brief Initialize outgoing signaling message fields*/
void signalingInitOutgoingMsg(MsgSignaling *outgoing,
			      PtpClock *ptpClock)
{
	/* set header fields */
	outgoing->header.majorSdoId = 0x0;
	outgoing->header.messageType = PTPD_MSG_SIGNALING;
	outgoing->header.minorVersionPTP = ptpClock->rtOpts.ptp_version_minor;
	outgoing->header.versionPTP = PTPD_PROTOCOL_VERSION;
	outgoing->header.messageLength = PTPD_SIGNALING_LENGTH;
	outgoing->header.domainNumber = ptpClock->domainNumber;
	outgoing->header.minorSdoId = 0x00;
	/* set header flagField to zero for management messages, Spec 13.3.2.6 */
	outgoing->header.flagField0 = 0x00;
	outgoing->header.flagField1 = 0x00;
	outgoing->header.correctionField = 0;
	outgoing->header.messageTypeSpecific = 0x00000000;
	copyPortIdentity(&outgoing->header.sourcePortIdentity, &ptpClock->portIdentity);
	outgoing->header.sequenceId = ptpClock->sentSignalingSequenceId;
	outgoing->header.controlField = PTPD_CONTROL_FIELD_ALL_OTHERS;
	outgoing->header.logMessageInterval = PTPD_MESSAGE_INTERVAL_UNDEFINED;

	/* set signaling message fields */
	/* default to all-ports target */
	memset(&outgoing->targetPortIdentity, 0xFF, sizeof outgoing->targetPortIdentity);
}


ssize_t unpackMsgSignaling(Octet *buf, size_t length, MsgSignaling *s, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;
	MsgSignaling* data = s;
	#define OPERATE( name, size, type) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type (buf + offset, length - offset, &data->name, ptpClock); \
		offset = offset + size;
	#include "../def/message/signaling.def"
 finish:
	msgSignaling_display(data);
	return result;
}

ssize_t
packMsgSignaling(MsgSignaling *data, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int offset = 0;

	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/message/signaling.def"
 finish:
	return result;
}

ssize_t unpackSlaveRxSyncTimingData(Octet *buf, size_t length, SlaveRxSyncTimingData *data, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;

	#define OPERATE( name, size, type) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type (buf + offset, length - offset, &data->name, ptpClock); \
		offset = offset + size;
	#include "../def/optional/slave_rx_sync_timing_data.def"
 finish:
	return result;
}

ssize_t
packSlaveRxSyncTimingData(SlaveRxSyncTimingData *data, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int offset = 0;

	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/optional/slave_rx_sync_timing_data.def"
 finish:
	return result;
}

ssize_t sizeSlaveRxSyncTimingDataElement(void)
{
	size_t result = 0;

	#define OPERATE( name, size, type) result += size;
	#include "../def/optional/slave_rx_sync_timing_data_element.def"

	return result;
}

ssize_t unpackSlaveRxSyncTimingDataElement(Octet *buf, size_t length, SlaveRxSyncTimingDataElement *data, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;

	#define OPERATE( name, size, type) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type (buf + offset, length - offset, &data->name, ptpClock); \
		offset = offset + size;
	#include "../def/optional/slave_rx_sync_timing_data_element.def"
 finish:
	return result;
}

void
freeSlaveRxSyncTimingDataTLV(SlaveRxSyncTimingDataTLV *tlv) {
	DBGV("cleanup slaveRxSyncTimingDataTLV data\n");
	free(tlv->elements);
}

ssize_t
packSlaveRxSyncTimingDataElement(SlaveRxSyncTimingDataElement *data, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int offset = 0;

	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/optional/slave_rx_sync_timing_data_element.def"
 finish:
	return result;
}

ssize_t unpackSlaveRxSyncTimingDataTLV(Octet *buf, size_t length,
				       SlaveRxSyncTimingDataTLV *data, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;
	int element;
	int num_elements;

	result = unpackSlaveRxSyncTimingData(buf, length, &data->preamble, ptpClock);
	assert(UNPACK_OK(result));
	offset += result;

	num_elements = (length - offset) / sizeSlaveRxSyncTimingDataElement();
	data->num_elements = num_elements;
	XMALLOC(data->elements, num_elements * sizeof(SlaveRxSyncTimingDataElement));

	for (element = 0; element < num_elements; element++) {
		result = unpackSlaveRxSyncTimingDataElement(buf + offset, length - offset,
							    &data->elements[element], ptpClock);
		if (!UNPACK_OK(result)) {
			free(data->elements);
			return result;
		}
		offset += result;
	}
	return UNPACK_SIZE(offset);
}

ssize_t
appendSlaveRxSyncTimingDataTLV(SlaveRxSyncTimingDataTLV *data,
			       Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int tlv_start = getHeaderLength(buf);
	int offset = tlv_start;
	int i;
	TLV tlv;

	tlv.tlvType = PTPD_TLV_SLAVE_RX_SYNC_TIMING_DATA;

	TLV_BOUNDARY_CHECK(offset, space);

	result = msgPackTLVHeader(buf + offset, space - offset, &tlv);
	assert(PACK_OK(result));
	offset += result;

	result = packSlaveRxSyncTimingData(&data->preamble,
					   buf + offset,
					   space - offset);
	assert(PACK_OK(result));
	offset += result;

	/* Now pack each element */
	for (i = 0; i < data->num_elements; i++) {
		offset += packSlaveRxSyncTimingDataElement(&data->elements[i],
							   buf + offset,
							   space - offset);
	}

	PAD_TO_EVEN_LENGTH(buf, offset, space, result, finish);

	/* Set TLV length */
	setHeaderLength(buf + tlv_start, offset - tlv_start - PTPD_TLV_HEADER_LENGTH);

	/* Set message length */
	setHeaderLength(buf, offset);
	result = PACK_SIZE(offset - tlv_start);

	/* return length */
	return result;
}

ssize_t unpackSlaveRxSyncComputedData(Octet *buf, size_t length, SlaveRxSyncComputedData *data, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;

	#define OPERATE( name, size, type) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type (buf + offset, length - offset, &data->name, ptpClock); \
		offset = offset + size;
	#include "../def/optional/slave_rx_sync_computed_data.def"
 finish:
	return result;
}

ssize_t sizeSlaveRxSyncComputedDataElement(void)
{
	size_t result = 0;

	#define OPERATE( name, size, type) result += size;
	#include "../def/optional/slave_rx_sync_computed_data_element.def"

	return result;
}

ssize_t unpackSlaveRxSyncComputedDataElement(Octet *buf, size_t length, SlaveRxSyncComputedDataElement *data, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;

	#define OPERATE( name, size, type) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type (buf + offset, length - offset, &data->name, ptpClock); \
		offset = offset + size;
	#include "../def/optional/slave_rx_sync_computed_data_element.def"
 finish:
	return result;
}

void
freeSlaveRxSyncComputedDataTLV(SlaveRxSyncComputedDataTLV *tlv) {
	DBGV("cleanup slaveRxSyncComputedDataTLV data\n");
	free(tlv->elements);
}

ssize_t
packSlaveRxSyncComputedDataElement(SlaveRxSyncComputedDataElement *data, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int offset = 0;

	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/optional/slave_rx_sync_computed_data_element.def"
 finish:
	return result;
}

ssize_t unpackSlaveRxSyncComputedDataTLV(Octet *buf, size_t length,
					 SlaveRxSyncComputedDataTLV *data, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;
	int element;
	int num_elements;

	result = unpackSlaveRxSyncComputedData(buf, length, &data->preamble, ptpClock);
	assert(UNPACK_OK(result));
	offset += result;

	num_elements = (length - offset) / sizeSlaveRxSyncComputedDataElement();
	data->num_elements = num_elements;
	XMALLOC(data->elements, num_elements * sizeof(SlaveRxSyncComputedDataElement));

	for (element = 0; element < num_elements; element++) {
		result = unpackSlaveRxSyncComputedDataElement(buf + offset, length - offset,
							    &data->elements[element], ptpClock);
		if (!UNPACK_OK(result)) {
			free(data->elements);
			return result;
		}
		offset += result;
	}
	return UNPACK_SIZE(offset);
}

ssize_t
appendSlaveRxSyncComputedDataTLV(SlaveRxSyncComputedData *data,
				 SlaveRxSyncComputedDataElement *elements,
				 int num_elements,
				 Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int tlv_start = getHeaderLength(buf);
	int offset = tlv_start;
	int i;
	TLV tlv;

	tlv.tlvType = PTPD_TLV_SLAVE_RX_SYNC_COMPUTED_DATA;

	TLV_BOUNDARY_CHECK(offset, space);

	result = msgPackTLVHeader(buf + offset, space - offset, &tlv);
	assert(PACK_OK(result));
	offset += result;

	/* Pack the header and preamble */
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/optional/slave_rx_sync_computed_data.def"

	/* Now pack each element */
	for (i = 0; i < num_elements; i++) {
		offset += packSlaveRxSyncComputedDataElement(&elements[i],
							     buf + offset,
							     space - offset);
	}

	PAD_TO_EVEN_LENGTH(buf, offset, space, result, finish);

	/* Set TLV length */
	setHeaderLength(buf + tlv_start, offset - tlv_start - PTPD_TLV_HEADER_LENGTH);

	/* Set message length */
	setHeaderLength(buf, offset);
	result = PACK_SIZE(offset - tlv_start);
 finish:
	/* return length */
	return result;
}


ssize_t unpackSlaveTxEventTimestamps(Octet *buf, size_t length, SlaveTxEventTimestamps *data, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;

	#define OPERATE( name, size, type) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type (buf + offset, length - offset, &data->name, ptpClock); \
		offset = offset + size;
	#include "../def/optional/slave_tx_event_timestamps.def"
 finish:
	return result;
}

ssize_t sizeSlaveTxEventTimestampsElement(void)
{
	size_t result = 0;

	#define OPERATE( name, size, type) result += size;
	#include "../def/optional/slave_tx_event_timestamps_element.def"

	return result;
}

ssize_t unpackSlaveTxEventTimestampsElement(Octet *buf, size_t length, SlaveTxEventTimestampsElement *data, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;

	#define OPERATE( name, size, type) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type (buf + offset, length - offset, &data->name, ptpClock); \
		offset = offset + size;
	#include "../def/optional/slave_tx_event_timestamps_element.def"
 finish:
	return result;
}

void
freeSlaveTxEventTimestampsTLV(SlaveTxEventTimestampsTLV *tlv) {
	DBGV("cleanup slaveTxEventTimestampsTLV data\n");
	free(tlv->elements);
}

ssize_t unpackSlaveTxEventTimestampsTLV(Octet *buf, size_t length,
					 SlaveTxEventTimestampsTLV *data, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;
	int element;
	int num_elements;

	result = unpackSlaveTxEventTimestamps(buf, length, &data->preamble, ptpClock);
	assert(UNPACK_OK(result));
	offset += result;

	num_elements = (length - offset) / sizeSlaveTxEventTimestampsElement();
	data->num_elements = num_elements;
	XMALLOC(data->elements, num_elements * sizeof(SlaveTxEventTimestampsElement));

	for (element = 0; element < num_elements; element++) {
		result = unpackSlaveTxEventTimestampsElement(buf + offset, length - offset,
							    &data->elements[element], ptpClock);
		if (!UNPACK_OK(result)) {
			free(data->elements);
			return result;
		}
		offset += result;
	}
	return UNPACK_SIZE(offset);
}

ssize_t
packSlaveTxEventTimestampsElement(SlaveTxEventTimestampsElement *data, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int offset = 0;

	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/optional/slave_tx_event_timestamps_element.def"
 finish:
	return result;
}

ssize_t
appendSlaveTxEventTimestampsTLV(SlaveTxEventTimestamps *data,
				SlaveTxEventTimestampsElement *elements,
				int num_elements,
				Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int tlv_start = getHeaderLength(buf);
	int offset = tlv_start;
	int i;
	TLV tlv;

	tlv.tlvType = PTPD_TLV_SLAVE_TX_EVENT_TIMESTAMPS;

	TLV_BOUNDARY_CHECK(offset, space);

	result = msgPackTLVHeader(buf + offset, space - offset, &tlv);
	assert(PACK_OK(result));
	offset += result;

	/* Pack the header and preamble */
	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/optional/slave_tx_event_timestamps.def"

	/* Now pack each element */
	for (i = 0; i < num_elements; i++) {
		offset += packSlaveTxEventTimestampsElement(&elements[i],
							    buf + offset,
							    space - offset);
	}

	PAD_TO_EVEN_LENGTH(buf, offset, space, result, finish);

	/* Set TLV length */
	setHeaderLength(buf + tlv_start, offset - tlv_start - PTPD_TLV_HEADER_LENGTH);

	/* Set message length */
	setHeaderLength(buf, offset);
	result = PACK_SIZE(offset - tlv_start);

 finish:
	/* return length */
	return result;
}


ssize_t unpackSlaveStatus(Octet *buf, size_t length, SlaveStatus *data, PtpClock *ptpClock)
{
	ssize_t result = UNPACK_INIT;
	int offset = 0;

	#define OPERATE( name, size, type) \
		CHECK_INPUT_LENGTH(offset, size, length, name, result, finish); \
		unpack##type (buf + offset, length - offset, &data->name, ptpClock); \
		offset = offset + size;
	#include "../def/sfc/slave_status.def"
 finish:
	return result;
}

ssize_t
packSlaveStatus(SlaveStatus *data, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int offset = 0;

	#define OPERATE( name, size, type ) STANDARD_PACKING_OPERATION(name, size, type)
	#include "../def/sfc/slave_status.def"
 finish:
	return result;
}


ssize_t
appendSlaveStatusTLV(SlaveStatus *data, Octet *buf, size_t space)
{
	ssize_t result = PACK_INIT;
	int tlv_start = getHeaderLength(buf);
	int offset = tlv_start;

	TLV_BOUNDARY_CHECK(offset, space);

	result = msgPackOrgTLVHeader(buf + offset, space - offset, false,
				     PTPD_SFC_TLV_ORGANISATION_ID,
				     PTPD_TLV_SFC_SLAVE_STATUS);
	assert(PACK_OK(result));
	offset += result;

	result = packSlaveStatus(data, buf + offset, space - offset);
	assert(PACK_OK(result));
	offset += result;

	PAD_TO_EVEN_LENGTH(buf, offset, space, result, finish);

	/* Set TLV length */
	setHeaderLength(buf + tlv_start, offset - tlv_start - PTPD_TLV_HEADER_LENGTH);

	/* Set message length */
	setHeaderLength(buf, offset);
	result = PACK_SIZE(offset - tlv_start);

	/* return length */
	return result;
}


/* Dump a packet */
void msgDump(PtpInterface *ptpInterface)
{
	char temp[MAXTIMESTR], time[MAXTIMESTR + 8];
	struct timeval now;
	sfptpd_secs_t s;

	gettimeofday(&now, 0);
	s = (sfptpd_secs_t) now.tv_sec;
	sfptpd_local_strftime(temp, sizeof(temp), "%Y-%m-%d %X", &s);
	snprintf(time, sizeof(time), "%s.%06ld", temp, now.tv_usec);
	time[sizeof(time) - 1] = '\0';

	msgDebugHeader(&ptpInterface->msgTmpHeader, time);
	switch (ptpInterface->msgTmpHeader.messageType) {
	case PTPD_MSG_SYNC:
		msgDebugSync(&ptpInterface->msgTmp.sync, time);
		break;

	case PTPD_MSG_ANNOUNCE:
		msgDebugAnnounce(&ptpInterface->msgTmp.announce, time);
		break;

	case PTPD_MSG_FOLLOW_UP:
		msgDebugFollowUp(&ptpInterface->msgTmp.follow, time);
		break;

	case PTPD_MSG_DELAY_REQ:
		msgDebugDelayReq(&ptpInterface->msgTmp.req, time);
		break;

	case PTPD_MSG_DELAY_RESP:
		msgDebugDelayResp(&ptpInterface->msgTmp.resp, time);
		break;

	case PTPD_MSG_PDELAY_REQ:
		msgDebugPDelayReq(&ptpInterface->msgTmp.preq, time);
		break;

	case PTPD_MSG_PDELAY_RESP:
		msgDebugPDelayResp(&ptpInterface->msgTmp.presp, time);
		break;

	case PTPD_MSG_PDELAY_RESP_FOLLOW_UP:
		msgDebugPDelayRespFollowUp(&ptpInterface->msgTmp.prespfollow, time);
		break;

	case PTPD_MSG_MANAGEMENT:
		msgDebugManagement(&ptpInterface->msgTmp.manage, time);
		break;

	default:
		WARNING("msgDump:unrecognized message\n");
		break;
	}
}

/**
 * Dump a PTP message header
 *
 * @param header a pre-filled msg header structure
 */
static void msgDebugHeader(MsgHeader *header, const char *time)
{
	int64_t correctionField = header->correctionField;
	
	printf("%s msgDebugHeader: messageType %d\n",
	       time, header->messageType);
	printf("%s msgDebugHeader: versionPTP %d\n",
	       time, header->versionPTP);
	printf("%s msgDebugHeader: messageLength %d\n",
	       time, header->messageLength);
	printf("%s msgDebugHeader: domainNumber %d\n",
	       time, header->domainNumber);
	printf("%s msgDebugHeader: flags %02hhx %02hhx\n", 
	       time, header->flagField0, header->flagField1);
	printf("%s msgDebugHeader: correctionfield %"PRIi64"\n",
	       time, correctionField);
	printf("%s msgDebugHeader: sourcePortIdentity.clockIdentity "
	       "%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx\n",
	       time, header->sourcePortIdentity.clockIdentity[0],
	       header->sourcePortIdentity.clockIdentity[1],
	       header->sourcePortIdentity.clockIdentity[2],
	       header->sourcePortIdentity.clockIdentity[3],
	       header->sourcePortIdentity.clockIdentity[4],
	       header->sourcePortIdentity.clockIdentity[5],
	       header->sourcePortIdentity.clockIdentity[6],
	       header->sourcePortIdentity.clockIdentity[7]);
	printf("%s msgDebugHeader: sourcePortIdentity.portNumber %d\n",
	       time, header->sourcePortIdentity.portNumber);
	printf("%s msgDebugHeader: sequenceId %d\n",
	       time, header->sequenceId);
	printf("%s msgDebugHeader: controlField %d\n",
	       time, header->controlField);
	printf("%s msgDebugHeader: logMessageInterval %d\n", 
	       time, header->logMessageInterval);

}

/**
 * Dump the contents of a sync packet
 *
 * @param sync A pre-filled MsgSync structure
 */
static void msgDebugSync(MsgSync *sync, const char *time)
{
	printf("%s msgDebugSync: originTimestamp.seconds %"PRIu64"\n",
	       time, sync->originTimestamp.secondsField);
	printf("%s msgDebugSync: originTimestamp.nanoseconds %d\n",
	       time, sync->originTimestamp.nanosecondsField);
}

/**
 * Dump the contents of a announce packet
 *
 * @param sync A pre-filled MsgAnnounce structure
 */
void msgDebugAnnounce(MsgAnnounce *announce, const char *time)
{
	printf("%s msgDebugAnnounce: originTimestamp.seconds %"PRIu64"\n",
	       time, announce->originTimestamp.secondsField);
	printf("%s msgDebugAnnounce: originTimestamp.nanoseconds %d\n",
	       time, announce->originTimestamp.nanosecondsField);
	printf("%s msgDebugAnnounce: currentUTCOffset %d\n", 
	       time, announce->currentUtcOffset);
	printf("%s msgDebugAnnounce: grandmasterPriority1 %d\n", 
	       time, announce->grandmasterPriority1);
	printf("%s msgDebugAnnounce: grandmasterClockQuality.clockClass %d\n",
	       time, announce->grandmasterClockQuality.clockClass);
	printf("%s msgDebugAnnounce: grandmasterClockQuality.clockAccuracy %d\n",
	       time, announce->grandmasterClockQuality.clockAccuracy);
	printf("%s msgDebugAnnounce: "
	       "grandmasterClockQuality.offsetScaledLogVariance %d\n",
	       time, announce->grandmasterClockQuality.offsetScaledLogVariance);
	printf("%s msgDebugAnnounce: grandmasterPriority2 %d\n", 
	       time, announce->grandmasterPriority2);
	printf("%s msgDebugAnnounce: grandmasterClockIdentity "
	       "%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx\n",
	       time,
	       announce->grandmasterIdentity[0],
	       announce->grandmasterIdentity[1],
	       announce->grandmasterIdentity[2],
	       announce->grandmasterIdentity[3],
	       announce->grandmasterIdentity[4],
	       announce->grandmasterIdentity[5],
	       announce->grandmasterIdentity[6],
	       announce->grandmasterIdentity[7]);
	printf("%s msgDebugAnnounce: stepsRemoved %d\n", 
	       time, announce->stepsRemoved);
	printf("%s msgDebugAnnounce: timeSource %d\n", 
	       time, announce->timeSource);
}

/**
 * Dump the contents of a followup packet
 *
 * @param follow A pre-fille MsgFollowUp structure
 */
static void msgDebugFollowUp(MsgFollowUp *follow, const char *time)
{
	printf("%s msgDebugFollowUp: preciseOriginTimestamp.seconds %"PRIu64"\n",
	       time, follow->preciseOriginTimestamp.secondsField);
	printf("%s msgDebugFollowUp: preciseOriginTimestamp.nanoseconds %d\n",
	       time, follow->preciseOriginTimestamp.nanosecondsField);
}

/**
 * Dump the contents of a delay request packet
 *
 * @param resp a pre-filled MsgDelayReq structure
 */
static void msgDebugDelayReq(MsgDelayReq *req, const char *time)
{
	printf("%s msgDebugDelayReq: originTimestamp.seconds %"PRIu64"\n",
	       time, req->originTimestamp.secondsField);
	printf("%s msgDebugDelayReq: originTimestamp.nanoseconds %d\n",
	       time, req->originTimestamp.nanosecondsField);
}

/**
 * Dump the contents of a delay response packet
 *
 * @param resp a pre-filled MsgDelayResp structure
 */
static void msgDebugDelayResp(MsgDelayResp *resp, const char *time)
{
	printf("%s msgDebugDelayResp: delayReceiptTimestamp.seconds %"PRIu64"\n",
	       time, resp->receiveTimestamp.secondsField);
	printf("%s msgDebugDelayResp: delayReceiptTimestamp.nanoseconds %d\n",
	       time, resp->receiveTimestamp.nanosecondsField);
	printf("%s msgDebugDelayResp: requestingPortIdentity.clockIdentity "
	       "%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx\n",
	       time, resp->requestingPortIdentity.clockIdentity[0],
	       resp->requestingPortIdentity.clockIdentity[1],
	       resp->requestingPortIdentity.clockIdentity[2],
	       resp->requestingPortIdentity.clockIdentity[3],
	       resp->requestingPortIdentity.clockIdentity[4],
	       resp->requestingPortIdentity.clockIdentity[5],
	       resp->requestingPortIdentity.clockIdentity[6],
	       resp->requestingPortIdentity.clockIdentity[7]);
	printf("%s msgDebugDelayResp: requestingPortIdentity.portNumber %d\n",
	       time, resp->requestingPortIdentity.portNumber);
}

/**
 * Dump the contents of a peer delay request packet
 *
 * @param req a pre-filled MsgPDelayReq structure
 */
static void msgDebugPDelayReq(MsgPDelayReq *req, const char *time)
{
	printf("%s msgDebugPDelayReq: originTimestamp.seconds %"PRIu64"\n",
	       time, req->originTimestamp.secondsField);
	printf("%s msgDebugPDelayReq: originTimestamp.nanoseconds %d\n",
	       time, req->originTimestamp.nanosecondsField);
}

/**
 * Dump the contents of a peer delay response packet
 *
 * @param resp a pre-filled MsgPDelayResp structure
 */
static void msgDebugPDelayResp(MsgPDelayResp *resp, const char *time)
{
	printf("%s msgDebugPDelayResp: requestReceiptTimestamp.seconds %"PRIu64"\n",
	       time, resp->requestReceiptTimestamp.secondsField);
	printf("%s msgDebugPDelayResp: requestReceiptTimestamp.nanoseconds %d\n",
	       time, resp->requestReceiptTimestamp.nanosecondsField);
	printf("%s msgDebugPDelayResp: requestingPortIdentity.clockIdentity "
	       "%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx\n",
	       time, resp->requestingPortIdentity.clockIdentity[0],
	       resp->requestingPortIdentity.clockIdentity[1],
	       resp->requestingPortIdentity.clockIdentity[2],
	       resp->requestingPortIdentity.clockIdentity[3],
	       resp->requestingPortIdentity.clockIdentity[4],
	       resp->requestingPortIdentity.clockIdentity[5],
	       resp->requestingPortIdentity.clockIdentity[6],
	       resp->requestingPortIdentity.clockIdentity[7]);
	printf("%s msgDebugPDelayResp: requestingPortIdentity.portNumber %d\n",
	       time, resp->requestingPortIdentity.portNumber);
}

/**
 * Dump the contents of a peer delay response follow up packet
 *
 * @param follow a pre-filled MsgPDelayRespFollowUp structure
 */
static void msgDebugPDelayRespFollowUp(MsgPDelayRespFollowUp *follow, const char *time)
{
	printf("%s msgDebugPDelayRespFollowUp: responseOriginTimestamp.seconds %"PRIu64"\n",
	       time, follow->responseOriginTimestamp.secondsField);
	printf("%s msgDebugPDelayRespFollowUp: responseOriginTimestamp.nanoseconds %d\n",
	       time, follow->responseOriginTimestamp.nanosecondsField);
	printf("%s msgDebugPDelayRespFollowUp: requestingPortIdentity.clockIdentity "
	       "%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx\n",
	       time, follow->requestingPortIdentity.clockIdentity[0],
	       follow->requestingPortIdentity.clockIdentity[1],
	       follow->requestingPortIdentity.clockIdentity[2],
	       follow->requestingPortIdentity.clockIdentity[3],
	       follow->requestingPortIdentity.clockIdentity[4],
	       follow->requestingPortIdentity.clockIdentity[5],
	       follow->requestingPortIdentity.clockIdentity[6],
	       follow->requestingPortIdentity.clockIdentity[7]);
	printf("%s msgDebugPDelayRespFollowUp: requestingPortIdentity.portNumber %d\n",
	       time, follow->requestingPortIdentity.portNumber);
}

/**
 * Dump the contents of management packet
 *
 * @param manage a pre-filled MsgManagement structure
 */
static void msgDebugManagement(MsgManagement *manage, const char *time)
{
	printf("%s msgDebugDelayManage: targetPortIdentity.clockIdentity "
	       "%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx\n",
	       time, manage->targetPortIdentity.clockIdentity[0],
	       manage->targetPortIdentity.clockIdentity[1],
	       manage->targetPortIdentity.clockIdentity[2],
	       manage->targetPortIdentity.clockIdentity[3],
	       manage->targetPortIdentity.clockIdentity[4],
	       manage->targetPortIdentity.clockIdentity[5],
	       manage->targetPortIdentity.clockIdentity[6],
	       manage->targetPortIdentity.clockIdentity[7]);
	printf("%s msgDebugDelayManage: targetPortIdentity.portNumber %d\n",
	       time, manage->targetPortIdentity.portNumber);
	printf("%s msgDebugManagement: startingBoundaryHops %d\n",
	       time, manage->startingBoundaryHops);
	printf("%s msgDebugManagement: boundaryHops %d\n",
	       time, manage->boundaryHops);
	printf("%s msgDebugManagement: actionField %d\n",
	       time, manage->actionField);
}


/* fin */

