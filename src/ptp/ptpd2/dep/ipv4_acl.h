/**
 * @file   ipv4_acl.h
 * 
 * @brief  definitions related to IPv4 access control list handling
 * 
 * SPDX-License-Identifier: BSD-2-Clause
 * From ptpd v2.3. See PTPD2_COPYRIGHT.
 */

#ifndef PTPD_IPV4_ACL_H_
#define PTPD_IPV4_ACL_H_

#define IN_RANGE(num, min,max) \
	(num >= min && num <= max)

typedef enum {
	PTPD_ACL_ALLOW_DENY,
	PTPD_ACL_DENY_ALLOW
} PtpdAclOrder;

typedef struct {
	uint32_t network;
	uint32_t bitmask;
	uint16_t netmask;
	uint32_t hitCount;
} AclEntry;

typedef struct {
	int numEntries;
	AclEntry *entries;
} MaskTable;

typedef struct {
	MaskTable *allowTable;
	MaskTable *denyTable;
	int processingOrder;
	uint32_t passedCounter;
	uint32_t droppedCounter;
} Ipv4AccessList;

/* Parse string into AclEntry array */
int maskParser(const char *input, AclEntry *output);
/* Destroy an Ipv4AccessList structure */
void freeIpv4AccessList(Ipv4AccessList **acl);
/* Initialise an Ipv4AccessList structure */
Ipv4AccessList *createIpv4AccessList(const char *allowList,
				     const char *denyList,
				     PtpdAclOrder processingOrder);
/* Match on an IP address */
int matchIpv4AccessList(Ipv4AccessList *acl, const uint32_t addr);
/* Display the contents and hit counters of an access list */
void dumpIpv4AccessList(Ipv4AccessList *acl);
/* Clear counters */
void clearIpv4AccessListCounters(Ipv4AccessList *acl);

#endif /* IPV4_ACL_H_ */
