/* SPDX-License-Identifier: BSD-2-Clause */
/*-
 * Copyright (c) 2014-2019 Xilinx, Inc. All rights reserved.
 * Copyright (c) 2013-2014 Wojciech Owczarek,
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
 * @file 	   ipv4_acl.c
 * @date   Sun Oct 13 21:02:23 2013
 * 
 * @brief  Code to handle IPv4 access control lists
 * 
 * Functions in this file parse, create and match IPv4 ACLs.
 * 
 */

#include "../ptpd.h"
#include "string.h"
#include "sfptpd_config_helpers.h"

/**
 * strdup + free are used across code using strtok_r, so as to
 * protect the original string, as strtok* modify it.
 */

/* count tokens in string delimited by delim */
static int countTokens(const char* text, const char* delim) {

    int count=0;
    char* stash = NULL;
    char* text_;
    char* text__;

    if(text==NULL || delim==NULL)
	return 0;

    text_=strdup(text);

    for(text__=text_,count=0;strtok_r(text__,delim, &stash) != NULL; text__=NULL) {
	    count++;
    }
    free(text_);
    return count;

}


/* Parse a single net mask into an AclEntry */
static int parseAclEntry(const char* line, AclEntry* acl) {

    struct sfptpd_acl_prefix prefix;

    if(line == NULL || acl == NULL)
	return -1;

    if(countTokens(line,"/") == 0)
	return -1;

    if (sfptpd_config_parse_net_prefix(&prefix, line, "ptp acl") != 0 ||
	prefix.af != AF_INET)
	return -1;

    acl->network = ntohl(prefix.addr.in.s_addr);
    acl->netmask = prefix.length;
    acl->bitmask = ~0 << (32 - prefix.length);
    acl->network &= acl->bitmask;

    return 1;
}


/* qsort() compare function for sorting ACL entries */
static int
cmpAclEntry(const void *p1, const void *p2)
{

	const AclEntry *left = p1;
	const AclEntry *right = p2;

	if(left->network > right->network)
		return 1;
	if(left->network < right->network)
		return -1;
	return 0;

}

/* Parse an ACL string into an aclEntry table and return number of entries. output can be NULL */
int
maskParser(const char* input, AclEntry* output)
{

    char* token;
    char* stash;
    int found = 0;
    char* text_;
    char* text__;
    AclEntry tmp;

    tmp.hitCount=0;

    if(strlen(input)==0) return 0;

    text_=strdup(input);

    for(text__=text_;;text__=NULL) {

	token=strtok_r(text__,", ;\t",&stash);
	if(token==NULL) break;

	if(parseAclEntry(token,&tmp)<1) {

	    found = -1;
	    break;

	}

	if(output != NULL)
	    output[found]=tmp;

	found++;


    }

	if(found && (output != NULL))
		qsort(output, found, sizeof(AclEntry), cmpAclEntry);

	/* We got input but found nothing - error */
	if (!found)
		found = -1;

	free(text_);
	return found;

}

/* Create a maskTable from a text ACL */
static MaskTable*
createMaskTable(const char* input)
{
	MaskTable* ret;
	int masksFound = maskParser(input, NULL);
	if(masksFound>=0) {
		ret=(MaskTable*)calloc(1,sizeof(MaskTable));
		ret->entries = (AclEntry*)calloc(masksFound, sizeof(AclEntry));
		ret->numEntries = maskParser(input,ret->entries);
		return ret;
	} else {
		ERROR("Error while parsing access list: \"%s\"\n", input);
		return NULL;
	}
}

/* Print the contents of a single mask table */
static void
dumpMaskTable(MaskTable* table)
{
	int i;
	uint32_t network;
	if(table == NULL)
	    return;
	INFO("number of entries: %d\n",table->numEntries);
	if(table->entries != NULL) {
		for(i = 0; i < table->numEntries; i++) {
		    AclEntry this = table->entries[i];
#ifdef PTPD_MSBF
		    network = this.network;
#else
		    network = htonl(this.network);
#endif
		    INFO("%d.%d.%d.%d/%d\t(0x%.8x/0x%.8x), matches: %d\n",
		    *((uint8_t*)&network), *((uint8_t*)&network+1),
		    *((uint8_t*)&network+2), *((uint8_t*)&network+3), this.netmask,
		    this.network, this.bitmask, this.hitCount);
		}
	}

}

/* Free a MaskTable structure */
static void freeMaskTable(MaskTable** table)
{
    if(*table == NULL)
	return;

    if((*table)->entries != NULL) {
	free((*table)->entries);
	(*table)->entries = NULL;
    }
    free(*table);
    *table = NULL;
}

/* Destroy an Ipv4AccessList structure */
void 
freeIpv4AccessList(Ipv4AccessList** acl)
{
	if(*acl == NULL)
	    return;

	freeMaskTable(&(*acl)->allowTable);
	freeMaskTable(&(*acl)->denyTable);

	free(*acl);
	*acl = NULL;
}

/* Structure initialisation for Ipv4AccessList */
Ipv4AccessList*
createIpv4AccessList(const char* allowList, const char* denyList, PtpdAclOrder processingOrder)
{
	Ipv4AccessList* ret;
	ret = (Ipv4AccessList*)calloc(1,sizeof(Ipv4AccessList));
	ret->allowTable = createMaskTable(allowList);
	ret->denyTable = createMaskTable(denyList);
	if(ret->allowTable == NULL || ret->denyTable == NULL) {
		freeIpv4AccessList(&ret);
		return NULL;
	}
	ret->processingOrder = processingOrder;
	return ret;
}


/* Match an IP address against a MaskTable */
static int
matchAddress(const uint32_t addr, MaskTable* table)
{

	int i;
	if(table == NULL || table->entries == NULL || table->numEntries==0)
	    return -1;
	for(i = 0; i < table->numEntries; i++) {
		DBGV("addr: %08x, addr & mask: %08x, network: %08x\n",addr, table->entries[i].bitmask & addr, table->entries[i].network);
		if((table->entries[i].bitmask & addr) == table->entries[i].network) {
			table->entries[i].hitCount++;
			return 1;
		}
	}

	return 0;

}


/* Test an IP address against an ACL */
int
matchIpv4AccessList(Ipv4AccessList* acl, const uint32_t addr)
{

	int ret;
	int matchAllow = 0;
	int matchDeny = 0;

	/* Non-functional ACL allows everything */
	if(acl == NULL) {
		ret = 1;
		goto end;
	}

	if(acl->allowTable != NULL)
		matchAllow = matchAddress(addr,acl->allowTable) > 0;

	if(acl->denyTable != NULL)
		matchDeny = matchAddress(addr,acl->denyTable) > 0;

	/* See http://httpd.apache.org/docs/2.2/mod/mod_authz_host.html#order
	 * for an explanation of the approach taken implementing ACLs.
	 */

	DBGV("ptp acl: order %s, matchAllow %d, matchDeny %d\n",
	     acl->processingOrder == PTPD_ACL_ALLOW_DENY ? "allow-deny" : "deny-allow",
	     matchAllow, matchDeny);

	switch(acl->processingOrder) {
	case PTPD_ACL_ALLOW_DENY:
		/* In this mode check the allow list then the deny
		 * list. If we match the allow list then check the
		 * deny list for an overriding deny rule.
		 * If no rules matched then deny. This matches the
		 * github ptpd2 project's behaviour and that of the
		 * Apache web server but 'permit' has been changed
		 * to 'allow' to ensure configurations relying on
		 * old sfptpd behaviour fail noisily.
		 */
		if(!matchAllow) {
			ret = 0;
			break;
		}
		if(matchDeny) {
			ret = 0;
			break;
		}
		ret = 1;
		break;
	default:
	case PTPD_ACL_DENY_ALLOW:
		/* In this mode check the deny list then the allow
		 * list. If we match the deny list then check the
		 * allow list for an overriding allow rule.
		 * If no rules matched then allow. This matches the
		 * github ptpd2 project's behaviour and that of the
		 * Apache web server but 'permit' has been changed
		 * to 'allow' to ensure configurations relying on
		 * old sfptpd behaviour fail noisily.
		 */
		if (matchDeny && !matchAllow) {
			ret = 0;
			break;
		}
		else {
			ret = 1;
			break;
		}
	}
	end:

	if(ret)
	    acl->passedCounter++;
	else
	    acl->droppedCounter++;

	return ret;
}


/* Dump the contents and hit counters of an ACL */
void dumpIpv4AccessList(Ipv4AccessList* acl)
{

		    INFO("\n\n");
	if(acl == NULL) {
		    INFO("(uninitialised ACL)\n");
		    return;
	}
	switch(acl->processingOrder) {
		case PTPD_ACL_DENY_ALLOW:
		    INFO("ACL order: deny,allow\n");
		    INFO("Passed packets: %d, dropped packets: %d\n",
				acl->passedCounter, acl->droppedCounter);
		    INFO("--------\n");
		    INFO("Deny list:\n");
		    dumpMaskTable(acl->denyTable);
		    INFO("--------\n");
		    INFO("Allow list:\n");
		    dumpMaskTable(acl->allowTable);
		break;
		case PTPD_ACL_ALLOW_DENY:
		default:
		    INFO("ACL order: allow,deny\n");
		    INFO("Passed packets: %d, dropped packets: %d\n",
				acl->passedCounter, acl->droppedCounter);
		    INFO("--------\n");
		    INFO("Allow list:\n");
		    dumpMaskTable(acl->allowTable);
		    INFO("--------\n");
		    INFO("Deny list:\n");
		    dumpMaskTable(acl->denyTable);
		break;
	}
		    INFO("\n\n");
}

/* Clear counters in a MaskTable */
static void
clearMaskTableCounters(MaskTable* table)
{

	int i, count;
	if(table==NULL || table->numEntries==0)
		return;
	count = table->numEntries;

	for(i=0; i<count; i++) {
		table->entries[i].hitCount = 0;
	}

}

/* Clear ACL counter */
void clearIpv4AccessListCounters(Ipv4AccessList* acl) {

	if(acl == NULL)
		return;
	acl->passedCounter=0;
	acl->droppedCounter=0;
	clearMaskTableCounters(acl->allowTable);
	clearMaskTableCounters(acl->denyTable);

}
