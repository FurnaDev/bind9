/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

#include <inttypes.h>
#include <stdbool.h>

#include <isc/base32.h>
#include <isc/mem.h>
#include <isc/md.h>
#include <isc/print.h>
#include <isc/string.h>
#include <isc/task.h>
#include <isc/util.h>

#include <dns/client.h>
#include <dns/db.h>
#include <dns/dnssec.h>
#include <dns/ds.h>
#include <dns/events.h>
#include <dns/keytable.h>
#include <dns/keyvalues.h>
#include <dns/log.h>
#include <dns/message.h>
#include <dns/ncache.h>
#include <dns/nsec.h>
#include <dns/nsec3.h>
#include <dns/rdata.h>
#include <dns/rdataset.h>
#include <dns/rdatatype.h>
#include <dns/resolver.h>
#include <dns/result.h>
#include <dns/validator.h>
#include <dns/view.h>

/*! \file
 * \brief
 * Basic processing sequences:
 *
 * \li When called with rdataset and sigrdataset:
 *     validator_start -> validate_answer -> proveunsecure
 *     validator_start -> validate_answer -> validate_nx (if secure wildcard)
 *
 * \li When called with rdataset but no sigrdataset:
 *     validator_start -> proveunsecure
 *
 * \li When called with no rdataset or sigrdataset:
 *     validator_start -> validate_nx-> proveunsecure
 *
 * validator_start:   determine what type of validation to do.
 * validate_answer:   attempt to perform a positive validation.
 * proveunsecure:     attempt to prove the answer comes from an unsecure zone.
 * validate_nx:       attempt to prove a negative response.
 */

#define VALIDATOR_MAGIC                 ISC_MAGIC('V', 'a', 'l', '?')
#define VALID_VALIDATOR(v)              ISC_MAGIC_VALID(v, VALIDATOR_MAGIC)

#define VALATTR_SHUTDOWN                0x0001  /*%< Shutting down. */
#define VALATTR_CANCELED                0x0002  /*%< Canceled. */
#define VALATTR_TRIEDVERIFY             0x0004  /*%< We have found a key and
						 * have attempted a verify. */
#define VALATTR_INSECURITY              0x0010 /*%< Attempting proveunsecure. */

/*!
 * NSEC proofs to be looked for.
 */
#define VALATTR_NEEDNOQNAME             0x00000100
#define VALATTR_NEEDNOWILDCARD          0x00000200
#define VALATTR_NEEDNODATA              0x00000400

/*!
 * NSEC proofs that have been found.
 */
#define VALATTR_FOUNDNOQNAME            0x00001000
#define VALATTR_FOUNDNOWILDCARD         0x00002000
#define VALATTR_FOUNDNODATA             0x00004000
#define VALATTR_FOUNDCLOSEST            0x00008000
#define VALATTR_FOUNDOPTOUT             0x00010000
#define VALATTR_FOUNDUNKNOWN            0x00020000

#define NEEDNODATA(val)      ((val->attributes & VALATTR_NEEDNODATA) != 0)
#define NEEDNOQNAME(val)     ((val->attributes & VALATTR_NEEDNOQNAME) != 0)
#define NEEDNOWILDCARD(val)  ((val->attributes & VALATTR_NEEDNOWILDCARD) != 0)
#define FOUNDNODATA(val)     ((val->attributes & VALATTR_FOUNDNODATA) != 0)
#define FOUNDNOQNAME(val)    ((val->attributes & VALATTR_FOUNDNOQNAME) != 0)
#define FOUNDNOWILDCARD(val) ((val->attributes & VALATTR_FOUNDNOWILDCARD) != 0)
#define FOUNDCLOSEST(val)    ((val->attributes & VALATTR_FOUNDCLOSEST) != 0)
#define FOUNDOPTOUT(val)     ((val->attributes & VALATTR_FOUNDOPTOUT) != 0)

#define SHUTDOWN(v)          (((v)->attributes & VALATTR_SHUTDOWN) != 0)
#define CANCELED(v)          (((v)->attributes & VALATTR_CANCELED) != 0)

#define NEGATIVE(r)       (((r)->attributes & DNS_RDATASETATTR_NEGATIVE) != 0)

static void
destroy(dns_validator_t *val);

static isc_result_t
get_dst_key(dns_validator_t *val, dns_rdata_rrsig_t *siginfo,
	    dns_rdataset_t *rdataset);

static isc_result_t
validate_answer(dns_validator_t *val, bool resume);

static isc_result_t
validatezonekey(dns_validator_t *val);

static isc_result_t
validate_nx(dns_validator_t *val, bool resume);

static isc_result_t
proveunsecure(dns_validator_t *val, bool have_ds, bool resume);

static void
validator_logv(dns_validator_t *val, isc_logcategory_t *category,
	       isc_logmodule_t *module, int level, const char *fmt, va_list ap)
	ISC_FORMAT_PRINTF(5, 0);

static void
validator_log(void *val, int level, const char *fmt, ...)
	ISC_FORMAT_PRINTF(3, 4);

static void
validator_logcreate(dns_validator_t *val,
		    dns_name_t *name, dns_rdatatype_t type,
		    const char *caller, const char *operation);

/*%
 * Ensure the validator's rdatasets are marked as expired.
 */
static void
expire_rdatasets(dns_validator_t *val) {
	if (dns_rdataset_isassociated(&val->frdataset)) {
		dns_rdataset_expire(&val->frdataset);
	}
	if (dns_rdataset_isassociated(&val->fsigrdataset)) {
		dns_rdataset_expire(&val->fsigrdataset);
	}
}

/*%
 * Ensure the validator's rdatasets are disassociated.
 */
static void
disassociate_rdatasets(dns_validator_t *val) {
	if (dns_rdataset_isassociated(&val->frdataset)) {
		dns_rdataset_disassociate(&val->frdataset);
	}
	if (dns_rdataset_isassociated(&val->fsigrdataset)) {
		dns_rdataset_disassociate(&val->fsigrdataset);
	}
}

/*%
 * Mark the rdatasets in val->event with trust level "answer",
 * indicating that they did not validate, but could be cached as insecure.
 *
 * If we are validating a name that is marked as "must be secure", log a
 * warning and return DNS_R_MUSTBESECURE instead.
 */
static inline isc_result_t
markanswer(dns_validator_t *val, const char *where, const char *mbstext) {
	if (val->mustbesecure && mbstext != NULL) {
		validator_log(val, ISC_LOG_WARNING,
			      "must be secure failure, %s", mbstext);
		return (DNS_R_MUSTBESECURE);
	}

	validator_log(val, ISC_LOG_DEBUG(3), "marking as answer (%s)", where);
	if (val->event->rdataset != NULL) {
		dns_rdataset_settrust(val->event->rdataset, dns_trust_answer);
	}
	if (val->event->sigrdataset != NULL) {
		dns_rdataset_settrust(val->event->sigrdataset,
				      dns_trust_answer);
	}

	return (ISC_R_SUCCESS);
}

/*%
 * Mark the RRsets with trust level secure.
 */
static inline void
marksecure(dns_validatorevent_t *event) {
	dns_rdataset_settrust(event->rdataset, dns_trust_secure);
	if (event->sigrdataset != NULL) {
		dns_rdataset_settrust(event->sigrdataset, dns_trust_secure);
	}
	event->secure = true;
}

static void
validator_done(dns_validator_t *val, isc_result_t result) {
	isc_task_t *task;

	if (val->event == NULL) {
		return;
	}

	/*
	 * Caller must be holding the lock.
	 */

	val->event->result = result;
	task = val->event->ev_sender;
	val->event->ev_sender = val;
	val->event->ev_type = DNS_EVENT_VALIDATORDONE;
	val->event->ev_action = val->action;
	val->event->ev_arg = val->arg;
	isc_task_sendanddetach(&task, (isc_event_t **)&val->event);
}

static inline bool
exit_check(dns_validator_t *val) {
	/*
	 * Caller must be holding the lock.
	 */
	if (!SHUTDOWN(val)) {
		return (false);
	}

	INSIST(val->event == NULL);

	if (val->fetch != NULL || val->subvalidator != NULL) {
		return (false);
	}

	return (true);
}

/*%
 * Look in the NSEC record returned from a DS query to see if there is
 * a NS RRset at this name.  If it is found we are at a delegation point.
 */
static bool
isdelegation(dns_name_t *name, dns_rdataset_t *rdataset,
	     isc_result_t dbresult)
{
	dns_fixedname_t fixed;
	dns_label_t hashlabel;
	dns_name_t nsec3name;
	dns_rdata_nsec3_t nsec3;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dns_rdataset_t set;
	int order;
	int scope;
	bool found;
	isc_buffer_t buffer;
	isc_result_t result;
	unsigned char hash[NSEC3_MAX_HASH_LENGTH];
	unsigned char owner[NSEC3_MAX_HASH_LENGTH];
	unsigned int length;

	REQUIRE(dbresult == DNS_R_NXRRSET || dbresult == DNS_R_NCACHENXRRSET);

	dns_rdataset_init(&set);
	if (dbresult == DNS_R_NXRRSET) {
		dns_rdataset_clone(rdataset, &set);
	} else {
		result = dns_ncache_getrdataset(rdataset, name,
						dns_rdatatype_nsec, &set);
		if (result == ISC_R_NOTFOUND) {
			goto trynsec3;
		}
		if (result != ISC_R_SUCCESS) {
			return (false);
		}
	}

	INSIST(set.type == dns_rdatatype_nsec);

	found = false;
	result = dns_rdataset_first(&set);
	if (result == ISC_R_SUCCESS) {
		dns_rdataset_current(&set, &rdata);
		found = dns_nsec_typepresent(&rdata, dns_rdatatype_ns);
		dns_rdata_reset(&rdata);
	}
	dns_rdataset_disassociate(&set);
	return (found);

 trynsec3:
	/*
	 * Iterate over the ncache entry.
	 */
	found = false;
	dns_name_init(&nsec3name, NULL);
	dns_fixedname_init(&fixed);
	dns_name_downcase(name, dns_fixedname_name(&fixed), NULL);
	name = dns_fixedname_name(&fixed);
	for (result = dns_rdataset_first(rdataset);
	     result == ISC_R_SUCCESS;
	     result = dns_rdataset_next(rdataset))
	{
		dns_ncache_current(rdataset, &nsec3name, &set);
		if (set.type != dns_rdatatype_nsec3) {
			dns_rdataset_disassociate(&set);
			continue;
		}
		dns_name_getlabel(&nsec3name, 0, &hashlabel);
		isc_region_consume(&hashlabel, 1);
		isc_buffer_init(&buffer, owner, sizeof(owner));
		result = isc_base32hexnp_decoderegion(&hashlabel, &buffer);
		if (result != ISC_R_SUCCESS) {
			dns_rdataset_disassociate(&set);
			continue;
		}
		for (result = dns_rdataset_first(&set);
		     result == ISC_R_SUCCESS;
		     result = dns_rdataset_next(&set))
		{
			dns_rdata_reset(&rdata);
			dns_rdataset_current(&set, &rdata);
			(void)dns_rdata_tostruct(&rdata, &nsec3, NULL);
			if (nsec3.hash != 1) {
				continue;
			}
			length = isc_iterated_hash(hash, nsec3.hash,
						   nsec3.iterations, nsec3.salt,
						   nsec3.salt_length,
						   name->ndata, name->length);
			if (length != isc_buffer_usedlength(&buffer)) {
				continue;
			}
			order = memcmp(hash, owner, length);
			if (order == 0) {
				found = dns_nsec3_typepresent(&rdata,
							      dns_rdatatype_ns);
				dns_rdataset_disassociate(&set);
				return (found);
			}
			if ((nsec3.flags & DNS_NSEC3FLAG_OPTOUT) == 0) {
				continue;
			}
			/*
			 * Does this optout span cover the name?
			 */
			scope = memcmp(owner, nsec3.next, nsec3.next_length);
			if ((scope < 0 && order > 0 &&
			     memcmp(hash, nsec3.next, length) < 0) ||
			    (scope >= 0 && (order > 0 ||
					    memcmp(hash, nsec3.next,
						   length) < 0)))
			{
				dns_rdataset_disassociate(&set);
				return (true);
			}
		}
		dns_rdataset_disassociate(&set);
	}
	return (found);
}

/*%
 * We have been asked to look for a key.
 * If found resume the validation process.
 * If not found fail the validation process.
 */
static void
fetch_callback_validator(isc_task_t *task, isc_event_t *event) {
	dns_fetchevent_t *devent;
	dns_validator_t *val;
	dns_rdataset_t *rdataset;
	bool want_destroy;
	isc_result_t result;
	isc_result_t eresult;
	isc_result_t saved_result;
	dns_fetch_t *fetch;

	UNUSED(task);
	INSIST(event->ev_type == DNS_EVENT_FETCHDONE);
	devent = (dns_fetchevent_t *)event;
	val = devent->ev_arg;
	rdataset = &val->frdataset;
	eresult = devent->result;

	/* Free resources which are not of interest. */
	if (devent->node != NULL) {
		dns_db_detachnode(devent->db, &devent->node);
	}
	if (devent->db != NULL) {
		dns_db_detach(&devent->db);
	}
	if (dns_rdataset_isassociated(&val->fsigrdataset)) {
		dns_rdataset_disassociate(&val->fsigrdataset);
	}
	isc_event_free(&event);

	INSIST(val->event != NULL);

	validator_log(val, ISC_LOG_DEBUG(3), "in fetch_callback_validator");
	LOCK(&val->lock);
	fetch = val->fetch;
	val->fetch = NULL;
	if (CANCELED(val)) {
		validator_done(val, ISC_R_CANCELED);
	} else if (eresult == ISC_R_SUCCESS) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "keyset with trust %s",
			      dns_trust_totext(rdataset->trust));
		/*
		 * Only extract the dst key if the keyset is secure.
		 */
		if (rdataset->trust >= dns_trust_secure) {
			result = get_dst_key(val, val->siginfo, rdataset);
			if (result == ISC_R_SUCCESS) {
				val->keyset = &val->frdataset;
			}
		}
		result = validate_answer(val, true);
		if (result == DNS_R_NOVALIDSIG &&
		    (val->attributes & VALATTR_TRIEDVERIFY) == 0)
		{
			saved_result = result;
			validator_log(val, ISC_LOG_DEBUG(3),
				      "falling back to insecurity proof");
			result = proveunsecure(val, false, false);
			if (result == DNS_R_NOTINSECURE) {
				result = saved_result;
			}
		}
		if (result != DNS_R_WAIT) {
			validator_done(val, result);
		}
	} else {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "fetch_callback_validator: got %s",
			      isc_result_totext(eresult));
		if (eresult == ISC_R_CANCELED) {
			validator_done(val, eresult);
		} else {
			validator_done(val, DNS_R_BROKENCHAIN);
		}
	}

	want_destroy = exit_check(val);
	UNLOCK(&val->lock);

	if (fetch != NULL) {
		dns_resolver_destroyfetch(&fetch);
	}

	if (want_destroy) {
		destroy(val);
	}
}

static void
dsfetched(isc_task_t *task, isc_event_t *event) {
	dns_fetchevent_t *devent;
	dns_validator_t *val;
	dns_rdataset_t *rdataset;
	bool want_destroy, trustchain;
	isc_result_t result;
	isc_result_t eresult;
	dns_fetch_t *fetch;

	UNUSED(task);
	INSIST(event->ev_type == DNS_EVENT_FETCHDONE);
	devent = (dns_fetchevent_t *)event;
	val = devent->ev_arg;
	rdataset = &val->frdataset;
	eresult = devent->result;

	/*
	 * Set to true if we're walking a chain of trust; false if
	 * we're attempting to prove insecurity.
	 */
	trustchain = ((val->attributes & VALATTR_INSECURITY) == 0);

	/* Free resources which are not of interest. */
	if (devent->node != NULL) {
		dns_db_detachnode(devent->db, &devent->node);
	}
	if (devent->db != NULL) {
		dns_db_detach(&devent->db);
	}
	if (dns_rdataset_isassociated(&val->fsigrdataset)) {
		dns_rdataset_disassociate(&val->fsigrdataset);
	}

	INSIST(val->event != NULL);

	validator_log(val, ISC_LOG_DEBUG(3), "in dsfetched");
	LOCK(&val->lock);
	fetch = val->fetch;
	val->fetch = NULL;

	if (CANCELED(val)) {
		validator_done(val, ISC_R_CANCELED);
		goto done;
	}

	switch (eresult) {
	case DNS_R_NXDOMAIN:
	case DNS_R_NCACHENXDOMAIN:
		/*
		 * These results only make sense if we're attempting
		 * an insecurity proof, not when walking a chain of trust.
		 */
		if (trustchain) {
			goto unexpected;
		}

		/* FALLTHROUGH */
	case ISC_R_SUCCESS:
		if (trustchain) {
			/*
			 * We looked for a DS record as part of
			 * following a key chain upwards; resume following
			 * the chain.
			 */
			validator_log(val, ISC_LOG_DEBUG(3),
				      "dsset with trust %s",
				      dns_trust_totext(rdataset->trust));
			val->dsset = &val->frdataset;
			result = validatezonekey(val);
			if (result != DNS_R_WAIT) {
				validator_done(val, result);
			}
		} else {
			/*
			 * There is a DS which may or may not be a zone cut.
			 * In either case we are still in a secure zone,
			 * so keep looking for the break in the chain
			 * of trust.
			 */
			result = proveunsecure(val, (eresult == ISC_R_SUCCESS),
					       true);
			if (result != DNS_R_WAIT) {
				validator_done(val, result);
			}
		}
		break;
	case DNS_R_CNAME:
	case DNS_R_NXRRSET:
	case DNS_R_NCACHENXRRSET:
	case DNS_R_SERVFAIL: /* RFC 1034 parent? */
		if (trustchain) {
			/*
			 * Failed to find a DS while following the
			 * chain of trust; now we need to prove insecurity.
			 */
			validator_log(val, ISC_LOG_DEBUG(3),
				      "falling back to insecurity proof (%s)",
				      dns_result_totext(eresult));
			result = proveunsecure(val, false, false);
			if (result != DNS_R_WAIT) {
				validator_done(val, result);
			}
		} else if (eresult == DNS_R_SERVFAIL) {
			goto unexpected;
		} else if (eresult != DNS_R_CNAME &&
			   isdelegation(dns_fixedname_name(&devent->foundname),
					&val->frdataset, eresult))
		{
			/*
			 * Failed to find a DS while trying to prove
			 * insecurity. If this is a zone cut, that
			 * means we're insecure.
			 */
			result = markanswer(val, "dsfetched",
					    "no DS and this is a delegation");
			validator_done(val, result);
		} else {
			/*
			 * Not a zone cut, so we have to keep looking for
			 * the break point in the chain of trust.
			 */
			result = proveunsecure(val, false, true);
			if (result != DNS_R_WAIT) {
				validator_done(val, result);
			}
		}
		break;

	default:
 unexpected:
		validator_log(val, ISC_LOG_DEBUG(3), "dsfetched: got %s",
			      isc_result_totext(eresult));
		if (eresult == ISC_R_CANCELED) {
			validator_done(val, eresult);
		} else {
			validator_done(val, DNS_R_BROKENCHAIN);
		}
	}
 done:

	isc_event_free(&event);
	want_destroy = exit_check(val);
	UNLOCK(&val->lock);

	if (fetch != NULL) {
		dns_resolver_destroyfetch(&fetch);
	}

	if (want_destroy) {
		destroy(val);
	}
}

/*%
 * Callback from when a DNSKEY RRset has been validated.
 *
 * Resumes the stalled validation process.
 */
static void
keyvalidated(isc_task_t *task, isc_event_t *event) {
	dns_validatorevent_t *devent;
	dns_validator_t *val;
	bool want_destroy;
	isc_result_t result;
	isc_result_t eresult;
	isc_result_t saved_result;

	UNUSED(task);
	INSIST(event->ev_type == DNS_EVENT_VALIDATORDONE);

	devent = (dns_validatorevent_t *)event;
	val = devent->ev_arg;
	eresult = devent->result;

	isc_event_free(&event);
	dns_validator_destroy(&val->subvalidator);

	INSIST(val->event != NULL);

	validator_log(val, ISC_LOG_DEBUG(3), "in keyvalidated");
	LOCK(&val->lock);
	if (CANCELED(val)) {
		validator_done(val, ISC_R_CANCELED);
	} else if (eresult == ISC_R_SUCCESS) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "keyset with trust %s",
			      dns_trust_totext(val->frdataset.trust));
		/*
		 * Only extract the dst key if the keyset is secure.
		 */
		if (val->frdataset.trust >= dns_trust_secure) {
			(void) get_dst_key(val, val->siginfo, &val->frdataset);
		}
		result = validate_answer(val, true);
		if (result == DNS_R_NOVALIDSIG &&
		    (val->attributes & VALATTR_TRIEDVERIFY) == 0)
		{
			saved_result = result;
			validator_log(val, ISC_LOG_DEBUG(3),
				      "falling back to insecurity proof");
			result = proveunsecure(val, false, false);
			if (result == DNS_R_NOTINSECURE) {
				result = saved_result;
			}
		}
		if (result != DNS_R_WAIT) {
			validator_done(val, result);
		}
	} else {
		if (eresult != DNS_R_BROKENCHAIN) {
			expire_rdatasets(val);
		}
		validator_log(val, ISC_LOG_DEBUG(3),
			      "keyvalidated: got %s",
			      isc_result_totext(eresult));
		validator_done(val, DNS_R_BROKENCHAIN);
	}

	want_destroy = exit_check(val);
	UNLOCK(&val->lock);
	if (want_destroy) {
		destroy(val);
	}
}

/*%
 * Callback when the DS record has been validated.
 *
 * Resumes validation of the zone key or the unsecure zone proof.
 */
static void
dsvalidated(isc_task_t *task, isc_event_t *event) {
	dns_validatorevent_t *devent;
	dns_validator_t *val;
	bool want_destroy;
	isc_result_t result;
	isc_result_t eresult;

	UNUSED(task);
	INSIST(event->ev_type == DNS_EVENT_VALIDATORDONE);

	devent = (dns_validatorevent_t *)event;
	val = devent->ev_arg;
	eresult = devent->result;

	isc_event_free(&event);
	dns_validator_destroy(&val->subvalidator);

	INSIST(val->event != NULL);

	validator_log(val, ISC_LOG_DEBUG(3), "in dsvalidated");
	LOCK(&val->lock);
	if (CANCELED(val)) {
		validator_done(val, ISC_R_CANCELED);
	} else if (eresult == ISC_R_SUCCESS) {
		bool have_dsset;
		dns_name_t *name;
		validator_log(val, ISC_LOG_DEBUG(3),
			      "%s with trust %s",
			      val->frdataset.type == dns_rdatatype_ds ?
			      "dsset" : "ds non-existance",
			      dns_trust_totext(val->frdataset.trust));
		have_dsset = (val->frdataset.type == dns_rdatatype_ds);
		name = dns_fixedname_name(&val->fname);
		if ((val->attributes & VALATTR_INSECURITY) != 0 &&
		    val->frdataset.covers == dns_rdatatype_ds &&
		    NEGATIVE(&val->frdataset) &&
		    isdelegation(name, &val->frdataset, DNS_R_NCACHENXRRSET))
		{
			result = markanswer(val, "dsvalidated",
					    "no DS and this is a delegation");
		} else if ((val->attributes & VALATTR_INSECURITY) != 0) {
			result = proveunsecure(val, have_dsset, true);
		} else {
			result = validatezonekey(val);
		}
		if (result != DNS_R_WAIT) {
			validator_done(val, result);
		}
	} else {
		if (eresult != DNS_R_BROKENCHAIN) {
			expire_rdatasets(val);
		}
		validator_log(val, ISC_LOG_DEBUG(3),
			      "dsvalidated: got %s",
			      isc_result_totext(eresult));
		validator_done(val, DNS_R_BROKENCHAIN);
	}

	want_destroy = exit_check(val);
	UNLOCK(&val->lock);
	if (want_destroy) {
		destroy(val);
	}
}

/*%
 * Callback when the CNAME record has been validated.
 *
 * Resumes validation of the unsecure zone proof.
 */
static void
cnamevalidated(isc_task_t *task, isc_event_t *event) {
	dns_validatorevent_t *devent;
	dns_validator_t *val;
	bool want_destroy;
	isc_result_t result;
	isc_result_t eresult;

	UNUSED(task);
	INSIST(event->ev_type == DNS_EVENT_VALIDATORDONE);

	devent = (dns_validatorevent_t *)event;
	val = devent->ev_arg;
	eresult = devent->result;

	isc_event_free(&event);
	dns_validator_destroy(&val->subvalidator);

	INSIST(val->event != NULL);
	INSIST((val->attributes & VALATTR_INSECURITY) != 0);

	validator_log(val, ISC_LOG_DEBUG(3), "in cnamevalidated");
	LOCK(&val->lock);
	if (CANCELED(val)) {
		validator_done(val, ISC_R_CANCELED);
	} else if (eresult == ISC_R_SUCCESS) {
		validator_log(val, ISC_LOG_DEBUG(3), "cname with trust %s",
			      dns_trust_totext(val->frdataset.trust));
		result = proveunsecure(val, false, true);
		if (result != DNS_R_WAIT) {
			validator_done(val, result);
		}
	} else {
		if (eresult != DNS_R_BROKENCHAIN) {
			expire_rdatasets(val);
		}
		validator_log(val, ISC_LOG_DEBUG(3),
			      "cnamevalidated: got %s",
			      isc_result_totext(eresult));
		validator_done(val, DNS_R_BROKENCHAIN);
	}

	want_destroy = exit_check(val);
	UNLOCK(&val->lock);
	if (want_destroy) {
		destroy(val);
	}
}

/*%
 * Callback for when NSEC records have been validated.
 *
 * Looks for NOQNAME, NODATA and OPTOUT proofs.
 *
 * Resumes validate_nx.
 */
static void
authvalidated(isc_task_t *task, isc_event_t *event) {
	dns_validatorevent_t *devent;
	dns_validator_t *val;
	dns_rdataset_t *rdataset;
	bool want_destroy;
	isc_result_t result;
	bool exists, data;

	UNUSED(task);
	INSIST(event->ev_type == DNS_EVENT_VALIDATORDONE);

	devent = (dns_validatorevent_t *)event;
	rdataset = devent->rdataset;
	val = devent->ev_arg;
	result = devent->result;
	dns_validator_destroy(&val->subvalidator);

	INSIST(val->event != NULL);

	validator_log(val, ISC_LOG_DEBUG(3), "in authvalidated");
	LOCK(&val->lock);
	if (CANCELED(val)) {
		validator_done(val, ISC_R_CANCELED);
	} else if (result != ISC_R_SUCCESS) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "authvalidated: got %s",
			      isc_result_totext(result));
		if (result == DNS_R_BROKENCHAIN) {
			val->authfail++;
		}
		if (result == ISC_R_CANCELED) {
			validator_done(val, result);
		} else {
			result = validate_nx(val, true);
			if (result != DNS_R_WAIT) {
				validator_done(val, result);
			}
		}
	} else {
		dns_name_t **proofs = val->event->proofs;
		dns_name_t *wild = dns_fixedname_name(&val->wild);

		if (rdataset->trust == dns_trust_secure) {
			val->seensig = true;
		}

		if (rdataset->type == dns_rdatatype_nsec &&
		    rdataset->trust == dns_trust_secure &&
		    (NEEDNODATA(val) || NEEDNOQNAME(val)) &&
		    !FOUNDNODATA(val) && !FOUNDNOQNAME(val) &&
		    dns_nsec_noexistnodata(val->event->type, val->event->name,
					   devent->name, rdataset, &exists,
					   &data, wild, validator_log, val)
					== ISC_R_SUCCESS)
		{
			if (exists && !data) {
				val->attributes |= VALATTR_FOUNDNODATA;
				if (NEEDNODATA(val)) {
					proofs[DNS_VALIDATOR_NODATAPROOF] =
						devent->name;
				}
			}
			if (!exists) {
				dns_name_t *closest;
				unsigned int clabels;

				val->attributes |= VALATTR_FOUNDNOQNAME;

				closest = dns_fixedname_name(&val->closest);
				clabels = dns_name_countlabels(closest);
				/*
				 * If we are validating a wildcard response
				 * clabels will not be zero.  We then need
				 * to check if the generated wilcard from
				 * dns_nsec_noexistnodata is consistent with
				 * the wildcard used to generate the response.
				 */
				if (clabels == 0 ||
				    dns_name_countlabels(wild) == clabels + 1)
				{
					val->attributes |= VALATTR_FOUNDCLOSEST;
				}
				/*
				 * The NSEC noqname proof also contains
				 * the closest encloser.
				 */
				if (NEEDNOQNAME(val)) {
					proofs[DNS_VALIDATOR_NOQNAMEPROOF] =
						devent->name;
				}
			}
		}

		result = validate_nx(val, true);
		if (result != DNS_R_WAIT) {
			validator_done(val, result);
		}
	}

	want_destroy = exit_check(val);
	UNLOCK(&val->lock);
	if (want_destroy) {
		destroy(val);
	}

	/*
	 * Free stuff from the event.
	 */
	isc_event_free(&event);
}

/*%
 * Looks for the requested name and type in the view (zones and cache).
 *
 * Returns:
 * \li	ISC_R_SUCCESS
 * \li	ISC_R_NOTFOUND
 * \li	DNS_R_NCACHENXDOMAIN
 * \li	DNS_R_NCACHENXRRSET
 * \li	DNS_R_NXRRSET
 * \li	DNS_R_NXDOMAIN
 * \li	DNS_R_BROKENCHAIN
 */
static inline isc_result_t
view_find(dns_validator_t *val, dns_name_t *name, dns_rdatatype_t type) {
	dns_fixedname_t fixedname;
	dns_name_t *foundname;
	isc_result_t result;
	unsigned int options;
	isc_time_t now;
	char namebuf[DNS_NAME_FORMATSIZE];
	char typebuf[DNS_RDATATYPE_FORMATSIZE];

	disassociate_rdatasets(val);

	if (isc_time_now(&now) == ISC_R_SUCCESS &&
	    dns_resolver_getbadcache(val->view->resolver, name, type, &now))
	{
		dns_name_format(name, namebuf, sizeof(namebuf));
		dns_rdatatype_format(type, typebuf, sizeof(typebuf));
		validator_log(val, ISC_LOG_INFO, "bad cache hit (%s/%s)",
			      namebuf, typebuf);
		return (DNS_R_BROKENCHAIN);
	}

	options = DNS_DBFIND_PENDINGOK;
	foundname = dns_fixedname_initname(&fixedname);
	result = dns_view_find(val->view, name, type, 0, options,
			       false, false, NULL, NULL, foundname,
			       &val->frdataset, &val->fsigrdataset);

	if (result == DNS_R_NXDOMAIN) {
		goto notfound;
	} else if (result != ISC_R_SUCCESS &&
		   result != DNS_R_NCACHENXDOMAIN &&
		   result != DNS_R_NCACHENXRRSET &&
		   result != DNS_R_EMPTYNAME &&
		   result != DNS_R_NXRRSET &&
		   result != ISC_R_NOTFOUND)
	{
		result = ISC_R_NOTFOUND;
		goto notfound;
	}

	return (result);

 notfound:
	disassociate_rdatasets(val);

	return (result);
}

/*%
* Checks to make sure we are not going to loop.  As we use a SHARED fetch
* the validation process will stall if looping was to occur.
*/
static inline bool
check_deadlock(dns_validator_t *val, dns_name_t *name, dns_rdatatype_t type,
       dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset)
{
	dns_validator_t *parent;

	for (parent = val; parent != NULL; parent = parent->parent) {
		if (parent->event != NULL &&
		    parent->event->type == type &&
		    dns_name_equal(parent->event->name, name) &&
		    /*
		     * As NSEC3 records are meta data you sometimes
		     * need to prove a NSEC3 record which says that
		     * itself doesn't exist.
		     */
		    (parent->event->type != dns_rdatatype_nsec3 ||
		     rdataset == NULL || sigrdataset == NULL ||
		     parent->event->message == NULL ||
		     parent->event->rdataset != NULL ||
		     parent->event->sigrdataset != NULL))
		{
			validator_log(val, ISC_LOG_DEBUG(3),
				      "continuing validation would lead to "
				      "deadlock: aborting validation");
			return (true);
		}
	}
	return (false);
}

/*%
 * Start a fetch for the requested name and type.
 */
static inline isc_result_t
create_fetch(dns_validator_t *val, dns_name_t *name, dns_rdatatype_t type,
	     isc_taskaction_t callback, const char *caller)
{
	unsigned int fopts = 0;

	disassociate_rdatasets(val);

	if (check_deadlock(val, name, type, NULL, NULL)) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "deadlock found (create_fetch)");
		return (DNS_R_NOVALIDSIG);
	}

	if ((val->options & DNS_VALIDATOR_NOCDFLAG) != 0) {
		fopts |= DNS_FETCHOPT_NOCDFLAG;
	}

	if ((val->options & DNS_VALIDATOR_NONTA) != 0) {
		fopts |= DNS_FETCHOPT_NONTA;
	}

	validator_logcreate(val, name, type, caller, "fetch");
	return (dns_resolver_createfetch(val->view->resolver, name, type,
					 NULL, NULL, NULL, NULL, 0, fopts,
					 0, NULL, val->event->ev_sender,
					 callback, val,
					 &val->frdataset,
					 &val->fsigrdataset,
					 &val->fetch));
}

/*%
 * Start a subvalidation process.
 */
static inline isc_result_t
create_validator(dns_validator_t *val, dns_name_t *name, dns_rdatatype_t type,
		 dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset,
		 isc_taskaction_t action, const char *caller)
{
	isc_result_t result;
	unsigned int vopts = 0;

	if (check_deadlock(val, name, type, rdataset, sigrdataset)) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "deadlock found (create_validator)");
		return (DNS_R_NOVALIDSIG);
	}

	/* OK to clear other options, but preserve NOCDFLAG and NONTA. */
	vopts |= (val->options & (DNS_VALIDATOR_NOCDFLAG |
				  DNS_VALIDATOR_NONTA));

	validator_logcreate(val, name, type, caller, "validator");
	result = dns_validator_create(val->view, name, type,
				      rdataset, sigrdataset, NULL, vopts,
				      val->task, action, val,
				      &val->subvalidator);
	if (result == ISC_R_SUCCESS) {
		val->subvalidator->parent = val;
		val->subvalidator->depth = val->depth + 1;
	}
	return (result);
}

/*%
 * Try to find a key that could have signed 'siginfo' among those
 * in 'rdataset'.  If found, build a dst_key_t for it and point
 * val->key at it.
 *
 * If val->key is non-NULL, this returns the next matching key.
 */
static isc_result_t
get_dst_key(dns_validator_t *val, dns_rdata_rrsig_t *siginfo,
	    dns_rdataset_t *rdataset)
{
	isc_result_t result;
	isc_buffer_t b;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dst_key_t *oldkey = val->key;
	bool foundold;

	if (oldkey == NULL) {
		foundold = true;
	} else {
		foundold = false;
		val->key = NULL;
	}

	result = dns_rdataset_first(rdataset);
	if (result != ISC_R_SUCCESS) {
		goto failure;
	}
	do {
		dns_rdataset_current(rdataset, &rdata);

		isc_buffer_init(&b, rdata.data, rdata.length);
		isc_buffer_add(&b, rdata.length);
		INSIST(val->key == NULL);
		result = dst_key_fromdns(&siginfo->signer, rdata.rdclass, &b,
					 val->view->mctx, &val->key);
		if (result != ISC_R_SUCCESS) {
			goto failure;
		}
		if (siginfo->algorithm ==
		    (dns_secalg_t)dst_key_alg(val->key) &&
		    siginfo->keyid ==
		    (dns_keytag_t)dst_key_id(val->key) &&
		    dst_key_iszonekey(val->key))
		{
			if (foundold) {
				/*
				 * This is the key we're looking for.
				 */
				return (ISC_R_SUCCESS);
			} else if (dst_key_compare(oldkey,
						   val->key) == true) {
				foundold = true;
				dst_key_free(&oldkey);
			}
		}
		dst_key_free(&val->key);
		dns_rdata_reset(&rdata);
		result = dns_rdataset_next(rdataset);
	} while (result == ISC_R_SUCCESS);

	if (result == ISC_R_NOMORE) {
		result = ISC_R_NOTFOUND;
	}

 failure:
	if (oldkey != NULL) {
		dst_key_free(&oldkey);
	}

	return (result);
}

/*%
 * Get the key that generated this signature.
 */
static isc_result_t
get_key(dns_validator_t *val, dns_rdata_rrsig_t *siginfo) {
	isc_result_t result;
	unsigned int nlabels;
	int order;
	dns_namereln_t namereln;

	/*
	 * Is the signer name appropriate for this signature?
	 *
	 * The signer name must be at the same level as the owner name
	 * or closer to the DNS root.
	 */
	namereln = dns_name_fullcompare(val->event->name, &siginfo->signer,
					&order, &nlabels);
	if (namereln != dns_namereln_subdomain &&
	    namereln != dns_namereln_equal)
	{
		return (DNS_R_CONTINUE);
	}

	if (namereln == dns_namereln_equal) {
		/*
		 * If this is a self-signed keyset, it must not be a zone key
		 * (since get_key is not called from validatezonekey).
		 */
		if (val->event->rdataset->type == dns_rdatatype_dnskey) {
			return (DNS_R_CONTINUE);
		}

		/*
		 * Records appearing in the parent zone at delegation
		 * points cannot be self-signed.
		 */
		if (dns_rdatatype_atparent(val->event->rdataset->type)) {
			return (DNS_R_CONTINUE);
		}
	} else {
		/*
		 * SOA and NS RRsets can only be signed by a key with
		 * the same name.
		 */
		if (val->event->rdataset->type == dns_rdatatype_soa ||
		    val->event->rdataset->type == dns_rdatatype_ns)
		{
			const char *type;

			if (val->event->rdataset->type == dns_rdatatype_soa) {
				type = "SOA";
			} else {
				type = "NS";
			}
			validator_log(val, ISC_LOG_DEBUG(3),
				      "%s signer mismatch", type);
			return (DNS_R_CONTINUE);
		}
	}

	/*
	 * Do we know about this key?
	 */
	result = view_find(val, &siginfo->signer, dns_rdatatype_dnskey);
	if (result == ISC_R_SUCCESS) {
		/*
		 * We have an rrset for the given keyname.
		 */
		val->keyset = &val->frdataset;
		if ((DNS_TRUST_PENDING(val->frdataset.trust) ||
		     DNS_TRUST_ANSWER(val->frdataset.trust)) &&
		    dns_rdataset_isassociated(&val->fsigrdataset))
		{
			/*
			 * We know the key but haven't validated it yet or
			 * we have a key of trust answer but a DS
			 * record for the zone may have been added.
			 */
			result = create_validator(val, &siginfo->signer,
						  dns_rdatatype_dnskey,
						  &val->frdataset,
						  &val->fsigrdataset,
						  keyvalidated,
						  "get_key");
			if (result != ISC_R_SUCCESS) {
				return (result);
			}
			return (DNS_R_WAIT);
		} else if (DNS_TRUST_PENDING(val->frdataset.trust)) {
			/*
			 * Having a pending key with no signature means that
			 * something is broken.
			 */
			result = DNS_R_CONTINUE;
		} else if (val->frdataset.trust < dns_trust_secure) {
			/*
			 * The key is legitimately insecure.  There's no
			 * point in even attempting verification.
			 */
			val->key = NULL;
			result = ISC_R_SUCCESS;
		} else {
			/*
			 * See if we've got the key used in the signature.
			 */
			validator_log(val, ISC_LOG_DEBUG(3),
				      "keyset with trust %s",
				      dns_trust_totext(val->frdataset.trust));
			result = get_dst_key(val, siginfo, val->keyset);
			if (result != ISC_R_SUCCESS) {
				/*
				 * Either the key we're looking for is not
				 * in the rrset, or something bad happened.
				 * Give up.
				 */
				result = DNS_R_CONTINUE;
			}
		}
	} else if (result == ISC_R_NOTFOUND) {
		/*
		 * We don't know anything about this key.
		 */
		result = create_fetch(val, &siginfo->signer,
				      dns_rdatatype_dnskey,
				      fetch_callback_validator, "get_key");
		if (result != ISC_R_SUCCESS) {
			return (result);
		}
		return (DNS_R_WAIT);
	} else if (result ==  DNS_R_NCACHENXDOMAIN ||
		   result == DNS_R_NCACHENXRRSET ||
		   result == DNS_R_EMPTYNAME ||
		   result == DNS_R_NXDOMAIN ||
		   result == DNS_R_NXRRSET)
	{
		/*
		 * This key doesn't exist.
		 */
		result = DNS_R_CONTINUE;
	} else if (result == DNS_R_BROKENCHAIN) {
		return (result);
	}

	if (dns_rdataset_isassociated(&val->frdataset) &&
	    val->keyset != &val->frdataset)
	{
		dns_rdataset_disassociate(&val->frdataset);
	}
	if (dns_rdataset_isassociated(&val->fsigrdataset)) {
		dns_rdataset_disassociate(&val->fsigrdataset);
	}

	return (result);
}

static dns_keytag_t
compute_keytag(dns_rdata_t *rdata) {
	isc_region_t r;

	dns_rdata_toregion(rdata, &r);
	return (dst_region_computeid(&r));
}

/*%
 * Is this keyset self-signed?
 */
static bool
isselfsigned(dns_validator_t *val) {
	dns_rdataset_t *rdataset, *sigrdataset;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dns_rdata_t sigrdata = DNS_RDATA_INIT;
	dns_rdata_dnskey_t key;
	dns_rdata_rrsig_t sig;
	dns_keytag_t keytag;
	dns_name_t *name;
	isc_result_t result;
	dst_key_t *dstkey;
	isc_mem_t *mctx;
	bool answer = false;

	rdataset = val->event->rdataset;
	sigrdataset = val->event->sigrdataset;
	name = val->event->name;
	mctx = val->view->mctx;

	if (rdataset->type == dns_rdatatype_cname ||
	    rdataset->type == dns_rdatatype_dname)
	{
		return (answer);
	}

	INSIST(rdataset->type == dns_rdatatype_dnskey);

	for (result = dns_rdataset_first(rdataset);
	     result == ISC_R_SUCCESS;
	     result = dns_rdataset_next(rdataset))
	{
		dns_rdata_reset(&rdata);
		dns_rdataset_current(rdataset, &rdata);
		result = dns_rdata_tostruct(&rdata, &key, NULL);
		RUNTIME_CHECK(result == ISC_R_SUCCESS);
		keytag = compute_keytag(&rdata);
		for (result = dns_rdataset_first(sigrdataset);
		     result == ISC_R_SUCCESS;
		     result = dns_rdataset_next(sigrdataset))
		{
			dns_rdata_reset(&sigrdata);
			dns_rdataset_current(sigrdataset, &sigrdata);
			result = dns_rdata_tostruct(&sigrdata, &sig, NULL);
			RUNTIME_CHECK(result == ISC_R_SUCCESS);

			if (sig.algorithm != key.algorithm ||
			    sig.keyid != keytag ||
			    !dns_name_equal(name, &sig.signer))
			{
				continue;
			}

			dstkey = NULL;
			result = dns_dnssec_keyfromrdata(name, &rdata, mctx,
							 &dstkey);
			if (result != ISC_R_SUCCESS) {
				continue;
			}

			result = dns_dnssec_verify(name, rdataset, dstkey,
						   true,
						   val->view->maxbits,
						   mctx, &sigrdata, NULL);
			dst_key_free(&dstkey);
			if (result != ISC_R_SUCCESS) {
				continue;
			}
			if ((key.flags & DNS_KEYFLAG_REVOKE) == 0) {
				answer = true;
				continue;
			}
			dns_view_untrust(val->view, name, &key, mctx);
		}
	}
	return (answer);
}

/*%
 * Attempt to verify the rdataset using the given key and rdata (RRSIG).
 * The signature was good and from a wildcard record and the QNAME does
 * not match the wildcard we need to look for a NOQNAME proof.
 *
 * Returns:
 * \li	ISC_R_SUCCESS if the verification succeeds.
 * \li	Others if the verification fails.
 */
static isc_result_t
verify(dns_validator_t *val, dst_key_t *key, dns_rdata_t *rdata,
       uint16_t keyid)
{
	isc_result_t result;
	dns_fixedname_t fixed;
	bool ignore = false;
	dns_name_t *wild;

	val->attributes |= VALATTR_TRIEDVERIFY;
	wild = dns_fixedname_initname(&fixed);
 again:
	result = dns_dnssec_verify(val->event->name, val->event->rdataset,
				   key, ignore, val->view->maxbits,
				   val->view->mctx, rdata, wild);
	if ((result == DNS_R_SIGEXPIRED || result == DNS_R_SIGFUTURE) &&
	    val->view->acceptexpired)
	{
		ignore = true;
		goto again;
	}

	if (ignore &&
	    (result == ISC_R_SUCCESS || result == DNS_R_FROMWILDCARD))
	{
		validator_log(val, ISC_LOG_INFO,
			      "accepted expired %sRRSIG (keyid=%u)",
			      (result == DNS_R_FROMWILDCARD) ?
			      "wildcard " : "", keyid);
	} else if (result == DNS_R_SIGEXPIRED || result == DNS_R_SIGFUTURE) {
		validator_log(val, ISC_LOG_INFO,
			      "verify failed due to bad signature (keyid=%u): "
			      "%s", keyid, isc_result_totext(result));
	} else {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "verify rdataset (keyid=%u): %s",
			      keyid, isc_result_totext(result));
	}
	if (result == DNS_R_FROMWILDCARD) {
		if (!dns_name_equal(val->event->name, wild)) {
			dns_name_t *closest;
			unsigned int labels;

			/*
			 * Compute the closest encloser in case we need it
			 * for the NSEC3 NOQNAME proof.
			 */
			closest = dns_fixedname_name(&val->closest);
			dns_name_copynf(wild, closest);
			labels = dns_name_countlabels(closest) - 1;
			dns_name_getlabelsequence(closest, 1, labels, closest);
			val->attributes |= VALATTR_NEEDNOQNAME;
		}
		result = ISC_R_SUCCESS;
	}
	return (result);
}

/*%
 * Attempts positive response validation of a normal RRset.
 *
 * Returns:
 * \li	ISC_R_SUCCESS	Validation completed successfully
 * \li	DNS_R_WAIT	Validation has started but is waiting
 *			for an event.
 * \li	Other return codes are possible and all indicate failure.
 */
static isc_result_t
validate_answer(dns_validator_t *val, bool resume) {
	isc_result_t result, vresult = DNS_R_NOVALIDSIG;
	dns_validatorevent_t *event;
	dns_rdata_t rdata = DNS_RDATA_INIT;

	/*
	 * Caller must be holding the validator lock.
	 */

	event = val->event;

	if (resume) {
		/*
		 * We already have a sigrdataset.
		 */
		result = ISC_R_SUCCESS;
		validator_log(val, ISC_LOG_DEBUG(3), "resuming validate");
	} else {
		result = dns_rdataset_first(event->sigrdataset);
	}

	for (;
	     result == ISC_R_SUCCESS;
	     result = dns_rdataset_next(event->sigrdataset))
	{
		dns_rdata_reset(&rdata);
		dns_rdataset_current(event->sigrdataset, &rdata);
		if (val->siginfo == NULL) {
			val->siginfo = isc_mem_get(val->view->mctx,
						   sizeof(*val->siginfo));
		}
		result = dns_rdata_tostruct(&rdata, val->siginfo, NULL);
		if (result != ISC_R_SUCCESS) {
			return (result);
		}

		/*
		 * At this point we could check that the signature algorithm
		 * was known and "sufficiently good".
		 */
		if (!dns_resolver_algorithm_supported(val->view->resolver,
						      event->name,
						      val->siginfo->algorithm))
		{
			resume = false;
			continue;
		}

		if (!resume) {
			result = get_key(val, val->siginfo);
			if (result == DNS_R_CONTINUE) {
				continue; /* Try the next SIG RR. */
			}
			if (result != ISC_R_SUCCESS) {
				return (result);
			}
		}

		/*
		 * There isn't a secure DNSKEY for this signature so move
		 * onto the next RRSIG.
		 */
		if (val->key == NULL) {
			resume = false;
			continue;
		}

		do {
			vresult = verify(val, val->key, &rdata,
					 val->siginfo->keyid);
			if (vresult == ISC_R_SUCCESS) {
				break;
			}
			if (val->keynode != NULL) {
				dns_keynode_t *nextnode = NULL;
				result = dns_keytable_findnextkeynode(
					val->keytable,
					val->keynode,
					&nextnode);
				dns_keytable_detachkeynode(val->keytable,
							   &val->keynode);
				val->keynode = nextnode;
				if (result != ISC_R_SUCCESS) {
					val->key = NULL;
					break;
				}
				val->key = dns_keynode_key(val->keynode);
				if (val->key == NULL) {
					break;
				}
			} else {
				if (get_dst_key(val, val->siginfo, val->keyset)
				    != ISC_R_SUCCESS)
				{
					break;
				}
			}
		} while (1);
		if (vresult != ISC_R_SUCCESS) {
			validator_log(val, ISC_LOG_DEBUG(3),
				      "failed to verify rdataset");
		} else {
			dns_rdataset_trimttl(event->rdataset,
					     event->sigrdataset,
					     val->siginfo, val->start,
					     val->view->acceptexpired);
		}

		if (val->keynode != NULL) {
			dns_keytable_detachkeynode(val->keytable,
						   &val->keynode);
		} else {
			if (val->key != NULL) {
				dst_key_free(&val->key);
			}
			if (val->keyset != NULL) {
				dns_rdataset_disassociate(val->keyset);
				val->keyset = NULL;
			}
		}
		val->key = NULL;
		if (NEEDNOQNAME(val)) {
			if (val->event->message == NULL) {
				validator_log(val, ISC_LOG_DEBUG(3),
					      "no message available "
					      "for noqname proof");
				return (DNS_R_NOVALIDSIG);
			}
			validator_log(val, ISC_LOG_DEBUG(3),
				      "looking for noqname proof");
			return (validate_nx(val, false));
		} else if (vresult == ISC_R_SUCCESS) {
			marksecure(event);
			validator_log(val, ISC_LOG_DEBUG(3),
				      "marking as secure, "
				      "noqname proof not needed");
			return (ISC_R_SUCCESS);
		} else {
			validator_log(val, ISC_LOG_DEBUG(3),
				      "verify failure: %s",
				      isc_result_totext(result));
			resume = false;
		}
	}
	if (result != ISC_R_NOMORE) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "failed to iterate signatures: %s",
			      isc_result_totext(result));
		return (result);
	}

	validator_log(val, ISC_LOG_INFO, "no valid signature found");
	return (vresult);
}

/*%
 * Check whether this DNSKEY (keyrdata) signed the DNSKEY RRset
 * (val->event->rdataset).
 */
static isc_result_t
checkkey(dns_validator_t *val, dns_rdata_t *keyrdata, uint16_t keyid,
	 dns_secalg_t algorithm)
{
	dns_rdata_rrsig_t sig;
	dst_key_t *dstkey = NULL;
	isc_result_t result;

	for (result = dns_rdataset_first(val->event->sigrdataset);
	     result == ISC_R_SUCCESS;
	     result = dns_rdataset_next(val->event->sigrdataset))
	{
		dns_rdata_t rdata = DNS_RDATA_INIT;

		dns_rdataset_current(val->event->sigrdataset, &rdata);
		result = dns_rdata_tostruct(&rdata, &sig, NULL);
		RUNTIME_CHECK(result == ISC_R_SUCCESS);
		if (keyid != sig.keyid || algorithm != sig.algorithm) {
			continue;
		}
		if (dstkey == NULL) {
			result = dns_dnssec_keyfromrdata(val->event->name,
							 keyrdata,
							 val->view->mctx,
							 &dstkey);
			if (result != ISC_R_SUCCESS) {
				/*
				 * This really shouldn't happen, but...
				 */
				continue;
			}
		}
		result = verify(val, dstkey, &rdata, sig.keyid);
		if (result == ISC_R_SUCCESS) {
			break;
		}
	}
	if (dstkey != NULL) {
		dst_key_free(&dstkey);
	}
	return (result);
}

/*%
 * Find the DNSKEY that corresponds to the DS.
 */
static isc_result_t
keyfromds(dns_validator_t *val, dns_rdataset_t *rdataset, dns_rdata_t *dsrdata,
	  dns_dsdigest_t digest, uint16_t keyid, dns_secalg_t algorithm,
	  dns_rdata_t *keyrdata)
{
	dns_keytag_t keytag;
	dns_rdata_dnskey_t key;
	isc_result_t result;
	unsigned char dsbuf[DNS_DS_BUFFERSIZE];

	for (result = dns_rdataset_first(rdataset);
	     result == ISC_R_SUCCESS;
	     result = dns_rdataset_next(rdataset))
	{
		dns_rdata_t newdsrdata = DNS_RDATA_INIT;

		dns_rdata_reset(keyrdata);
		dns_rdataset_current(rdataset, keyrdata);
		result = dns_rdata_tostruct(keyrdata, &key, NULL);
		RUNTIME_CHECK(result == ISC_R_SUCCESS);
		keytag = compute_keytag(keyrdata);
		if (keyid != keytag || algorithm != key.algorithm) {
			continue;
		}
		dns_rdata_reset(&newdsrdata);
		result = dns_ds_buildrdata(val->event->name, keyrdata, digest,
					   dsbuf, &newdsrdata);
		if (result != ISC_R_SUCCESS) {
			validator_log(val, ISC_LOG_DEBUG(3),
				      "dns_ds_buildrdata() -> %s",
				      dns_result_totext(result));
			continue;
		}
		if (dns_rdata_compare(dsrdata, &newdsrdata) == 0) {
			break;
		}
	}
	return (result);
}

/*%
 * Attempts positive response validation of an RRset containing zone keys
 * (i.e. a DNSKEY rrset).
 *
 * Returns:
 * \li	ISC_R_SUCCESS	Validation completed successfully
 * \li	DNS_R_WAIT	Validation has started but is waiting
 *			for an event.
 * \li	Other return codes are possible and all indicate failure.
 */
static isc_result_t
validatezonekey(dns_validator_t *val) {
	isc_result_t result;
	dns_validatorevent_t *event;
	dns_rdataset_t trdataset;
	dns_rdata_t dsrdata = DNS_RDATA_INIT;
	dns_rdata_t keyrdata = DNS_RDATA_INIT;
	dns_rdata_t sigrdata = DNS_RDATA_INIT;
	char namebuf[DNS_NAME_FORMATSIZE];
	dns_rdata_ds_t ds;
	dns_rdata_rrsig_t sig;
	dst_key_t *dstkey;
	bool supported_algorithm;
	bool atsep = false;
	char digest_types[256];

	/*
	 * Caller must be holding the validator lock.
	 */

	event = val->event;

	if (val->dsset == NULL) {
		/*
		 * First, see if this key was signed by a trusted key.
		 */
		for (result = dns_rdataset_first(val->event->sigrdataset);
		     result == ISC_R_SUCCESS;
		     result = dns_rdataset_next(val->event->sigrdataset))
		{
			dns_keynode_t *keynode = NULL;
			dns_fixedname_t fixed;
			dns_name_t *found;

			found = dns_fixedname_initname(&fixed);
			dns_rdata_reset(&sigrdata);
			dns_rdataset_current(val->event->sigrdataset,
					     &sigrdata);
			result = dns_rdata_tostruct(&sigrdata, &sig, NULL);
			RUNTIME_CHECK(result == ISC_R_SUCCESS);

			if (!dns_name_equal(val->event->name, &sig.signer)) {
				continue;
			}

			result = dns_keytable_findkeynode(val->keytable,
							  val->event->name,
							  sig.algorithm,
							  sig.keyid, &keynode);
			if (result == ISC_R_NOTFOUND &&
			    dns_keytable_finddeepestmatch(val->keytable,
							  val->event->name,
							  found)
							!= ISC_R_SUCCESS)
			{
				validator_log(val, ISC_LOG_DEBUG(3),
					      "not beneath secure root");
				return (markanswer(val, "validatezonekey (1)",
						   "not beneath secure root"));
			}
			if (result == DNS_R_PARTIALMATCH ||
			    result == ISC_R_SUCCESS)
			{
				atsep = true;
			}
			while (result == ISC_R_SUCCESS) {
				dns_keynode_t *nextnode = NULL;
				dstkey = dns_keynode_key(keynode);
				if (dstkey == NULL) {
					dns_keytable_detachkeynode(
						val->keytable,
						&keynode);
					break;
				}
				result = verify(val, dstkey, &sigrdata,
						sig.keyid);
				if (result == ISC_R_SUCCESS) {
					dns_keytable_detachkeynode(
								val->keytable,
								&keynode);
					break;
				}
				result = dns_keytable_findnextkeynode(
								val->keytable,
								keynode,
								&nextnode);
				dns_keytable_detachkeynode(val->keytable,
							   &keynode);
				keynode = nextnode;
			}
			if (result == ISC_R_SUCCESS) {
				marksecure(event);
				validator_log(val, ISC_LOG_DEBUG(3),
					      "signed by trusted key; "
					      "marking as secure");
				return (result);
			}
		}

		if (atsep) {
			/*
			 * We have not found a key to verify this DNSKEY
			 * RRset.  As this is a SEP we have to assume that
			 * the RRset is invalid.
			 */
			dns_name_format(val->event->name, namebuf,
					sizeof(namebuf));
			validator_log(val, ISC_LOG_NOTICE,
				      "unable to find a DNSKEY which verifies "
				      "the DNSKEY RRset and also matches a "
				      "trusted key for '%s'", namebuf);
			return (DNS_R_NOVALIDKEY);
		}

		/*
		 * If this is the root name and there was no trusted key,
		 * give up, since there's no DS at the root.
		 */
		if (dns_name_equal(event->name, dns_rootname)) {
			if ((val->attributes & VALATTR_TRIEDVERIFY) != 0) {
				validator_log(val, ISC_LOG_DEBUG(3),
					      "root key failed to validate");
				return (DNS_R_NOVALIDSIG);
			} else {
				validator_log(val, ISC_LOG_DEBUG(3),
					      "no trusted root key");
				return (DNS_R_NOVALIDDS);
			}
		}

		/*
		 * Otherwise, try to find the DS record.
		 */
		result = view_find(val, val->event->name, dns_rdatatype_ds);
		if (result == ISC_R_SUCCESS) {
			/*
			 * We have DS records.
			 */
			val->dsset = &val->frdataset;
			if ((DNS_TRUST_PENDING(val->frdataset.trust) ||
			     DNS_TRUST_ANSWER(val->frdataset.trust)) &&
			    dns_rdataset_isassociated(&val->fsigrdataset))
			{
				result = create_validator(val,
							  val->event->name,
							  dns_rdatatype_ds,
							  &val->frdataset,
							  &val->fsigrdataset,
							  dsvalidated,
							  "validatezonekey");
				if (result != ISC_R_SUCCESS) {
					return (result);
				}
				return (DNS_R_WAIT);
			} else if (DNS_TRUST_PENDING(val->frdataset.trust)) {
				/*
				 * There should never be an unsigned DS.
				 */
				dns_rdataset_disassociate(&val->frdataset);
				validator_log(val, ISC_LOG_DEBUG(2),
					      "unsigned DS record");
				return (DNS_R_NOVALIDSIG);
			} else {
				result = ISC_R_SUCCESS;
				POST(result);
			}
		} else if (result == ISC_R_NOTFOUND) {
			/*
			 * We don't have the DS.  Find it.
			 */
			result = create_fetch(val, val->event->name,
					      dns_rdatatype_ds, dsfetched,
					      "validatezonekey");
			if (result != ISC_R_SUCCESS) {
				return (result);
			}
			return (DNS_R_WAIT);
		} else if (result == DNS_R_NCACHENXDOMAIN ||
			   result == DNS_R_NCACHENXRRSET ||
			   result == DNS_R_EMPTYNAME ||
			   result == DNS_R_NXDOMAIN ||
			   result == DNS_R_NXRRSET ||
			   result == DNS_R_CNAME)
		{
			/*
			 * The DS does not exist.
			 */
			disassociate_rdatasets(val);
			validator_log(val, ISC_LOG_DEBUG(2), "no DS record");
			return (DNS_R_NOVALIDSIG);
		} else if (result == DNS_R_BROKENCHAIN) {
			return (result);
		}
	}

	/*
	 * We have a DS set.
	 */
	INSIST(val->dsset != NULL);

	if (val->dsset->trust < dns_trust_secure) {
		return (markanswer(val, "validatezonekey (2)", "insecure DS"));
	}

	/*
	 * Look through the DS record and find the keys that can sign the
	 * key set and the matching signature.  For each such key, attempt
	 * verification.
	 */

	supported_algorithm = false;

	/*
	 * If DNS_DSDIGEST_SHA256 or DNS_DSDIGEST_SHA384 is present we
	 * are required to prefer it over DNS_DSDIGEST_SHA1.  This in
	 * practice means that we need to ignore DNS_DSDIGEST_SHA1 if a
	 * DNS_DSDIGEST_SHA256 or DNS_DSDIGEST_SHA384 is present.
	 */
	memset(digest_types, 1, sizeof(digest_types));
	for (result = dns_rdataset_first(val->dsset);
	     result == ISC_R_SUCCESS;
	     result = dns_rdataset_next(val->dsset))
	{
		dns_rdata_reset(&dsrdata);
		dns_rdataset_current(val->dsset, &dsrdata);
		result = dns_rdata_tostruct(&dsrdata, &ds, NULL);
		RUNTIME_CHECK(result == ISC_R_SUCCESS);

		if (!dns_resolver_ds_digest_supported(val->view->resolver,
						      val->event->name,
						      ds.digest_type))
		{
			continue;
		}

		if (!dns_resolver_algorithm_supported(val->view->resolver,
						      val->event->name,
						      ds.algorithm))
		{
			continue;
		}

		if ((ds.digest_type == DNS_DSDIGEST_SHA256 &&
		     ds.length == ISC_SHA256_DIGESTLENGTH) ||
		    (ds.digest_type == DNS_DSDIGEST_SHA384 &&
		     ds.length == ISC_SHA384_DIGESTLENGTH))
		{
			digest_types[DNS_DSDIGEST_SHA1] = 0;
			break;
		}
	}

	for (result = dns_rdataset_first(val->dsset);
	     result == ISC_R_SUCCESS;
	     result = dns_rdataset_next(val->dsset))
	{
		dns_rdata_reset(&dsrdata);
		dns_rdataset_current(val->dsset, &dsrdata);
		result = dns_rdata_tostruct(&dsrdata, &ds, NULL);
		RUNTIME_CHECK(result == ISC_R_SUCCESS);

		if (digest_types[ds.digest_type] == 0) {
			continue;
		}

		if (!dns_resolver_ds_digest_supported(val->view->resolver,
						      val->event->name,
						      ds.digest_type))
		{
			continue;
		}

		if (!dns_resolver_algorithm_supported(val->view->resolver,
						      val->event->name,
						      ds.algorithm))
		{
			continue;
		}

		supported_algorithm = true;

		dns_rdataset_init(&trdataset);
		dns_rdataset_clone(val->event->rdataset, &trdataset);

		/*
		 * Find matching DNSKEY from DS.
		 */
		result = keyfromds(val, &trdataset, &dsrdata, ds.digest_type,
				   ds.key_tag, ds.algorithm, &keyrdata);
		if (result != ISC_R_SUCCESS) {
			dns_rdataset_disassociate(&trdataset);
			validator_log(val, ISC_LOG_DEBUG(3),
				      "no DNSKEY matching DS");
			continue;
		}

		/*
		 * Check that this DNSKEY signed the DNSKEY rrset.
		 */
		result = checkkey(val, &keyrdata, ds.key_tag, ds.algorithm);

		dns_rdataset_disassociate(&trdataset);
		if (result == ISC_R_SUCCESS) {
			break;
		}
		validator_log(val, ISC_LOG_DEBUG(3),
			      "no RRSIG matching DS key");
	}
	if (result == ISC_R_SUCCESS) {
		marksecure(event);
		validator_log(val, ISC_LOG_DEBUG(3), "marking as secure (DS)");
		return (result);
	} else if (result == ISC_R_NOMORE && !supported_algorithm) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "no supported algorithm/digest (DS)");
		return (markanswer(val, "validatezonekey (3)",
				   "no supported algorithm/digest (DS)"));
	} else {
		validator_log(val, ISC_LOG_INFO,
			      "no valid signature found (DS)");
		return (DNS_R_NOVALIDSIG);
	}
}

/*%
 * Starts a positive response validation.
 *
 * Returns:
 * \li	ISC_R_SUCCESS	Validation completed successfully
 * \li	DNS_R_WAIT	Validation has started but is waiting
 *			for an event.
 * \li	Other return codes are possible and all indicate failure.
 */
static isc_result_t
start_positive_validation(dns_validator_t *val) {
	/*
	 * If this is not a key, go straight into validate_answer().
	 */
	if (val->event->type != dns_rdatatype_dnskey || !isselfsigned(val)) {
		return (validate_answer(val, false));
	}

	return (validatezonekey(val));
}

/*%
 * val_rdataset_first and val_rdataset_next provide iteration methods
 * that hide whether we are iterating across a message or a negative
 * cache rdataset.
 */
static isc_result_t
val_rdataset_first(dns_validator_t *val, dns_name_t **namep,
		   dns_rdataset_t **rdatasetp)
{
	dns_message_t *message = val->event->message;
	isc_result_t result;

	REQUIRE(rdatasetp != NULL);
	REQUIRE(namep != NULL);
	if (message == NULL) {
		REQUIRE(*rdatasetp != NULL);
		REQUIRE(*namep != NULL);
	} else {
		REQUIRE(*rdatasetp == NULL);
		REQUIRE(*namep == NULL);
	}

	if (message != NULL) {
		result = dns_message_firstname(message, DNS_SECTION_AUTHORITY);
		if (result != ISC_R_SUCCESS) {
			return (result);
		}
		dns_message_currentname(message, DNS_SECTION_AUTHORITY, namep);
		*rdatasetp = ISC_LIST_HEAD((*namep)->list);
		INSIST(*rdatasetp != NULL);
	} else {
		result = dns_rdataset_first(val->event->rdataset);
		if (result == ISC_R_SUCCESS) {
			dns_ncache_current(val->event->rdataset, *namep,
					   *rdatasetp);
		}
	}
	return (result);
}

static isc_result_t
val_rdataset_next(dns_validator_t *val, dns_name_t **namep,
		  dns_rdataset_t **rdatasetp)
{
	dns_message_t *message = val->event->message;
	isc_result_t result = ISC_R_SUCCESS;

	REQUIRE(rdatasetp != NULL && *rdatasetp != NULL);
	REQUIRE(namep != NULL && *namep != NULL);

	if (message != NULL) {
		dns_rdataset_t *rdataset = *rdatasetp;
		rdataset = ISC_LIST_NEXT(rdataset, link);
		if (rdataset == NULL) {
			*namep = NULL;
			result = dns_message_nextname(message,
						      DNS_SECTION_AUTHORITY);
			if (result == ISC_R_SUCCESS) {
				dns_message_currentname(message,
							DNS_SECTION_AUTHORITY,
							namep);
				rdataset = ISC_LIST_HEAD((*namep)->list);
				INSIST(rdataset != NULL);
			}
		}
		*rdatasetp = rdataset;
	} else {
		dns_rdataset_disassociate(*rdatasetp);
		result = dns_rdataset_next(val->event->rdataset);
		if (result == ISC_R_SUCCESS) {
			dns_ncache_current(val->event->rdataset, *namep,
					   *rdatasetp);
		}
	}
	return (result);
}

/*%
 * Look for NODATA at the wildcard and NOWILDCARD proofs in the
 * previously validated NSEC records.  As these proofs are mutually
 * exclusive we stop when one is found.
 *
 * Returns
 * \li	ISC_R_SUCCESS
 */
static isc_result_t
checkwildcard(dns_validator_t *val, dns_rdatatype_t type,
	      dns_name_t *zonename)
{
	dns_name_t *name, *wild, tname;
	isc_result_t result;
	bool exists, data;
	char namebuf[DNS_NAME_FORMATSIZE];
	dns_rdataset_t *rdataset, trdataset;

	dns_name_init(&tname, NULL);
	dns_rdataset_init(&trdataset);
	wild = dns_fixedname_name(&val->wild);

	if (dns_name_countlabels(wild) == 0) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "in checkwildcard: no wildcard to check");
		return (ISC_R_SUCCESS);
	}

	dns_name_format(wild, namebuf, sizeof(namebuf));
	validator_log(val, ISC_LOG_DEBUG(3), "in checkwildcard: %s", namebuf);

	if (val->event->message == NULL) {
		name = &tname;
		rdataset = &trdataset;
	} else {
		name = NULL;
		rdataset = NULL;
	}

	for (result = val_rdataset_first(val, &name, &rdataset);
	     result == ISC_R_SUCCESS;
	     result = val_rdataset_next(val, &name, &rdataset))
	{
		if (rdataset->type != type ||
		    rdataset->trust != dns_trust_secure)
		{
			continue;
		}

		if (rdataset->type == dns_rdatatype_nsec &&
		    (NEEDNODATA(val) || NEEDNOWILDCARD(val)) &&
		    !FOUNDNODATA(val) && !FOUNDNOWILDCARD(val) &&
		    dns_nsec_noexistnodata(val->event->type, wild, name,
					   rdataset, &exists, &data, NULL,
					   validator_log, val)
					== ISC_R_SUCCESS)
		{
			dns_name_t **proofs = val->event->proofs;
			if (exists && !data) {
				val->attributes |= VALATTR_FOUNDNODATA;
			}
			if (exists && !data && NEEDNODATA(val)) {
				proofs[DNS_VALIDATOR_NODATAPROOF] =
					name;
			}
			if (!exists) {
				val->attributes |=
					VALATTR_FOUNDNOWILDCARD;
			}
			if (!exists && NEEDNOQNAME(val)) {
				proofs[DNS_VALIDATOR_NOWILDCARDPROOF] = name;
			}
			if (dns_rdataset_isassociated(&trdataset)) {
				dns_rdataset_disassociate(&trdataset);
			}
			return (ISC_R_SUCCESS);
		}

		if (rdataset->type == dns_rdatatype_nsec3 &&
		    (NEEDNODATA(val) || NEEDNOWILDCARD(val)) &&
		    !FOUNDNODATA(val) && !FOUNDNOWILDCARD(val) &&
		    dns_nsec3_noexistnodata(val->event->type, wild, name,
					    rdataset, zonename, &exists, &data,
					    NULL, NULL, NULL, NULL, NULL, NULL,
					    validator_log, val)
					== ISC_R_SUCCESS)
		{
			dns_name_t **proofs = val->event->proofs;
			if (exists && !data) {
				val->attributes |= VALATTR_FOUNDNODATA;
			}
			if (exists && !data && NEEDNODATA(val)) {
				proofs[DNS_VALIDATOR_NODATAPROOF] = name;
			}
			if (!exists) {
				val->attributes |= VALATTR_FOUNDNOWILDCARD;
			}
			if (!exists && NEEDNOQNAME(val)) {
				proofs[DNS_VALIDATOR_NOWILDCARDPROOF] = name;
			}
			if (dns_rdataset_isassociated(&trdataset)) {
				dns_rdataset_disassociate(&trdataset);
			}
			return (ISC_R_SUCCESS);
		}
	}
	if (result == ISC_R_NOMORE) {
		result = ISC_R_SUCCESS;
	}
	if (dns_rdataset_isassociated(&trdataset)) {
		dns_rdataset_disassociate(&trdataset);
	}
	return (result);
}

static isc_result_t
findnsec3proofs(dns_validator_t *val) {
	dns_name_t *name, tname;
	isc_result_t result;
	bool exists, data, optout, unknown;
	bool setclosest, setnearest, *setclosestp;
	dns_fixedname_t fclosest, fnearest, fzonename;
	dns_name_t *closest, *nearest, *zonename, *closestp;
	dns_name_t **proofs = val->event->proofs;
	dns_rdataset_t *rdataset, trdataset;

	dns_name_init(&tname, NULL);
	dns_rdataset_init(&trdataset);
	closest = dns_fixedname_initname(&fclosest);
	nearest = dns_fixedname_initname(&fnearest);
	zonename = dns_fixedname_initname(&fzonename);

	if (val->event->message == NULL) {
		name = &tname;
		rdataset = &trdataset;
	} else {
		name = NULL;
		rdataset = NULL;
	}

	for (result = val_rdataset_first(val, &name, &rdataset);
	     result == ISC_R_SUCCESS;
	     result = val_rdataset_next(val, &name, &rdataset))
	{
		if (rdataset->type != dns_rdatatype_nsec3 ||
		    rdataset->trust != dns_trust_secure)
		{
			continue;
		}

		result = dns_nsec3_noexistnodata(val->event->type,
						 val->event->name, name,
						 rdataset, zonename, NULL,
						 NULL, NULL, NULL, NULL, NULL,
						 NULL, NULL, validator_log,
						 val);
		if (result != ISC_R_IGNORE && result != ISC_R_SUCCESS) {
			if (dns_rdataset_isassociated(&trdataset)) {
				dns_rdataset_disassociate(&trdataset);
			}
			return (result);
		}
	}
	if (result != ISC_R_NOMORE) {
		result = ISC_R_SUCCESS;
	}
	POST(result);

	if (dns_name_countlabels(zonename) == 0) {
		return (ISC_R_SUCCESS);
	}

	/*
	 * If the val->closest is set then we want to use it otherwise
	 * we need to discover it.
	 */
	if (dns_name_countlabels(dns_fixedname_name(&val->closest)) != 0) {
		char namebuf[DNS_NAME_FORMATSIZE];

		dns_name_format(dns_fixedname_name(&val->closest),
				namebuf, sizeof(namebuf));
		validator_log(val, ISC_LOG_DEBUG(3),
			      "closest encloser from wildcard signature '%s'",
			      namebuf);
		dns_name_copynf(dns_fixedname_name(&val->closest), closest);
		closestp = NULL;
		setclosestp = NULL;
	} else {
		closestp = closest;
		setclosestp = &setclosest;
	}

	for (result = val_rdataset_first(val, &name, &rdataset);
	     result == ISC_R_SUCCESS;
	     result = val_rdataset_next(val, &name, &rdataset))
	{
		if (rdataset->type != dns_rdatatype_nsec3 ||
		    rdataset->trust != dns_trust_secure)
		{
			continue;
		}

		/*
		 * We process all NSEC3 records to find the closest
		 * encloser and nearest name to the closest encloser.
		 */
		setclosest = setnearest = false;
		optout = false;
		unknown = false;
		result = dns_nsec3_noexistnodata(val->event->type,
						 val->event->name,
						 name, rdataset, zonename,
						 &exists, &data, &optout,
						 &unknown, setclosestp,
						 &setnearest, closestp,
						 nearest, validator_log, val);
		if (unknown) {
			val->attributes |= VALATTR_FOUNDUNKNOWN;
		}
		if (result != ISC_R_SUCCESS) {
			continue;
		}
		if (setclosest) {
			proofs[DNS_VALIDATOR_CLOSESTENCLOSER] = name;
		}
		if (exists && !data && NEEDNODATA(val)) {
			val->attributes |= VALATTR_FOUNDNODATA;
			proofs[DNS_VALIDATOR_NODATAPROOF] = name;
		}
		if (!exists && setnearest) {
			val->attributes |= VALATTR_FOUNDNOQNAME;
			proofs[DNS_VALIDATOR_NOQNAMEPROOF] = name;
			if (optout) {
				val->attributes |= VALATTR_FOUNDOPTOUT;
			}
		}
	}
	if (result == ISC_R_NOMORE) {
		result = ISC_R_SUCCESS;
	}

	/*
	 * To know we have a valid noqname and optout proofs we need to also
	 * have a valid closest encloser.  Otherwise we could still be looking
	 * at proofs from the parent zone.
	 */
	if (dns_name_countlabels(closest) > 0 &&
	    dns_name_countlabels(nearest) ==
	    dns_name_countlabels(closest) + 1 &&
	    dns_name_issubdomain(nearest, closest))
	{
		val->attributes |= VALATTR_FOUNDCLOSEST;
		result = dns_name_concatenate(dns_wildcardname, closest,
					      dns_fixedname_name(&val->wild),
					      NULL);
		RUNTIME_CHECK(result == ISC_R_SUCCESS);
	} else {
		val->attributes &= ~VALATTR_FOUNDNOQNAME;
		val->attributes &= ~VALATTR_FOUNDOPTOUT;
		proofs[DNS_VALIDATOR_NOQNAMEPROOF] = NULL;
	}

	/*
	 * Do we need to check for the wildcard?
	 */
	if (FOUNDNOQNAME(val) && FOUNDCLOSEST(val) &&
	    ((NEEDNODATA(val) && !FOUNDNODATA(val)) || NEEDNOWILDCARD(val)))
	{
		result = checkwildcard(val, dns_rdatatype_nsec3, zonename);
		if (result != ISC_R_SUCCESS) {
			return (result);
		}
	}
	return (result);
}

/*
 * Start a validator for negative response data.
 *
 * Returns:
 * \li	DNS_R_CONTINUE	Validation skipped, continue
 * \li	DNS_R_WAIT	Validation is in progress
 *
 * \li	Other return codes indicate failure.
 */
static isc_result_t
validate_neg_rrset(dns_validator_t *val, dns_name_t *name,
		   dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset)
{
	isc_result_t result;

	/*
	 * If a signed zone is missing the zone key, bad
	 * things could happen.  A query for data in the zone
	 * would lead to a query for the zone key, which
	 * would return a negative answer, which would contain
	 * an SOA and an NSEC signed by the missing key, which
	 * would trigger another query for the DNSKEY (since
	 * the first one is still in progress), and go into an
	 * infinite loop.  Avoid that.
	 */
	if (val->event->type == dns_rdatatype_dnskey &&
	    rdataset->type == dns_rdatatype_nsec &&
	    dns_name_equal(name, val->event->name))
	{
		dns_rdata_t nsec = DNS_RDATA_INIT;

		result = dns_rdataset_first(rdataset);
		if (result != ISC_R_SUCCESS) {
			return (result);
		}
		dns_rdataset_current(rdataset, &nsec);
		if (dns_nsec_typepresent(&nsec, dns_rdatatype_soa)) {
			return (DNS_R_CONTINUE);
		}
	}

	val->currentset = rdataset;
	result = create_validator(val, name, rdataset->type,
				  rdataset, sigrdataset,
				  authvalidated, "validate_neg_rrset");
	if (result != ISC_R_SUCCESS) {
		return (result);
	}

	val->authcount++;
	return (DNS_R_WAIT);
}

/*%
 * Validate the authority section records.
 */
static isc_result_t
validate_authority(dns_validator_t *val, bool resume) {
	dns_name_t *name;
	dns_message_t *message = val->event->message;
	isc_result_t result;

	if (!resume) {
		result = dns_message_firstname(message, DNS_SECTION_AUTHORITY);
	} else {
		result = ISC_R_SUCCESS;
	}

	for (;
	     result == ISC_R_SUCCESS;
	     result = dns_message_nextname(message, DNS_SECTION_AUTHORITY))
	{
		dns_rdataset_t *rdataset = NULL, *sigrdataset = NULL;

		name = NULL;
		dns_message_currentname(message, DNS_SECTION_AUTHORITY, &name);
		if (resume) {
			rdataset = ISC_LIST_NEXT(val->currentset, link);
			val->currentset = NULL;
			resume = false;
		} else {
			rdataset = ISC_LIST_HEAD(name->list);
		}

		for (;
		     rdataset != NULL;
		     rdataset = ISC_LIST_NEXT(rdataset, link))
		{
			if (rdataset->type == dns_rdatatype_rrsig) {
				continue;
			}

			for (sigrdataset = ISC_LIST_HEAD(name->list);
			     sigrdataset != NULL;
			     sigrdataset = ISC_LIST_NEXT(sigrdataset, link))
			{
				if (sigrdataset->type == dns_rdatatype_rrsig &&
				    sigrdataset->covers == rdataset->type)
				{
					break;
				}
			}

			result = validate_neg_rrset(val, name, rdataset,
						    sigrdataset);
			if (result != DNS_R_CONTINUE) {
				return (result);
			}
		}
	}
	if (result == ISC_R_NOMORE) {
		result = ISC_R_SUCCESS;
	}
	return (result);
}

/*%
 * Validate the ncache elements.
 */
static isc_result_t
validate_ncache(dns_validator_t *val, bool resume) {
	dns_name_t *name;
	isc_result_t result;

	if (!resume) {
		result = dns_rdataset_first(val->event->rdataset);
	} else {
		result = dns_rdataset_next(val->event->rdataset);
	}

	for (;
	     result == ISC_R_SUCCESS;
	     result = dns_rdataset_next(val->event->rdataset))
	{
		dns_rdataset_t *rdataset, *sigrdataset = NULL;

		disassociate_rdatasets(val);

		name = dns_fixedname_initname(&val->fname);
		rdataset = &val->frdataset;
		dns_ncache_current(val->event->rdataset, name, rdataset);

		if (val->frdataset.type == dns_rdatatype_rrsig) {
			continue;
		}

		result = dns_ncache_getsigrdataset(val->event->rdataset, name,
						   rdataset->type,
						   &val->fsigrdataset);
		if (result == ISC_R_SUCCESS) {
			sigrdataset = &val->fsigrdataset;
		}

		result = validate_neg_rrset(val, name, rdataset, sigrdataset);
		if (result == DNS_R_CONTINUE) {
			continue;
		}

		return (result);
	}
	if (result == ISC_R_NOMORE) {
		result = ISC_R_SUCCESS;
	}
	return (result);
}

/*%
 * Prove a negative answer is good or that there is a NOQNAME when the
 * answer is from a wildcard.
 *
 * Loop through the authority section looking for NODATA, NOWILDCARD
 * and NOQNAME proofs in the NSEC records by calling authvalidated().
 *
 * If the required proofs are found we are done.
 *
 * If the proofs are not found attempt to prove this is an unsecure
 * response.
 */
static isc_result_t
validate_nx(dns_validator_t *val, bool resume) {
	isc_result_t result;

	if (resume) {
		validator_log(val, ISC_LOG_DEBUG(3), "resuming validate_nx");
	}

	if (val->event->message == NULL) {
		result = validate_ncache(val, resume);
	} else {
		result = validate_authority(val, resume);
	}

	if (result != ISC_R_SUCCESS) {
		return (result);
	}

	/*
	 * Do we only need to check for NOQNAME?  To get here we must have
	 * had a secure wildcard answer.
	 */
	if (!NEEDNODATA(val) && !NEEDNOWILDCARD(val) && NEEDNOQNAME(val)) {
		if (!FOUNDNOQNAME(val)) {
			findnsec3proofs(val);
		}

		if (FOUNDNOQNAME(val) && FOUNDCLOSEST(val) &&
		    !FOUNDOPTOUT(val))
		{
			validator_log(val, ISC_LOG_DEBUG(3),
				      "marking as secure, noqname proof found");
			marksecure(val->event);
			return (ISC_R_SUCCESS);
		} else if (FOUNDOPTOUT(val) &&
		      dns_name_countlabels(dns_fixedname_name(&val->wild)) != 0)
		{
			validator_log(val, ISC_LOG_DEBUG(3),
				      "optout proof found");
			val->event->optout = true;
			markanswer(val, "validate_nx (1)", NULL);
			return (ISC_R_SUCCESS);
		} else if ((val->attributes & VALATTR_FOUNDUNKNOWN) != 0) {
			validator_log(val, ISC_LOG_DEBUG(3),
				      "unknown NSEC3 hash algorithm found");
			markanswer(val, "validate_nx (2)", NULL);
			return (ISC_R_SUCCESS);
		}

		validator_log(val, ISC_LOG_DEBUG(3),
			      "noqname proof not found");
		return (DNS_R_NOVALIDNSEC);
	}

	if (!FOUNDNOQNAME(val) && !FOUNDNODATA(val)) {
		findnsec3proofs(val);
	}

	/*
	 * Do we need to check for the wildcard?
	 */
	if (FOUNDNOQNAME(val) && FOUNDCLOSEST(val) &&
	    ((NEEDNODATA(val) && !FOUNDNODATA(val)) || NEEDNOWILDCARD(val)))
	{
		result = checkwildcard(val, dns_rdatatype_nsec, NULL);
		if (result != ISC_R_SUCCESS) {
			return (result);
		}
	}

	if ((NEEDNODATA(val) && (FOUNDNODATA(val) || FOUNDOPTOUT(val))) ||
	    (NEEDNOQNAME(val) && FOUNDNOQNAME(val) &&
	     NEEDNOWILDCARD(val) && FOUNDNOWILDCARD(val) &&
	     FOUNDCLOSEST(val)))
	{
		if ((val->attributes & VALATTR_FOUNDOPTOUT) != 0) {
			val->event->optout = true;
		}
		validator_log(val, ISC_LOG_DEBUG(3),
			      "nonexistence proof(s) found");
		if (val->event->message == NULL) {
			marksecure(val->event);
		} else {
			val->event->secure = true;
		}
		return (ISC_R_SUCCESS);
	}

	if (val->authfail != 0 && val->authcount == val->authfail) {
		return (DNS_R_BROKENCHAIN);
	}

	validator_log(val, ISC_LOG_DEBUG(3),
		      "nonexistence proof(s) not found");
	return (proveunsecure(val, false, false));
}

static bool
check_ds(dns_validator_t *val, dns_name_t *name, dns_rdataset_t *rdataset) {
	dns_rdata_t dsrdata = DNS_RDATA_INIT;
	dns_rdata_ds_t ds;
	isc_result_t result;

	for (result = dns_rdataset_first(rdataset);
	     result == ISC_R_SUCCESS;
	     result = dns_rdataset_next(rdataset))
	{
		dns_rdataset_current(rdataset, &dsrdata);
		result = dns_rdata_tostruct(&dsrdata, &ds, NULL);
		RUNTIME_CHECK(result == ISC_R_SUCCESS);

		if (dns_resolver_ds_digest_supported(val->view->resolver,
						     name, ds.digest_type) &&
		    dns_resolver_algorithm_supported(val->view->resolver,
						     name, ds.algorithm))
		{
			dns_rdata_reset(&dsrdata);
			return (true);
		}
		dns_rdata_reset(&dsrdata);
	}
	return (false);
}

/*%
 * seek_ds looks for DS rrsets at the label indicated by val->labels,
 * for an insecurity proof.
 *
 * Returns:
 * \li	ISC_R_COMPLETE		a result has been determined and copied
 * 				into `*resp`; ISC_R_SUCCESS indicates that
 * 				the name has been proven insecure and any
 * 				other result indicates failure.
 * \li	DNS_R_CONTINUE		result is indeterminate; caller should
 * 				continue walking down labels.
 */
static isc_result_t
seek_ds(dns_validator_t *val, isc_result_t *resp) {
	isc_result_t result;
	char namebuf[DNS_NAME_FORMATSIZE];
	dns_fixedname_t fixedfound;
	dns_name_t *found = dns_fixedname_initname(&fixedfound);
	dns_name_t *tname = dns_fixedname_initname(&val->fname);

	if (val->labels == dns_name_countlabels(val->event->name)) {
		dns_name_copynf(val->event->name, tname);
	} else {
		dns_name_split(val->event->name, val->labels, NULL, tname);
	}

	dns_name_format(tname, namebuf, sizeof(namebuf));
	validator_log(val, ISC_LOG_DEBUG(3),
		      "checking existence of DS at '%s'", namebuf);

	result = view_find(val, tname, dns_rdatatype_ds);
	switch (result) {
	case DNS_R_NXRRSET:
	case DNS_R_NCACHENXRRSET:
		/*
		 * There is no DS.  If this is a delegation,
		 * we may be done.
		 *
		 * If we have "trust == answer" then this namespace
		 * has switched from insecure to should be secure.
		 */
		if (DNS_TRUST_PENDING(val->frdataset.trust) ||
		    DNS_TRUST_ANSWER(val->frdataset.trust))
		{
			result = create_validator(val, tname,
						  dns_rdatatype_ds,
						  &val->frdataset,
						  NULL, dsvalidated,
						  "proveunsecure");
			*resp = DNS_R_WAIT;
			if (result != ISC_R_SUCCESS) {
				*resp = result;
			}
			return (ISC_R_COMPLETE);
		}

		/*
		 * Zones using NSEC3 don't return a NSEC RRset so
		 * we need to use dns_view_findzonecut2 to find
		 * the zone cut.
		 */
		if (result == DNS_R_NXRRSET &&
		    !dns_rdataset_isassociated(&val->frdataset) &&
		    dns_view_findzonecut(val->view, tname, found, NULL,
					 0, 0, false, false,
					 NULL, NULL) == ISC_R_SUCCESS &&
		    dns_name_equal(tname, found))
		{
			*resp = markanswer(val, "proveunsecure (3)",
					   "no DS at zone cut");
			return (ISC_R_COMPLETE);
		}

		if (val->frdataset.trust < dns_trust_secure) {
			/*
			 * This shouldn't happen, since the negative
			 * response should have been validated.  Since
			 * there's no way of validating existing
			 * negative response blobs, give up.
			 */
			validator_log(val, ISC_LOG_WARNING,
				      "can't validate existing "
				      "negative responses (no DS)");
			*resp = DNS_R_MUSTBESECURE;
			return (ISC_R_COMPLETE);
		}

		if (isdelegation(tname, &val->frdataset, result)) {
			*resp = markanswer(val, "proveunsecure (4)",
					   "this is a delegation");
			return (ISC_R_COMPLETE);
		}

		break;

	case DNS_R_CNAME:
		if (DNS_TRUST_PENDING(val->frdataset.trust) ||
		    DNS_TRUST_ANSWER(val->frdataset.trust))
		{
			result = create_validator(val, tname,
						  dns_rdatatype_cname,
						  &val->frdataset,
						  NULL, cnamevalidated,
						  "proveunsecure "
						  "(cname)");
			*resp = DNS_R_WAIT;
			if (result != ISC_R_SUCCESS) {
				*resp = result;
			}
			return (ISC_R_COMPLETE);
		}

		break;

	case ISC_R_SUCCESS:
		/*
		 * There is a DS here.  Verify that it's secure and
		 * continue walking down labels.
		 */
		if (val->frdataset.trust >= dns_trust_secure) {
			if (!check_ds(val, tname, &val->frdataset)) {
				validator_log(val, ISC_LOG_DEBUG(3),
					      "no supported algorithm/"
					      "digest (%s/DS)",
					      namebuf);
				*resp = markanswer(val, "proveunsecure (5)",
						   "no supported "
						   "algorithm/digest (DS)");
				return (ISC_R_COMPLETE);
			}

			break;
		}

		if (!dns_rdataset_isassociated(&val->fsigrdataset)) {
			validator_log(val, ISC_LOG_DEBUG(3), "DS is unsigned");
			*resp = DNS_R_NOVALIDSIG;
		} else {
			/*
			 * Validate / re-validate answer.
			 */
			result = create_validator(val, tname,
						  dns_rdatatype_ds,
						  &val->frdataset,
						  &val->fsigrdataset,
						  dsvalidated,
						  "proveunsecure");
			*resp = DNS_R_WAIT;
			if (result != ISC_R_SUCCESS) {
				*resp = result;
			}
		}

		return (ISC_R_COMPLETE);

	case DNS_R_NXDOMAIN:
	case DNS_R_NCACHENXDOMAIN:
		/*
		 * This is not a zone cut. Assuming things are
		 * as expected, continue.
		 */
		if (!dns_rdataset_isassociated(&val->frdataset)) {
			/*
			 * There should be an NSEC here, since we
			 * are still in a secure zone.
			 */
			*resp = DNS_R_NOVALIDNSEC;
			return (ISC_R_COMPLETE);
		} else if (DNS_TRUST_PENDING(val->frdataset.trust) ||
			   DNS_TRUST_ANSWER(val->frdataset.trust))
		{
			/*
			 * If we have "trust == answer" then this
			 * namespace has switched from insecure to
			 * should be secure.
			 */
			*resp = DNS_R_WAIT;
			result = create_validator(val, tname,
						  dns_rdatatype_ds,
						  &val->frdataset,
						  NULL, dsvalidated,
						  "proveunsecure");
			if (result != ISC_R_SUCCESS) {
				*resp = result;
			}
			return (ISC_R_COMPLETE);
		} else if (val->frdataset.trust < dns_trust_secure) {
			/*
			 * This shouldn't happen, since the negative
			 * response should have been validated.  Since
			 * there's no way of validating existing
			 * negative response blobs, give up.
			 */
			validator_log(val, ISC_LOG_WARNING,
				      "can't validate existing "
				      "negative responses "
				      "(not a zone cut)");
			*resp = DNS_R_NOVALIDSIG;
			return (ISC_R_COMPLETE);
		}

		break;

	case ISC_R_NOTFOUND:
		/*
		 * We don't know anything about the DS.  Find it.
		 */
		*resp = DNS_R_WAIT;
		result = create_fetch(val, tname, dns_rdatatype_ds,
				      dsfetched, "proveunsecure");
		if (result != ISC_R_SUCCESS) {
			*resp = result;
		}
		return (ISC_R_COMPLETE);

	default:
		*resp = result;
		return (ISC_R_COMPLETE);
	}

	/*
	 * No definite answer yet; continue walking down labels.
	 */
	return (DNS_R_CONTINUE);
}

/*%
 * proveunsecure walks down, label by label, from the closest enclosing
 * trust anchor to the name that is being validated, looking for an
 * endpoint in the chain of trust.  That occurs when we can prove that
 * a DS record does not exist at a delegation point, or that a DS exists
 * at a delegation point but we don't support its algorithm/digest.  If
 * no such endpoint is found, then the response should have been secure.
 *
 * Returns:
 * \li	ISC_R_SUCCESS		val->event->name is in an unsecure zone
 * \li	DNS_R_WAIT		validation is in progress.
 * \li	DNS_R_MUSTBESECURE	val->event->name is supposed to be secure
 *				(policy) but we proved that it is unsecure.
 * \li	DNS_R_NOVALIDSIG
 * \li	DNS_R_NOVALIDNSEC
 * \li	DNS_R_NOTINSECURE
 * \li	DNS_R_BROKENCHAIN
 */
static isc_result_t
proveunsecure(dns_validator_t *val, bool have_ds, bool resume) {
	isc_result_t result;
	char namebuf[DNS_NAME_FORMATSIZE];
	dns_fixedname_t fixedsecroot;
	dns_name_t *secroot = dns_fixedname_initname(&fixedsecroot);
	unsigned int labels;

	/*
	 * We're attempting to prove insecurity.
	 */
	val->attributes |= VALATTR_INSECURITY;

	dns_name_copynf(val->event->name, secroot);

	/*
	 * If this is a response to a DS query, we need to look in
	 * the parent zone for the trust anchor.
	 */
	labels = dns_name_countlabels(secroot);
	if (val->event->type == dns_rdatatype_ds && labels > 1U) {
		dns_name_getlabelsequence(secroot, 1, labels - 1, secroot);
	}

	result = dns_keytable_finddeepestmatch(val->keytable, secroot, secroot);
	if (result == ISC_R_NOTFOUND) {
		validator_log(val, ISC_LOG_DEBUG(3),
			      "not beneath secure root");
		return (markanswer(val, "proveunsecure (1)",
				   "not beneath secure root"));
	} else if (result != ISC_R_SUCCESS) {
		return (result);
	}

	if (!resume) {
		/*
		 * We are looking for interruptions in the chain of trust.
		 * That can only happen *below* the trust anchor, so we
		 * start looking at the next label down.
		 */
		val->labels = dns_name_countlabels(secroot) + 1;
	} else {
		validator_log(val, ISC_LOG_DEBUG(3), "resuming proveunsecure");

		/*
		 * If we have a DS rdataset and it is secure, check whether
		 * it has a supported algorithm combination.  If not, this is
		 * an insecure delegation as far as this resolver is concerned.
		 */
		if (have_ds && val->frdataset.trust >= dns_trust_secure &&
		    !check_ds(val, dns_fixedname_name(&val->fname),
			      &val->frdataset))
		{
			dns_name_format(dns_fixedname_name(&val->fname),
					namebuf, sizeof(namebuf));
			validator_log(val, ISC_LOG_DEBUG(3),
				      "no supported algorithm/digest (%s/DS)",
				      namebuf);
			result = markanswer(val, "proveunsecure (2)", namebuf);
			goto out;
		}
		val->labels++;
	}

	/*
	 * Walk down through each of the remaining labels in the name,
	 * looking for DS records.
	 */
	while (val->labels <= dns_name_countlabels(val->event->name)) {
		isc_result_t tresult;

		result = seek_ds(val, &tresult);
		if (result == ISC_R_COMPLETE) {
			result = tresult;
			goto out;
		}

		INSIST(result == DNS_R_CONTINUE);
		val->labels++;
	}

	/* Couldn't complete insecurity proof. */
	validator_log(val, ISC_LOG_DEBUG(3),
		      "insecurity proof failed: %s",
		      isc_result_totext(result));
	return (DNS_R_NOTINSECURE);

 out:
	if (result != DNS_R_WAIT) {
		disassociate_rdatasets(val);
	}
	return (result);
}

/*%
 * Start the validation process.
 *
 * Attempt to validate the answer based on the category it appears to
 * fall in.
 * \li	1. secure positive answer.
 * \li	2. unsecure positive answer.
 * \li	3. a negative answer (secure or unsecure).
 *
 * Note an answer that appears to be a secure positive answer may actually
 * be an unsecure positive answer.
 */
static void
validator_start(isc_task_t *task, isc_event_t *event) {
	dns_validator_t *val;
	dns_validatorevent_t *vevent;
	bool want_destroy = false;
	isc_result_t result = ISC_R_FAILURE;

	UNUSED(task);
	REQUIRE(event->ev_type == DNS_EVENT_VALIDATORSTART);
	vevent = (dns_validatorevent_t *)event;
	val = vevent->validator;

	/* If the validator has been canceled, val->event == NULL */
	if (val->event == NULL) {
		return;
	}

	validator_log(val, ISC_LOG_DEBUG(3), "starting");

	LOCK(&val->lock);

	if (val->event->rdataset != NULL && val->event->sigrdataset != NULL) {
		isc_result_t saved_result;

		/*
		 * This looks like a simple validation.  We say "looks like"
		 * because it might end up requiring an insecurity proof.
		 */
		validator_log(val, ISC_LOG_DEBUG(3),
			      "attempting positive response validation");

		INSIST(dns_rdataset_isassociated(val->event->rdataset));
		INSIST(dns_rdataset_isassociated(val->event->sigrdataset));
		result = start_positive_validation(val);
		if (result == DNS_R_NOVALIDSIG &&
		    (val->attributes & VALATTR_TRIEDVERIFY) == 0)
		{
			saved_result = result;
			validator_log(val, ISC_LOG_DEBUG(3),
				      "falling back to insecurity proof");
			result = proveunsecure(val, false, false);
			if (result == DNS_R_NOTINSECURE) {
				result = saved_result;
			}
		}
	} else if (val->event->rdataset != NULL &&
		   val->event->rdataset->type != 0)
	{
		/*
		 * This is either an unsecure subdomain or a response from
		 * a broken server.
		 */
		INSIST(dns_rdataset_isassociated(val->event->rdataset));
		validator_log(val, ISC_LOG_DEBUG(3),
			      "attempting insecurity proof");

		result = proveunsecure(val, false, false);
		if (result == DNS_R_NOTINSECURE) {
			validator_log(val, ISC_LOG_INFO,
				      "got insecure response; "
				      "parent indicates it should be secure");
		}
	} else if ((val->event->rdataset == NULL &&
		    val->event->sigrdataset == NULL) ||
		   (val->event->rdataset != NULL &&
		    NEGATIVE(val->event->rdataset)))

	{
		/*
		 * This is a nonexistence validation.
		 */
		validator_log(val, ISC_LOG_DEBUG(3),
			      "attempting negative response validation");

		if (val->event->message->rcode == dns_rcode_nxdomain) {
			val->attributes |= VALATTR_NEEDNOQNAME;
			val->attributes |= VALATTR_NEEDNOWILDCARD;
		} else {
			val->attributes |= VALATTR_NEEDNODATA;
		}
		result = validate_nx(val, false);
	} else {
		INSIST(0);
		ISC_UNREACHABLE();
	}

	if (result != DNS_R_WAIT) {
		want_destroy = exit_check(val);
		validator_done(val, result);
	}

	UNLOCK(&val->lock);
	if (want_destroy) {
		destroy(val);
	}
}

isc_result_t
dns_validator_create(dns_view_t *view, dns_name_t *name, dns_rdatatype_t type,
		     dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset,
		     dns_message_t *message, unsigned int options,
		     isc_task_t *task, isc_taskaction_t action, void *arg,
		     dns_validator_t **validatorp)
{
	isc_result_t result = ISC_R_FAILURE;
	dns_validator_t *val;
	isc_task_t *tclone = NULL;
	dns_validatorevent_t *event;

	REQUIRE(name != NULL);
	REQUIRE(rdataset != NULL ||
		(rdataset == NULL && sigrdataset == NULL && message != NULL));
	REQUIRE(validatorp != NULL && *validatorp == NULL);

	val = isc_mem_get(view->mctx, sizeof(*val));
	val->view = NULL;
	dns_view_weakattach(view, &val->view);

	event = (dns_validatorevent_t *)
		isc_event_allocate(view->mctx, task,
				   DNS_EVENT_VALIDATORSTART,
				   validator_start, NULL,
				   sizeof(dns_validatorevent_t));

	isc_task_attach(task, &tclone);
	event->validator = val;
	event->result = ISC_R_FAILURE;
	event->name = name;
	event->type = type;
	event->rdataset = rdataset;
	event->sigrdataset = sigrdataset;
	event->message = message;
	memset(event->proofs, 0, sizeof(event->proofs));
	event->optout = false;
	event->secure = false;

	isc_mutex_init(&val->lock);

	val->event = event;
	val->options = options;
	val->attributes = 0;
	val->fetch = NULL;
	val->subvalidator = NULL;
	val->parent = NULL;

	val->keytable = NULL;
	result = dns_view_getsecroots(val->view, &val->keytable);
	if (result != ISC_R_SUCCESS) {
		goto cleanup;
	}
	val->keynode = NULL;
	val->key = NULL;
	val->siginfo = NULL;
	val->task = task;
	val->action = action;
	val->arg = arg;
	val->labels = 0;
	val->currentset = NULL;
	val->keyset = NULL;
	val->dsset = NULL;
	val->seensig = false;
	val->depth = 0;
	val->authcount = 0;
	val->authfail = 0;
	val->mustbesecure = dns_resolver_getmustbesecure(view->resolver, name);
	dns_rdataset_init(&val->frdataset);
	dns_rdataset_init(&val->fsigrdataset);
	dns_fixedname_init(&val->wild);
	dns_fixedname_init(&val->nearest);
	dns_fixedname_init(&val->closest);
	isc_stdtime_get(&val->start);
	ISC_LINK_INIT(val, link);
	val->magic = VALIDATOR_MAGIC;

	if ((options & DNS_VALIDATOR_DEFER) == 0) {
		isc_task_send(task, ISC_EVENT_PTR(&event));
	}

	*validatorp = val;

	return (ISC_R_SUCCESS);

 cleanup:
	isc_mutex_destroy(&val->lock);

	isc_task_detach(&tclone);
	isc_event_free(ISC_EVENT_PTR(&event));

	dns_view_weakdetach(&val->view);
	isc_mem_put(view->mctx, val, sizeof(*val));

	return (result);
}

void
dns_validator_send(dns_validator_t *validator) {
	isc_event_t *event;
	REQUIRE(VALID_VALIDATOR(validator));

	LOCK(&validator->lock);

	INSIST((validator->options & DNS_VALIDATOR_DEFER) != 0);
	event = (isc_event_t *)validator->event;
	validator->options &= ~DNS_VALIDATOR_DEFER;
	UNLOCK(&validator->lock);

	isc_task_send(validator->task, ISC_EVENT_PTR(&event));
}

void
dns_validator_cancel(dns_validator_t *validator) {
	dns_fetch_t *fetch = NULL;

	REQUIRE(VALID_VALIDATOR(validator));

	LOCK(&validator->lock);

	validator_log(validator, ISC_LOG_DEBUG(3), "dns_validator_cancel");

	if ((validator->attributes & VALATTR_CANCELED) == 0) {
		validator->attributes |= VALATTR_CANCELED;
		if (validator->event != NULL) {
			fetch = validator->fetch;
			validator->fetch = NULL;

			if (validator->subvalidator != NULL) {
				dns_validator_cancel(validator->subvalidator);
			}
			if ((validator->options & DNS_VALIDATOR_DEFER) != 0) {
				validator->options &= ~DNS_VALIDATOR_DEFER;
				validator_done(validator, ISC_R_CANCELED);
			}
		}
	}
	UNLOCK(&validator->lock);

	/* Need to cancel and destroy the fetch outside validator lock */
	if (fetch != NULL) {
		dns_resolver_cancelfetch(fetch);
		dns_resolver_destroyfetch(&fetch);
	}
}

static void
destroy(dns_validator_t *val) {
	isc_mem_t *mctx;

	REQUIRE(SHUTDOWN(val));
	REQUIRE(val->event == NULL);
	REQUIRE(val->fetch == NULL);

	if (val->keynode != NULL) {
		dns_keytable_detachkeynode(val->keytable, &val->keynode);
	} else if (val->key != NULL) {
		dst_key_free(&val->key);
	}
	if (val->keytable != NULL) {
		dns_keytable_detach(&val->keytable);
	}
	if (val->subvalidator != NULL) {
		dns_validator_destroy(&val->subvalidator);
	}
	disassociate_rdatasets(val);
	mctx = val->view->mctx;
	if (val->siginfo != NULL) {
		isc_mem_put(mctx, val->siginfo, sizeof(*val->siginfo));
	}
	isc_mutex_destroy(&val->lock);
	dns_view_weakdetach(&val->view);
	val->magic = 0;
	isc_mem_put(mctx, val, sizeof(*val));
}

void
dns_validator_destroy(dns_validator_t **validatorp) {
	dns_validator_t *val;
	bool want_destroy = false;

	REQUIRE(validatorp != NULL);
	val = *validatorp;
	REQUIRE(VALID_VALIDATOR(val));

	LOCK(&val->lock);

	val->attributes |= VALATTR_SHUTDOWN;
	validator_log(val, ISC_LOG_DEBUG(4), "dns_validator_destroy");

	want_destroy = exit_check(val);
	UNLOCK(&val->lock);
	if (want_destroy) {
		destroy(val);
	}

	*validatorp = NULL;
}

static void
validator_logv(dns_validator_t *val, isc_logcategory_t *category,
	       isc_logmodule_t *module, int level, const char *fmt, va_list ap)
{
	char msgbuf[2048];
	static const char spaces[] = "        *";
	int depth = val->depth * 2;
	const char *viewname, *sep1, *sep2;

	vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);

	if ((unsigned int) depth >= sizeof spaces) {
		depth = sizeof spaces - 1;
	}

	/*
	 * Log the view name unless it's:
	 * * "_default/IN" (which means there's only one view
	 *   configured in the server), or
	 * * "_dnsclient/IN" (which means this is being called
	 *   from an application using dns/client.c).
	 */
	if (val->view->rdclass == dns_rdataclass_in &&
	    (strcmp(val->view->name, "_default") == 0 ||
	     strcmp(val->view->name, DNS_CLIENTVIEW_NAME) == 0))
	{
		sep1 = viewname = sep2 = "";
	} else {
		sep1 = "view ";
		viewname = val->view->name;
		sep2 = ": ";
	}

	if (val->event != NULL && val->event->name != NULL) {
		char namebuf[DNS_NAME_FORMATSIZE];
		char typebuf[DNS_RDATATYPE_FORMATSIZE];

		dns_name_format(val->event->name, namebuf, sizeof(namebuf));
		dns_rdatatype_format(val->event->type, typebuf,
				     sizeof(typebuf));
		isc_log_write(dns_lctx, category, module, level,
			      "%s%s%s%.*svalidating %s/%s: %s",
			      sep1, viewname, sep2, depth, spaces,
			      namebuf, typebuf, msgbuf);
	} else {
		isc_log_write(dns_lctx, category, module, level,
			      "%s%s%s%.*svalidator @%p: %s",
			      sep1, viewname, sep2, depth, spaces,
			      val, msgbuf);
	}
}

static void
validator_log(void *val, int level, const char *fmt, ...) {
	va_list ap;

	if (!isc_log_wouldlog(dns_lctx, level)) {
		return;
	}

	va_start(ap, fmt);

	validator_logv(val, DNS_LOGCATEGORY_DNSSEC,
		       DNS_LOGMODULE_VALIDATOR, level, fmt, ap);
	va_end(ap);
}

static void
validator_logcreate(dns_validator_t *val, dns_name_t *name,
		    dns_rdatatype_t type, const char *caller,
		    const char *operation)
{
	char namestr[DNS_NAME_FORMATSIZE];
	char typestr[DNS_RDATATYPE_FORMATSIZE];

	dns_name_format(name, namestr, sizeof(namestr));
	dns_rdatatype_format(type, typestr, sizeof(typestr));
	validator_log(val, ISC_LOG_DEBUG(9), "%s: creating %s for %s %s",
		      caller, operation, namestr, typestr);
}
