/*
 * validator/val_sigcrypt.c - validator signature crypto functions.
 *
 * Copyright (c) 2007, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file contains helper functions for the validator module.
 * The functions help with signature verification and checking, the
 * bridging between RR wireformat data and crypto calls.
 */
#include "config.h"
#include "validator/val_sigcrypt.h"
#include "validator/validator.h"
#include "util/data/msgreply.h"
#include "util/data/dname.h"
#include "util/module.h"
#include "util/net_help.h"
#include "util/region-allocator.h"

#ifndef HAVE_SSL
#error "Need SSL library to do digital signature cryptography"
#endif

/** return number of rrs in an rrset */
static size_t
rrset_get_count(struct ub_packed_rrset_key* rrset)
{
	struct packed_rrset_data* d = (struct packed_rrset_data*)
	rrset->entry.data;
	if(!d) return 0;
	return d->count;
}

/**
 * Get RR signature count
 */
static size_t
rrset_get_sigcount(struct ub_packed_rrset_key* k)
{
	struct packed_rrset_data* d = (struct packed_rrset_data*)k->entry.data;
	return d->rrsig_count;
}

/**
 * Get signature keytag value
 * @param k: rrset (with signatures)
 * @param sig_idx: signature index.
 * @return keytag or 0 if malformed rrsig.
 */
static uint16_t 
rrset_get_sig_keytag(struct ub_packed_rrset_key* k, size_t sig_idx)
{
	uint16_t t;
	struct packed_rrset_data* d = (struct packed_rrset_data*)k->entry.data;
	log_assert(sig_idx < d->rrsig_count);
	if(d->rr_len[d->count + sig_idx] < 2+18)
		return 0;
	memmove(&t, d->rr_data[d->count + sig_idx]+2+16, 2);
	return t;
}

/**
 * Get signature signing algorithm value
 * @param k: rrset (with signatures)
 * @param sig_idx: signature index.
 * @return algo or 0 if malformed rrsig.
 */
static int 
rrset_get_sig_algo(struct ub_packed_rrset_key* k, size_t sig_idx)
{
	struct packed_rrset_data* d = (struct packed_rrset_data*)k->entry.data;
	log_assert(sig_idx < d->rrsig_count);
	if(d->rr_len[d->count + sig_idx] < 2+3)
		return 0;
	return (int)d->rr_data[d->count + sig_idx][2+2];
}

/** get rdata pointer and size */
static void
rrset_get_rdata(struct ub_packed_rrset_key* k, size_t idx, uint8_t** rdata,
	size_t* len)
{
	struct packed_rrset_data* d = (struct packed_rrset_data*)k->entry.data;
	log_assert(d && idx < (d->count + d->rrsig_count));
	*rdata = d->rr_data[idx];
	*len = d->rr_len[idx];
}

uint16_t
dnskey_get_flags(struct ub_packed_rrset_key* k, size_t idx)
{
	uint8_t* rdata;
	size_t len;
	uint16_t f;
	rrset_get_rdata(k, idx, &rdata, &len);
	if(len < 2+2)
		return 0;
	memmove(&f, rdata+2, 2);
	f = ntohs(f);
	return f;
}

int
dnskey_get_algo(struct ub_packed_rrset_key* k, size_t idx)
{
	uint8_t* rdata;
	size_t len;
	rrset_get_rdata(k, idx, &rdata, &len);
	if(len < 2+4)
		return 0;
	return (int)rdata[2+3];
}

int
ds_get_key_algo(struct ub_packed_rrset_key* k, size_t idx)
{
	uint8_t* rdata;
	size_t len;
	rrset_get_rdata(k, idx, &rdata, &len);
	if(len < 2+3)
		return 0;
	return (int)rdata[2+2];
}

/**
 * Get DS RR digest algorithm
 * @param k: DS rrset.
 * @param idx: which DS.
 * @return algorithm or 0 if DS too short.
 */
static int
ds_get_digest_algo(struct ub_packed_rrset_key* k, size_t idx)
{
	uint8_t* rdata;
	size_t len;
	rrset_get_rdata(k, idx, &rdata, &len);
	if(len < 2+4)
		return 0;
	return (int)rdata[2+3];
}

uint16_t 
ds_get_keytag(struct ub_packed_rrset_key* ds_rrset, size_t ds_idx)
{
	uint16_t t;
	uint8_t* rdata;
	size_t len;
	rrset_get_rdata(ds_rrset, ds_idx, &rdata, &len);
	if(len < 2+2)
		return 0;
	memmove(&t, rdata+2, 2);
	return t;
}

/**
 * Return pointer to the digest in a DS RR.
 * @param k: DS rrset.
 * @param idx: which DS.
 * @param digest: digest data is returned.
 *	on error, this is NULL.
 * @param len: length of digest is returned.
 *	on error, the length is 0.
 */
static void
ds_get_sigdata(struct ub_packed_rrset_key* k, size_t idx, uint8_t** digest,
        size_t* len)
{
	uint8_t* rdata;
	size_t rdlen;
	rrset_get_rdata(k, idx, &rdata, &rdlen);
	if(rdlen < 2+5) {
		*digest = NULL;
		*len = 0;
		return;
	}
	*digest = rdata + 2 + 4;
	*len = rdlen - 2 - 4;
}

/**
 * Return size of DS digest according to its hash algorithm.
 * @param k: DS rrset.
 * @param idx: which DS.
 * @return size in bytes of digest, or 0 if not supported. 
 */
static size_t
ds_digest_size_algo(struct ub_packed_rrset_key* k, size_t idx)
{
	switch(ds_get_digest_algo(k, idx)) {
#ifdef SHA_DIGEST_LENGTH
		case LDNS_SHA1:
			return SHA_DIGEST_LENGTH;
#endif
#ifdef SHA256_DIGEST_LENGTH
		case LDNS_SHA256:
			return SHA256_DIGEST_LENGTH;
#endif
		default: break;
	}
	return 0;
}

/**
 * Create a DS digest for a DNSKEY entry.
 *
 * @param env: module environment. Uses scratch space.
 * @param dnskey_rrset: DNSKEY rrset.
 * @param dnskey_idx: index of RR in rrset.
 * @param ds_rrset: DS rrset
 * @param ds_idx: index of RR in DS rrset.
 * @param digest: digest is returned in here (must be correctly sized).
 * @return false on error.
 */
static int
ds_create_dnskey_digest(struct module_env* env, 
	struct ub_packed_rrset_key* dnskey_rrset, size_t dnskey_idx,
	struct ub_packed_rrset_key* ds_rrset, size_t ds_idx,
	uint8_t* digest)
{
	ldns_buffer* b = env->scratch_buffer;
	uint8_t* dnskey_rdata;
	size_t dnskey_len;
	rrset_get_rdata(dnskey_rrset, dnskey_idx, &dnskey_rdata, &dnskey_len);

	/* create digest source material in buffer 
	 * digest = digest_algorithm( DNSKEY owner name | DNSKEY RDATA);
	 *	DNSKEY RDATA = Flags | Protocol | Algorithm | Public Key. */
	ldns_buffer_clear(b);
	ldns_buffer_write(b, dnskey_rrset->rk.dname, 
		dnskey_rrset->rk.dname_len);
	query_dname_tolower(ldns_buffer_begin(b));
	ldns_buffer_write(b, dnskey_rdata+2, dnskey_len-2); /* skip rdatalen*/
	ldns_buffer_flip(b);
	
	switch(ds_get_digest_algo(ds_rrset, ds_idx)) {
#ifdef SHA_DIGEST_LENGTH
		case LDNS_SHA1:
			(void)SHA1((unsigned char*)ldns_buffer_begin(b),
				ldns_buffer_limit(b), (unsigned char*)digest);
			return 1;
#endif
#ifdef SHA256_DIGEST_LENGTH
		case LDNS_SHA256:
			(void)SHA256((unsigned char*)ldns_buffer_begin(b),
				ldns_buffer_limit(b), (unsigned char*)digest);
			return 1;
#endif
		default: break;
	}
	return 0;
}

int ds_digest_match_dnskey(struct module_env* env,
	struct ub_packed_rrset_key* dnskey_rrset, size_t dnskey_idx,
	struct ub_packed_rrset_key* ds_rrset, size_t ds_idx)
{
	uint8_t* ds;	/* DS digest */
	size_t dslen;
	uint8_t* digest; /* generated digest */
	size_t digestlen = ds_digest_size_algo(ds_rrset, ds_idx);
	
	if(digestlen == 0)
		return 0; /* not supported, or DS RR format error */
	/* check digest length in DS with length from hash function */
	ds_get_sigdata(ds_rrset, ds_idx, &ds, &dslen);
	if(!ds || dslen != digestlen)
		return 0; /* DS algorithm and digest do not match */

	digest = region_alloc(env->scratch, digestlen);
	if(!digest)
		return 0; /* mem error */
	if(!ds_create_dnskey_digest(env, dnskey_rrset, dnskey_idx, ds_rrset, 
		ds_idx, digest))
		return 0; /* digest algo failed */
	if(memcmp(digest, ds, dslen) != 0)
		return 0; /* digest different */
	return 1;
}

int 
ds_digest_algo_is_supported(struct ub_packed_rrset_key* ds_rrset, 
	size_t ds_idx)
{
	return (ds_digest_size_algo(ds_rrset, ds_idx) != 0);
}

/** return true if DNSKEY algorithm id is supported */
static int
dnskey_algo_id_is_supported(int id)
{
	switch(id) {
	case LDNS_DSA:
	case LDNS_DSA_NSEC3:
	case LDNS_RSASHA1:
	case LDNS_RSASHA1_NSEC3:
	case LDNS_RSAMD5:
		return 1;
	default:
		return 0;
	}
}

int 
ds_key_algo_is_supported(struct ub_packed_rrset_key* ds_rrset, 
	size_t ds_idx)
{
	return dnskey_algo_id_is_supported(ds_get_key_algo(ds_rrset, ds_idx));
}

uint16_t 
dnskey_calc_keytag(struct ub_packed_rrset_key* dnskey_rrset, size_t dnskey_idx)
{
	uint8_t* data;
	size_t len;
	rrset_get_rdata(dnskey_rrset, dnskey_idx, &data, &len);
	/* do not pass rdatalen to ldns */
	return ldns_calc_keytag_raw(data+2, len-2);
}

int dnskey_algo_is_supported(struct ub_packed_rrset_key* dnskey_rrset,
        size_t dnskey_idx)
{
	return dnskey_algo_id_is_supported(dnskey_get_algo(dnskey_rrset, 
		dnskey_idx));
}

enum sec_status 
dnskeyset_verify_rrset(struct module_env* env, struct val_env* ve,
        struct ub_packed_rrset_key* rrset, struct ub_packed_rrset_key* dnskey)
{
	enum sec_status sec;
	size_t i, num;
	num = rrset_get_sigcount(rrset);
	if(num == 0) {
		verbose(VERB_ALGO, "rrset failed to verify due to a lack of "
			"signatures");
		return sec_status_bogus;
	}
	for(i=0; i<num; i++) {
		sec = dnskeyset_verify_rrset_sig(env, ve, rrset, dnskey, i);
		if(sec == sec_status_secure)
			return sec;
	}
	verbose(VERB_ALGO, "rrset failed to verify: all signatures are bogus");
	return sec_status_bogus;
}

enum sec_status 
dnskey_verify_rrset(struct module_env* env, struct val_env* ve,
        struct ub_packed_rrset_key* rrset, struct ub_packed_rrset_key* dnskey,
	        size_t dnskey_idx)
{
	enum sec_status sec;
	size_t i, num;
	num = rrset_get_sigcount(rrset);
	if(num == 0) {
		verbose(VERB_ALGO, "rrset failed to verify due to a lack of "
			"signatures");
		return sec_status_bogus;
	}
	for(i=0; i<num; i++) {
		sec = dnskey_verify_rrset_sig(env, ve, rrset, dnskey, 
			dnskey_idx, i);
		if(sec == sec_status_secure)
			return sec;
	}
	verbose(VERB_ALGO, "rrset failed to verify: all signatures are bogus");
	return sec_status_bogus;
}

enum sec_status 
dnskeyset_verify_rrset_sig(struct module_env* env, struct val_env* ve,
        struct ub_packed_rrset_key* rrset, struct ub_packed_rrset_key* dnskey,
	        size_t sig_idx)
{
	/* find matching keys and check them */
	enum sec_status sec = sec_status_bogus;
	uint16_t tag = rrset_get_sig_keytag(rrset, sig_idx);
	int algo = rrset_get_sig_algo(rrset, sig_idx);
	size_t i, num = rrset_get_count(dnskey);
	size_t numchecked = 0;
	
	for(i=0; i<num; i++) {
		/* see if key matches keytag and algo */
		if(algo != dnskey_get_algo(dnskey, i) ||
			tag != dnskey_calc_keytag(dnskey, i))
			continue;

		numchecked ++;
		/* see if key verifies */
		sec = dnskey_verify_rrset_sig(env, ve, rrset, dnskey, 
			i, sig_idx);
		if(sec == sec_status_secure)
			return sec;
	}
	if(numchecked == 0) {
		verbose(VERB_ALGO, "could not find appropriate key");
		return sec_status_bogus;
	}
	return sec_status_bogus;
}

/**
 * Sort RRs for rrset in canonical order.
 * Does not actually canonicalize the RR rdatas.
 * Does not touch rrsigs.
 * @param rrset: to sort.
 */
static void
canonical_sort(struct ub_packed_rrset_key* rrset)
{
	/* check if already sorted */
	/* remove duplicates */
}

/**
 * Inser canonical owner name into buffer.
 * @param buf: buffer to insert into at current position.
 * @param k: rrset with its owner name.
 * @param sig: signature with signer name and label count.
 * 	must be length checked, at least 18 bytes long.
 * @param can_owner: position in buffer returned for future use.
 * @param can_owner_len: length of canonical owner name.
 */
static void
insert_can_owner(ldns_buffer* buf, struct ub_packed_rrset_key* k,
	uint8_t* sig, uint8_t** can_owner, size_t* can_owner_len)
{
	int rrsig_labels = (int)sig[3];
	int fqdn_labels = dname_signame_label_count(k->rk.dname);
	*can_owner = ldns_buffer_current(buf);
	if(rrsig_labels == fqdn_labels) {
		/* no change */
		ldns_buffer_write(buf, k->rk.dname, k->rk.dname_len);
		query_dname_tolower(*can_owner);
		*can_owner_len = k->rk.dname_len;
		return;
	}
	log_assert(rrsig_labels < fqdn_labels);
	/* *. | fqdn(rightmost rrsig_labels) */
	if(rrsig_labels < fqdn_labels) {
		int i;
		uint8_t* nm = k->rk.dname;
		size_t len = k->rk.dname_len;
		/* so skip fqdn_labels-rrsig_labels */
		for(i=0; i<fqdn_labels-rrsig_labels; i++) {
			dname_remove_label(&nm, &len);	
		}
		*can_owner_len = len+2;
		ldns_buffer_write(buf, (uint8_t*)"\001*", 2);
		ldns_buffer_write(buf, nm, len);
		query_dname_tolower(*can_owner);
	}
}

/** 
 * Lowercase a text rdata field in a buffer.
 * @param p: pointer to start of text field (length byte).
 */
static void
lowercase_text_field(uint8_t* p)
{
	int i, len = (int)*p;
	p++;
	for(i=0; i<len; i++) {
		*p = (uint8_t)tolower((int)*p);
		p++;
	}
}

/**
 * Canonicalize Rdata in buffer.
 * @param buf: buffer at position just after the rdata.
 * @param rrset: rrset with type.
 * @param len: length of the rdata (including rdatalen uint16).
 */
static void
canonicalize_rdata(ldns_buffer* buf, struct ub_packed_rrset_key* rrset,
	size_t len)
{
	uint8_t* datstart = ldns_buffer_current(buf)-len+2;
	switch(ntohs(rrset->rk.type)) {
		case LDNS_RR_TYPE_NXT: 
		case LDNS_RR_TYPE_NSEC: /* type starts with the name */
		case LDNS_RR_TYPE_NS:
		case LDNS_RR_TYPE_MD:
		case LDNS_RR_TYPE_MF:
		case LDNS_RR_TYPE_CNAME:
		case LDNS_RR_TYPE_MB:
		case LDNS_RR_TYPE_MG:
		case LDNS_RR_TYPE_MR:
		case LDNS_RR_TYPE_PTR:
		case LDNS_RR_TYPE_DNAME:
			/* type only has a single argument, the name */
			query_dname_tolower(datstart);
			return;
		case LDNS_RR_TYPE_MINFO:
		case LDNS_RR_TYPE_RP:
		case LDNS_RR_TYPE_SOA:
			/* two names after another */
			query_dname_tolower(datstart);
			query_dname_tolower(datstart + 
				dname_valid(datstart, len-2));
			return;
		case LDNS_RR_TYPE_HINFO:
			/* lowercase text records */
			len -= 2;
			if(len < (size_t)datstart[0]+1)
				return;
			lowercase_text_field(datstart);
			len -= (size_t)datstart[0]+1; /* and skip the 1st */
			datstart += (size_t)datstart[0]+1;
			if(len < (size_t)datstart[0]+1)
				return;
			lowercase_text_field(datstart);
			return;
		case LDNS_RR_TYPE_RT:
		case LDNS_RR_TYPE_AFSDB:
		case LDNS_RR_TYPE_KX:
		case LDNS_RR_TYPE_MX:
			/* skip fixed part */
			if(len < 2+2+1) /* rdlen, skiplen, 1byteroot */
				return;
			datstart += 2;
			query_dname_tolower(datstart);
			return;
		case LDNS_RR_TYPE_SIG:
		case LDNS_RR_TYPE_RRSIG:
			/* skip fixed part */
			if(len < 2+18+1)
				return;
			datstart += 18;
			query_dname_tolower(datstart);
			return;
		case LDNS_RR_TYPE_PX:
			/* skip, then two names after another */
			if(len < 2+2+1) 
				return;
			datstart += 2;
			query_dname_tolower(datstart);
			query_dname_tolower(datstart + 
				dname_valid(datstart, len-2-2));
			return;
		case LDNS_RR_TYPE_NAPTR:
			if(len < 2+4)
				return;
			len -= 2+4;
			datstart += 4;
			if(len < (size_t)datstart[0]+1) /* skip text field */
				return;
			len -= (size_t)datstart[0]+1;
			datstart += (size_t)datstart[0]+1;
			if(len < (size_t)datstart[0]+1) /* skip text field */
				return;
			len -= (size_t)datstart[0]+1;
			datstart += (size_t)datstart[0]+1;
			if(len < (size_t)datstart[0]+1) /* skip text field */
				return;
			len -= (size_t)datstart[0]+1;
			datstart += (size_t)datstart[0]+1;
			if(len < 1)	/* check name is at least 1 byte*/
				return;
			query_dname_tolower(datstart);
			return;
		case LDNS_RR_TYPE_SRV:
			/* skip fixed part */
			if(len < 2+6+1)
				return;
			datstart += 6;
			query_dname_tolower(datstart);
			return;
		/* A6 not supported */
		default:	
			/* nothing to do for unknown types */
			return;
	}
}

/**
 * Create canonical form of rrset in the scratch buffer.
 * @param buf: the buffer to use.
 * @param k: the rrset to insert.
 * @param sig: RRSIG rdata to include.
 * @param siglen: RRSIG rdata len excluding signature field, but inclusive
 * 	signer name length.
 * @return false on alloc error.
 */
static int
rrset_canonical(ldns_buffer* buf, struct ub_packed_rrset_key* k, 
	uint8_t* sig, size_t siglen)
{
	struct packed_rrset_data* d = (struct packed_rrset_data*)k->entry.data;
	size_t i;
	uint8_t* can_owner = NULL;
	size_t can_owner_len = 0;
	/* sort RRs in place */
	canonical_sort(k);

	ldns_buffer_clear(buf);
	ldns_buffer_write(buf, sig, siglen);
	query_dname_tolower(sig+18); /* canonicalize signer name */
	for(i=0; i<d->count; i++) {
		/* determine canonical owner name */
		if(can_owner)
			ldns_buffer_write(buf, can_owner, can_owner_len);
		else	insert_can_owner(buf, k, sig, &can_owner, 
				&can_owner_len);
		ldns_buffer_write(buf, &k->rk.type, 2);
		ldns_buffer_write(buf, &k->rk.rrset_class, 2);
		ldns_buffer_write(buf, sig+4, 4);
		ldns_buffer_write(buf, d->rr_data[i], d->rr_len[i]);
		canonicalize_rdata(buf, k, d->rr_len[i]);
	}
	ldns_buffer_flip(buf);
	return 1;
}

/** check rrsig dates */
static int
check_dates(struct val_env* ve, uint8_t* expi_p, uint8_t* incep_p)
{
	/* read out the dates */
	int32_t expi, incep, now;
	memmove(&expi, expi_p, sizeof(expi));
	memmove(&incep, incep_p, sizeof(incep));
	expi = ntohl(expi);
	incep = ntohl(incep);

	/* get current date */
	if(ve->date_override) {
		now = ve->date_override;
		verbose(VERB_ALGO, "date override option %d", (int)now); 
	} else	now = (int32_t)time(0);

	/* check them */
	if(incep - expi > 0) {
		verbose(VERB_ALGO, "verify: inception after expiration, "
			"signature bad");
		return 0;
	}
	if(incep - now > 0) {
		verbose(VERB_ALGO, "verify: signature bad, current time is"
			" before inception date");
		return 0;
	}
	if(now - expi > 0) {
		verbose(VERB_ALGO, "verify: signature expired");
		return 0;
	}
	return 1;

}

enum sec_status 
dnskey_verify_rrset_sig(struct module_env* env, struct val_env* ve,
        struct ub_packed_rrset_key* rrset, struct ub_packed_rrset_key* dnskey,
	        size_t dnskey_idx, size_t sig_idx)
{
	uint8_t* sig; /* rdata */
	size_t siglen;
	size_t rrnum = rrset_get_count(rrset);
	uint8_t* signer;
	size_t signer_len;
	uint8_t* sigblock; /* signature rdata field */
	size_t sigblock_len;
	uint16_t ktag;
	rrset_get_rdata(rrset, rrnum + sig_idx, &sig, &siglen);
	/* min length of rdatalen, fixed rrsig, root signer, 1 byte sig */
	if(siglen < 2+20) {
		verbose(VERB_ALGO, "verify: signature too short");
		return sec_status_bogus;
	}

	if(!(dnskey_get_flags(dnskey, dnskey_idx) & DNSKEY_BIT_ZSK)) {
		verbose(VERB_ALGO, "verify: dnskey without ZSK flag");
		return sec_status_bogus; /* signer name invalid */
	}

	/* verify as many fields in rrsig as possible */
	signer = sig+2+18;
	signer_len = dname_valid(signer, siglen-2-18);
	if(!signer_len) {
		verbose(VERB_ALGO, "verify: malformed signer name");
		return sec_status_bogus; /* signer name invalid */
	}
	sigblock = signer+signer_len;
	if(siglen < 2+18+signer_len+1) {
		verbose(VERB_ALGO, "verify: too short, no signature data");
		return sec_status_bogus; /* sig rdf is < 1 byte */
	}
	sigblock_len = siglen - 2 - 18 - signer_len;

	/* verify key dname == sig signer name */
	if(query_dname_compare(signer, dnskey->rk.dname) != 0) {
		verbose(VERB_ALGO, "verify: wrong key for rrsig");
		return sec_status_bogus;
	}

	/* verify covered type */
	/* memcmp works because type is in network format for rrset */
	if(memcmp(sig+2, &rrset->rk.type, 2) != 0) {
		verbose(VERB_ALGO, "verify: wrong type covered");
		return sec_status_bogus;
	}
	/* verify keytag and sig algo (possibly again) */
	if((int)sig[2] != dnskey_get_algo(dnskey, dnskey_idx)) {
		verbose(VERB_ALGO, "verify: wrong algorithm");
		return sec_status_bogus;
	}
	ktag = dnskey_calc_keytag(dnskey, dnskey_idx);
	if(memcmp(sig+16, &ktag, 2) != 0) {
		verbose(VERB_ALGO, "verify: wrong keytag");
		return sec_status_bogus;
	}

	/* verify labels is in a valid range */
	if((int)sig[3] > dname_signame_label_count(rrset->rk.dname)) {
		verbose(VERB_ALGO, "verify: labelcount out of range");
		return sec_status_bogus;
	}

	/* original ttl, always ok */

	/* verify inception, expiration dates */
	if(!check_dates(ve, sig+8, sig+12)) {
		return sec_status_bogus;
	}

	/* create rrset canonical format in buffer, ready for signature */
	if(!rrset_canonical(env->scratch_buffer, rrset, sig+2, 
		18 + signer_len)) {
		log_err("verify: failed due to alloc error");
		return sec_status_unchecked;
	}

	/* verify */
	return sec_status_unchecked;
}
