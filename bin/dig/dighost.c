/*
 * Copyright (C) 2000  Internet Software Consortium.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM
 * DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
 * INTERNET SOFTWARE CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING
 * FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* $Id: dighost.c,v 1.166 2000/11/21 21:35:32 mws Exp $ */

/*
 * Notice to programmers:  Do not use this code as an example of how to
 * use the ISC library to perform DNS lookups.  Dig and Host both operate
 * on the request level, since they allow fine-tuning of output and are
 * intended as debugging tools.  As a result, they perform many of the
 * functions which could be better handled using the dns_resolver
 * functions in most applications.
 */

#include <config.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>
#include <limits.h>
#if (!(defined(HAVE_ADDRINFO) && defined(HAVE_GETADDRINFO)))
extern int h_errno;
#endif

#include <dns/byaddr.h>
#include <dns/fixedname.h>
#include <dns/message.h>
#include <dns/name.h>
#include <dns/opt.h>
#include <dns/rdata.h>
#include <dns/rdataclass.h>
#include <dns/rdatalist.h>
#include <dns/rdataset.h>
#include <dns/rdatastruct.h>
#include <dns/rdatatype.h>
#include <dns/result.h>
#include <dns/tsig.h>

#include <dst/dst.h>

#include <isc/app.h>
#include <isc/base64.h>
#include <isc/entropy.h>
#include <isc/lang.h>
#include <isc/netaddr.h>
#include <isc/netdb.h>
#include <isc/print.h>
#include <isc/result.h>
#include <isc/string.h>
#include <isc/task.h>
#include <isc/timer.h>
#include <isc/types.h>
#include <isc/util.h>

#include <dig/dig.h>

ISC_LIST(dig_lookup_t) lookup_list;
dig_serverlist_t server_list;
ISC_LIST(dig_searchlist_t) search_list;

isc_boolean_t
	have_ipv4 = ISC_FALSE,
	have_ipv6 = ISC_FALSE,
	specified_source = ISC_FALSE,
	free_now = ISC_FALSE,
	cancel_now = ISC_FALSE,
	usesearch = ISC_FALSE,
	qr = ISC_FALSE,
	is_dst_up = ISC_FALSE,
	have_domain = ISC_FALSE,
	is_blocking = ISC_FALSE;

in_port_t port = 53;
unsigned int timeout = 0;
isc_mem_t *mctx = NULL;
isc_taskmgr_t *taskmgr = NULL;
isc_task_t *global_task = NULL;
isc_timermgr_t *timermgr = NULL;
isc_socketmgr_t *socketmgr = NULL;
isc_sockaddr_t bind_address;
isc_sockaddr_t bind_any;
isc_buffer_t rootbuf;
int sendcount = 0;
int recvcount = 0;
int sockcount = 0;
int ndots = -1;
int tries = 2;
int lookup_counter = 0;
char fixeddomain[MXNAME] = "";
dig_searchlist_t *fixedsearch = NULL;
/*
 * Exit Codes:
 *   0   Everything went well, including things like NXDOMAIN
 *   1   Usage error
 *   7   Got too many RR's or Names
 *   8   Couldn't open batch file
 *   9   No reply from server
 *   10  Internal error
 */
int exitcode = 0;
char keynametext[MXNAME];
char keyfile[MXNAME] = "";
char keysecret[MXNAME] = "";
dns_name_t keyname;
isc_buffer_t *namebuf = NULL;
dns_tsigkey_t *key = NULL;
isc_boolean_t validated = ISC_TRUE;
isc_entropy_t *entp = NULL;
isc_mempool_t *commctx = NULL;
isc_boolean_t debugging = ISC_FALSE;
isc_boolean_t memdebugging = ISC_FALSE;
char *progname = NULL;
isc_mutex_t lookup_lock;
dig_lookup_t *current_lookup = NULL;
isc_uint32_t rr_limit = INT_MAX;

/*
 * Apply and clear locks at the event level in global task.
 * Can I get rid of these using shutdown events?  XXX
 */
#define LOCK_LOOKUP {\
        debug("lock_lookup %s:%d", __FILE__, __LINE__);\
        check_result(isc_mutex_lock((&lookup_lock)), "isc_mutex_lock");\
        debug("success");\
}
#define UNLOCK_LOOKUP {\
        debug("unlock_lookup %s:%d", __FILE__, __LINE__);\
        check_result(isc_mutex_unlock((&lookup_lock)),\
                     "isc_mutex_unlock");\
}

static void
cancel_lookup(dig_lookup_t *lookup);

static void
recv_done(isc_task_t *task, isc_event_t *event);

static void
connect_timeout(isc_task_t *task, isc_event_t *event);

char *
next_token(char **stringp, const char *delim) {
	char *res;

	do {
		res = strsep(stringp, delim);
		if (res == NULL)
			break;
	} while (*res == '\0');
	return (res);
}                       

static int
count_dots(char *string) {
	char *s;
	int i = 0;

	s = string;
	while (*s != '\0') {
		if (*s == '.')
			i++;
		s++;
	}
	return (i);
}

static void
hex_dump(isc_buffer_t *b) {
	unsigned int len;
	isc_region_t r;

	isc_buffer_usedregion(b, &r);

	printf("%d bytes\n", r.length);
	for (len = 0; len < r.length; len++) {
		printf("%02x ", r.base[len]);
		if (len % 16 == 15)
			printf("\n");
	}
	if (len % 16 != 0)
		printf("\n");
}


isc_result_t
get_reverse(char reverse[MXNAME], char *value, isc_boolean_t nibble) {
	int adrs[4];
	char working[MXNAME];
	int i, n;
	isc_result_t result;

	result = DNS_R_BADDOTTEDQUAD;

	debug("get_reverse(%s)", value);
	if (strspn(value, "0123456789.") == strlen(value)) {
		n = sscanf(value, "%d.%d.%d.%d",
			   &adrs[0], &adrs[1],
			   &adrs[2], &adrs[3]);
		if (n == 0) {
			return (DNS_R_BADDOTTEDQUAD);
		}
		for (i = n - 1; i >= 0; i--) {
			snprintf(working, MXNAME/8, "%d.",
				 adrs[i]);
			strncat(reverse, working, MXNAME);
		}
		strncat(reverse, "in-addr.arpa.", MXNAME);
		result = ISC_R_SUCCESS;
	} else if (strspn(value, "0123456789abcdefABCDEF:") 
		   == strlen(value)) {
		isc_netaddr_t addr;
		dns_fixedname_t fname;
		dns_name_t *name;
		isc_buffer_t b;
		
		addr.family = AF_INET6;
		n = inet_pton(AF_INET6, value, &addr.type.in6);
		if (n <= 0)
			return (DNS_R_BADDOTTEDQUAD);
		dns_fixedname_init(&fname);
		name = dns_fixedname_name(&fname);
		result = dns_byaddr_createptrname(&addr, nibble,
						  name);
		if (result != ISC_R_SUCCESS)
			return (result);
		isc_buffer_init(&b, reverse, MXNAME);
		result = dns_name_totext(name, ISC_FALSE, &b);
		isc_buffer_putuint8(&b, 0);
	}
	return (result);
}

void
fatal(const char *format, ...) {
	va_list args;

	fprintf(stderr, "%s: ", progname);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fprintf(stderr, "\n");
	if (exitcode < 10)
		exitcode = 10;
	exit(exitcode);
}

void
debug(const char *format, ...) {
	va_list args;

	if (debugging) {
		va_start(args, format);
		vfprintf(stderr, format, args);
		va_end(args);
		fprintf(stderr, "\n");
	}
}

void
check_result(isc_result_t result, const char *msg) {
	if (result != ISC_R_SUCCESS) {
		fatal("%s: %s", msg, isc_result_totext(result));
	}
}

/*
 * Create a server structure, which is part of the lookup structure.
 * This is little more than a linked list of servers to query in hopes
 * of finding the answer the user is looking for
 */
dig_server_t *
make_server(const char *servname) {
	dig_server_t *srv;

	REQUIRE(servname != NULL);

	debug("make_server(%s)", servname);
	srv = isc_mem_allocate(mctx, sizeof(struct dig_server));
	if (srv == NULL)
		fatal("Memory allocation failure in %s:%d",
		      __FILE__, __LINE__);
	strncpy(srv->servername, servname, MXNAME);
	srv->servername[MXNAME-1] = 0;
	ISC_LINK_INIT(srv, link);
	return (srv);
}

/*
 * Produce a cloned server list.  The dest list must have already had
 * ISC_LIST_INIT applied.
 */
void
clone_server_list(dig_serverlist_t src,
		  dig_serverlist_t *dest)
{
	dig_server_t *srv, *newsrv;

	debug("clone_server_list()");
	srv = ISC_LIST_HEAD(src);
	while (srv != NULL) {
		newsrv = make_server(srv->servername);
		ISC_LINK_INIT(newsrv, link);
		ISC_LIST_ENQUEUE(*dest, newsrv, link);
		srv = ISC_LIST_NEXT(srv, link);
	}
}

/*
 * Create an empty lookup structure, which holds all the information needed
 * to get an answer to a user's question.  This structure contains two
 * linked lists: the server list (servers to query) and the query list
 * (outstanding queries which have been made to the listed servers).
 */
dig_lookup_t *
make_empty_lookup(void) {
	dig_lookup_t *looknew;

	debug("make_empty_lookup()");

	INSIST(!free_now);

	looknew = isc_mem_allocate(mctx, sizeof(struct dig_lookup));
	if (looknew == NULL)
		fatal("Memory allocation failure in %s:%d",
		       __FILE__, __LINE__);
	looknew->pending = ISC_TRUE;
	looknew->textname[0] = 0;
	looknew->cmdline[0] = 0; /* Not copied in clone_lookup! */
	looknew->rdtype = dns_rdatatype_a;
	looknew->rdclass = dns_rdataclass_in;
	looknew->sendspace = NULL;
	looknew->sendmsg = NULL;
	looknew->name = NULL;
	looknew->oname = NULL;
	looknew->timer = NULL;
	looknew->xfr_q = NULL;
	looknew->current_query = NULL;
	looknew->doing_xfr = ISC_FALSE;
	looknew->ixfr_serial = ISC_FALSE;
	looknew->defname = ISC_FALSE;
	looknew->trace = ISC_FALSE;
	looknew->trace_root = ISC_FALSE;
	looknew->identify = ISC_FALSE;
	looknew->ignore = ISC_FALSE;
	looknew->servfail_stops = ISC_FALSE;
	looknew->besteffort = ISC_TRUE;
	looknew->dnssec = ISC_FALSE;
	looknew->udpsize = 0;
	looknew->recurse = ISC_TRUE;
	looknew->aaonly = ISC_FALSE;
	looknew->adflag = ISC_FALSE;
	looknew->cdflag = ISC_FALSE;
	looknew->ns_search_only = ISC_FALSE;
	looknew->origin = NULL;
	looknew->querysig = NULL;
	looknew->retries = tries;
	looknew->nsfound = 0;
	looknew->tcp_mode = ISC_FALSE;
	looknew->nibble = ISC_FALSE;
	looknew->comments = ISC_TRUE;
	looknew->stats = ISC_TRUE;
	looknew->section_question = ISC_TRUE;
	looknew->section_answer = ISC_TRUE;
	looknew->section_authority = ISC_TRUE;
	looknew->section_additional = ISC_TRUE;
	looknew->new_search = ISC_FALSE;
#ifdef DNS_OPT_NEWCODES_LIVE
	looknew->zonename[0] = 0;
	looknew->viewname[0] = 0;
#endif /* DNS_OPT_NEWCODES_LIVE */
	ISC_LINK_INIT(looknew, link);
	ISC_LIST_INIT(looknew->q);
	ISC_LIST_INIT(looknew->my_server_list);
	return (looknew);
}

/*
 * Clone a lookup, perhaps copying the server list.  This does not clone
 * the query list, since it will be regenerated by the setup_lookup()
 * function, nor does it queue up the new lookup for processing.
 * Caution: If you don't clone the servers, you MUST clone the server
 * list seperately from somewhere else, or construct it by hand.
 */
dig_lookup_t *
clone_lookup(dig_lookup_t *lookold, isc_boolean_t servers) {
	dig_lookup_t *looknew;

	debug("clone_lookup()");

	INSIST(!free_now);

	looknew = make_empty_lookup();
	INSIST(looknew != NULL);
	strncpy(looknew->textname, lookold-> textname, MXNAME);
	looknew->textname[MXNAME-1]=0;
	looknew->rdtype = lookold->rdtype;
	looknew->rdclass = lookold->rdclass;
	looknew->doing_xfr = lookold->doing_xfr;
	looknew->ixfr_serial = lookold->ixfr_serial;
	looknew->defname = lookold->defname;
	looknew->trace = lookold->trace;
	looknew->trace_root = lookold->trace_root;
	looknew->identify = lookold->identify;
	looknew->ignore = lookold->ignore;
	looknew->servfail_stops = lookold->servfail_stops;
	looknew->besteffort = lookold->besteffort;
	looknew->dnssec = lookold->dnssec;
	looknew->udpsize = lookold->udpsize;
	looknew->recurse = lookold->recurse;
        looknew->aaonly = lookold->aaonly;
	looknew->adflag = lookold->adflag;
	looknew->cdflag = lookold->cdflag;
	looknew->ns_search_only = lookold->ns_search_only;
	looknew->tcp_mode = lookold->tcp_mode;
	looknew->comments = lookold->comments;
	looknew->stats = lookold->stats;
	looknew->section_question = lookold->section_question;
	looknew->section_answer = lookold->section_answer;
	looknew->section_authority = lookold->section_authority;
	looknew->section_additional = lookold->section_additional;
	looknew->retries = lookold->retries;
#ifdef DNS_OPT_NEWCODES_LIVE
	strncpy(looknew->viewname, lookold-> viewname, MXNAME);
	strncpy(looknew->zonename, lookold-> zonename, MXNAME);
#endif /* DNS_OPT_NEWCODES_LIVE */

	if (servers)
		clone_server_list(lookold->my_server_list,
				  &looknew->my_server_list);
	return (looknew);
}

/*
 * Requeue a lookup for further processing, perhaps copying the server
 * list.  The new lookup structure is returned to the caller, and is
 * queued for processing.  If servers are not cloned in the requeue, they
 * must be added before allowing the current event to complete, since the
 * completion of the event may result in the next entry on the lookup
 * queue getting run.
 */
dig_lookup_t *
requeue_lookup(dig_lookup_t *lookold, isc_boolean_t servers) {
	dig_lookup_t *looknew;

	debug("requeue_lookup()");

	lookup_counter++;
	if (lookup_counter > LOOKUP_LIMIT)
		fatal("Too many lookups");

	looknew = clone_lookup(lookold, servers);
	INSIST(looknew != NULL);

	debug("before insertion, init@%p "
	       "-> %p, new@%p -> %p",
	      lookold, lookold->link.next, looknew, looknew->link.next);
	ISC_LIST_PREPEND(lookup_list, looknew, link);
	debug("after insertion, init -> "
	      "%p, new = %p, new -> %p",
	      lookold, looknew, looknew->link.next);
	return (looknew);
}


static void
setup_text_key(void) {
	isc_result_t result;
	isc_buffer_t secretbuf;
	int secretsize;
	unsigned char *secretstore;
	isc_stdtime_t now;

	debug("setup_text_key()");
	result = isc_buffer_allocate(mctx, &namebuf, MXNAME);
	check_result(result, "isc_buffer_allocate");
	dns_name_init(&keyname, NULL);
	check_result(result, "dns_name_init");
	isc_buffer_putstr(namebuf, keynametext);
	secretsize = strlen(keysecret) * 3 / 4;
	secretstore = isc_mem_allocate(mctx, secretsize);
	if (secretstore == NULL)
		fatal("Memory allocation failure in %s:%d",
		      __FILE__, __LINE__);
	isc_buffer_init(&secretbuf, secretstore, secretsize);
	result = isc_base64_decodestring(mctx, keysecret,
					 &secretbuf);
	if (result != ISC_R_SUCCESS) {
		printf(";; Couldn't create key %s: %s\n",
		       keynametext, isc_result_totext(result));
		goto failure;
	}
	secretsize = isc_buffer_usedlength(&secretbuf);
	isc_stdtime_get(&now);

	result = dns_name_fromtext(&keyname, namebuf,
				   dns_rootname, ISC_FALSE,
				   namebuf);
	if (result != ISC_R_SUCCESS) {
		printf(";; Couldn't create key %s: %s\n",
		       keynametext, dns_result_totext(result));
		goto failure;
	}
	result = dns_tsigkey_create(&keyname, dns_tsig_hmacmd5_name,
				    secretstore, secretsize,
				    ISC_TRUE, NULL, now, now, mctx,
				    NULL, &key);
	if (result != ISC_R_SUCCESS) {
		printf(";; Couldn't create key %s: %s\n",
		       keynametext, dns_result_totext(result));
	}
 failure:
	isc_mem_free(mctx, secretstore);
	dns_name_invalidate(&keyname);
	isc_buffer_free(&namebuf);
}


static void
setup_file_key(void) {
	isc_result_t result;
	isc_buffer_t secretbuf;
	unsigned char *secretstore = NULL;
	int secretlen;
	dst_key_t *dstkey = NULL;
	isc_stdtime_t now;


	debug("setup_file_key()");
	result = dst_key_fromnamedfile(keyfile, DST_TYPE_PRIVATE,
				       mctx, &dstkey);
	if (result != ISC_R_SUCCESS) {
		fprintf(stderr, "Couldn't read key from %s: %s\n",
			keyfile, isc_result_totext(result));
		goto failure;
	}
	/*
	 * Get key size in bits, convert to bytes, rounding up (?)
	 */
	secretlen = (dst_key_size(dstkey) + 7) >> 3;
	secretstore = isc_mem_allocate(mctx, secretlen);
	if (secretstore == NULL)
		fatal("out of memory");
	isc_buffer_init(&secretbuf, secretstore, secretlen);
	result = dst_key_tobuffer(dstkey, &secretbuf);
	if (result != ISC_R_SUCCESS) {
		fprintf(stderr, "Couldn't read key from %s: %s\n",
			keyfile, isc_result_totext(result));
		goto failure;
	}
	isc_stdtime_get(&now);
	dns_name_init(&keyname, NULL);
	dns_name_clone(dst_key_name(dstkey), &keyname);
	result = dns_tsigkey_create(&keyname, dns_tsig_hmacmd5_name,
				    secretstore, secretlen,
				    ISC_TRUE, NULL, now, now, mctx,
				    NULL, &key);
	if (result != ISC_R_SUCCESS) {
		printf(";; Couldn't create key %s: %s\n",
		       keynametext, dns_result_totext(result));
	}
 failure:
	if (dstkey != NULL)
		dst_key_free(&dstkey);
	if (secretstore != NULL)
		isc_mem_free(mctx, secretstore);
}

/*
 * Setup the system as a whole, reading key information and resolv.conf
 * settings.
 */
void
setup_system(void) {
	char rcinput[MXNAME];
	FILE *fp;
	char *ptr;
	dig_server_t *srv;
	dig_searchlist_t *search;
	isc_boolean_t get_servers;
	char *input;

	debug("setup_system()");

	free_now = ISC_FALSE;
	get_servers = ISC_TF(server_list.head == NULL);
	fp = fopen(RESOLVCONF, "r");
	/* XXX Use lwres resolv.conf reader */
	if (fp != NULL) {
		while (fgets(rcinput, MXNAME, fp) != 0) {
			input = rcinput;
			ptr = next_token(&input, " \t\r\n");
			if (ptr != NULL) {
				if (get_servers &&
				    strcasecmp(ptr, "nameserver") == 0) {
					debug("got a nameserver line");
					ptr = next_token(&input, " \t\r\n");
					if (ptr != NULL) {
						srv = make_server(ptr);
						ISC_LIST_APPEND
							(server_list,
							 srv, link);
					}
				} else if (strcasecmp(ptr, "options") == 0) {
					ptr = next_token(&input, " \t\r\n");
					if (ptr != NULL) {
						if((strncasecmp(ptr, "ndots:",
							    6) == 0) &&
						    (ndots == -1)) {
							ndots = atoi(
							      &ptr[6]);
							debug("ndots is "
							       "%d.",
							       ndots);
						}
					}
				} else if (strcasecmp(ptr, "search") == 0){
					while ((ptr = next_token(&input, " \t\r\n"))
					       != NULL) {
						debug("adding search %s",
						      ptr);
						search = isc_mem_allocate(
						   mctx, sizeof(struct
								dig_server));
						if (search == NULL)
							fatal("Memory "
							      "allocation "
							      "failure in %s:"
							      "%d", __FILE__,
							      __LINE__);
						strncpy(search->
							origin,
							ptr,
							MXNAME);
						search->origin[MXNAME-1]=0;
						ISC_LIST_APPENDUNSAFE
							(search_list,
							 search,
							 link);
					}
				} else if ((strcasecmp(ptr, "domain") == 0) &&
					   (fixeddomain[0] == 0 )){
					have_domain = ISC_TRUE;
					while ((ptr = next_token(&input, " \t\r\n"))
					       != NULL) {
						search = isc_mem_allocate(
						   mctx, sizeof(struct
								dig_server));
						if (search == NULL)
							fatal("Memory "
							      "allocation "
							      "failure in %s:"
							      "%d", __FILE__,
							      __LINE__);
						strncpy(search->
							origin,
							ptr,
							MXNAME - 1);
						search->origin[MXNAME-1]=0;
						ISC_LIST_PREPENDUNSAFE
							(search_list,
							 search,
							 link);
					}
				}
			}
		}
		fclose(fp);
	}

	if (ndots == -1)
		ndots = 1;

	if (server_list.head == NULL) {
		srv = make_server("127.0.0.1");
		ISC_LIST_APPEND(server_list, srv, link);
	}

	if (keyfile[0] != 0)
		setup_file_key();
	else if (keysecret[0] != 0)
		setup_text_key();
}

/*
 * Setup the ISC and DNS libraries for use by the system.
 */
void
setup_libs(void) {
	isc_result_t result;

	debug("setup_libs()");

	/*
	 * Warning: This is not particularly good randomness.  We'll
	 * just use random() now for getting id values, but doing so
	 * does NOT insure that id's cann't be guessed.
	 */
	srandom(getpid() + (int)&setup_libs);

	result = isc_net_probeipv4();
	if (result == ISC_R_SUCCESS)
		have_ipv4 = ISC_TRUE;

	result = isc_net_probeipv6();
	if (result == ISC_R_SUCCESS)
		have_ipv6 = ISC_TRUE;
	if (!have_ipv6 && !have_ipv4)
		fatal("can't find either v4 or v6 networking");

	result = isc_mem_create(0, 0, &mctx);
	check_result(result, "isc_mem_create");

	result = isc_taskmgr_create(mctx, 1, 0, &taskmgr);
	check_result(result, "isc_taskmgr_create");

	result = isc_task_create(taskmgr, 0, &global_task);
	check_result(result, "isc_task_create");

	result = isc_timermgr_create(mctx, &timermgr);
	check_result(result, "isc_timermgr_create");

	result = isc_socketmgr_create(mctx, &socketmgr);
	check_result(result, "isc_socketmgr_create");

	result = isc_entropy_create(mctx, &entp);
	check_result(result, "isc_entropy_create");

	result = dst_lib_init(mctx, entp, 0);
	check_result(result, "dst_lib_init");
	is_dst_up = ISC_TRUE;

	result = isc_mempool_create(mctx, COMMSIZE, &commctx);
	check_result(result, "isc_mempool_create");
	isc_mempool_setname(commctx, "COMMPOOL");
	/*
	 * 6 and 2 set as reasonable parameters for 3 or 4 nameserver
	 * systems.
	 */
	isc_mempool_setfreemax(commctx, 6);
	isc_mempool_setfillcount(commctx, 2);

	result = isc_mutex_init(&lookup_lock);
	check_result(result, "isc_mutex_init");

	dns_result_register();
}

/*
 * Add EDNS0 option record to a message.  Currently, the only supported
 * option is UDP buffer size.
 */
static void
add_opt(dns_message_t *msg, isc_uint16_t udpsize, isc_boolean_t dnssec,
	dns_optlist_t optlist)
{
	dns_rdataset_t *rdataset = NULL;
	dns_rdatalist_t *rdatalist = NULL;
	dns_rdata_t *rdata = NULL;
	isc_result_t result;
#ifdef DNS_OPT_NEWCODES_LIVE
	isc_buffer_t *rdatabuf = NULL;
	unsigned int i, optsize = 0;
#else /* DNS_OPT_NEWCODES_LIVE */

	UNUSED(optlist);
#endif /* DNS_OPT_NEWCODES_LIVE */

	debug("add_opt()");
	result = dns_message_gettemprdataset(msg, &rdataset);
	check_result(result, "dns_message_gettemprdataset");
	dns_rdataset_init(rdataset);
	result = dns_message_gettemprdatalist(msg, &rdatalist);
	check_result(result, "dns_message_gettemprdatalist");
	result = dns_message_gettemprdata(msg, &rdata);
	check_result(result, "dns_message_gettemprdata");

	debug("setting udp size of %d", udpsize);
	rdatalist->type = dns_rdatatype_opt;
	rdatalist->covers = 0;
	rdatalist->rdclass = udpsize;
	rdatalist->ttl = 0;
	if (dnssec)
		rdatalist->ttl = DNS_MESSAGEEXTFLAG_DO;
	rdata->data = NULL;
	rdata->length = 0;
#ifdef DNS_OPT_NEWCODES_LIVE
	for (i=0; i<optlist.used; i++)
		optsize += optlist.attrs[i].value.length + 4;
	result = isc_buffer_allocate(mctx, &rdatabuf, optsize);
	check_result(result, "isc_buffer_allocate");
	result = dns_opt_add(rdata, &optlist, rdatabuf);
	check_result(result, "dns_opt_add");
	dns_message_takebuffer(msg, &rdatabuf);
#endif /* DNS_OPT_NEWCODES_LIVE */
	ISC_LIST_INIT(rdatalist->rdata);
	ISC_LIST_APPEND(rdatalist->rdata, rdata, link);
	dns_rdatalist_tordataset(rdatalist, rdataset);
	result = dns_message_setopt(msg, rdataset);
	check_result(result, "dns_message_setopt");
}

/*
 * Add a question section to a message, asking for the specified name,
 * type, and class.
 */
static void
add_question(dns_message_t *message, dns_name_t *name,
	     dns_rdataclass_t rdclass, dns_rdatatype_t rdtype)
{
	dns_rdataset_t *rdataset;
	isc_result_t result;

	debug("add_question()");
	rdataset = NULL;
	result = dns_message_gettemprdataset(message, &rdataset);
	check_result(result, "dns_message_gettemprdataset()");
	dns_rdataset_init(rdataset);
	dns_rdataset_makequestion(rdataset, rdclass, rdtype);
	ISC_LIST_APPEND(name->list, rdataset, link);
}

/*
 * Check if we're done with all the queued lookups, which is true iff
 * all sockets, sends, and recvs are accounted for (counters == 0),
 * and the lookup list is empty.
 * If we are done, pass control back out to dighost_shutdown() (which is
 * part of dig.c, host.c, or nslookup.c) to either shutdown the system as
 * a whole or reseed the lookup list.
 */
static void
check_if_done(void) {
	debug("check_if_done()");
	debug("list %s", ISC_LIST_EMPTY(lookup_list) ? "empty" : "full");
	if (ISC_LIST_EMPTY(lookup_list) && current_lookup == NULL &&
	    sendcount == 0) {
		INSIST(sockcount == 0);
		INSIST(recvcount == 0);
		debug("shutting down");
		dighost_shutdown();
	}
}

/*
 * Clear out a query when we're done with it.  WARNING: This routine
 * WILL invalidate the query pointer.
 */
static void
clear_query(dig_query_t *query) {
	dig_lookup_t *lookup;

	REQUIRE(query != NULL);

	debug("clear_query(%p)", query);

	lookup = query->lookup;

	if (lookup->current_query == query)
		lookup->current_query = NULL;

	ISC_LIST_UNLINK(lookup->q, query, link);
	if (ISC_LINK_LINKED(&query->recvbuf, link))
		ISC_LIST_DEQUEUE(query->recvlist, &query->recvbuf,
				 link);
	if (ISC_LINK_LINKED(&query->lengthbuf, link))
		ISC_LIST_DEQUEUE(query->lengthlist, &query->lengthbuf,
				 link);
	INSIST(query->recvspace != NULL);
	if (query->sock != NULL) {
		isc_socket_detach(&query->sock);
		sockcount--;
		debug("sockcount=%d", sockcount);
	}
	isc_mempool_put(commctx, query->recvspace);
	isc_buffer_invalidate(&query->recvbuf);
	isc_buffer_invalidate(&query->lengthbuf);
	isc_mem_free(mctx, query);
}

/*
 * Try and clear out a lookup if we're done with it.  Return ISC_TRUE if
 * the lookup was successfully cleared.  If ISC_TRUE is returned, the
 * lookup pointer has been invalidated.
 */
static isc_boolean_t
try_clear_lookup(dig_lookup_t *lookup) {
	dig_server_t *s;
	dig_query_t *q;
	void *ptr;

	REQUIRE(lookup != NULL);

	debug("try_clear_lookup(%p)", lookup);

	if (ISC_LIST_HEAD(lookup->q) != NULL) {
		if (debugging) {
			q = ISC_LIST_HEAD(lookup->q);
			while (q != NULL) {
				debug("query to %s still pending",
				       q->servname);
				q = ISC_LIST_NEXT(q, link);
			}
		return (ISC_FALSE);
		}
	}
	/*
	 * At this point, we know there are no queries on the lookup,
	 * so can make it go away also.
	 */
	debug("cleared");
	s = ISC_LIST_HEAD(lookup->my_server_list);
	while (s != NULL) {
		debug("freeing server %p belonging to %p",
		      s, lookup);
		ptr = s;
		s = ISC_LIST_NEXT(s, link);
		ISC_LIST_DEQUEUE(lookup->my_server_list,
				 (dig_server_t *)ptr, link);
		isc_mem_free(mctx, ptr);
	}
	if (lookup->sendmsg != NULL)
		dns_message_destroy(&lookup->sendmsg);
	if (lookup->querysig != NULL) {
		debug("freeing buffer %p", lookup->querysig);
		isc_buffer_free(&lookup->querysig);
	}
	if (lookup->timer != NULL)
		isc_timer_detach(&lookup->timer);
	if (lookup->sendspace != NULL)
		isc_mempool_put(commctx, lookup->sendspace);

	isc_mem_free(mctx, lookup);
	return (ISC_TRUE);
}


/*
 * If we can, start the next lookup in the queue running.
 * This assumes that the lookup on the head of the queue hasn't been
 * started yet.  It also removes the lookup from the head of the queue,
 * setting the current_lookup pointer pointing to it.
 */
void
start_lookup(void) {
	debug("start_lookup()");
	if (cancel_now)
		return;

	/*
	 * If there's a current lookup running, we really shouldn't get
	 * here.
	 */
	INSIST(current_lookup == NULL);

	current_lookup = ISC_LIST_HEAD(lookup_list);
	/*
	 * Put the current lookup somewhere so cancel_all can find it
	 */
	if (current_lookup != NULL) {
		ISC_LIST_DEQUEUE(lookup_list, current_lookup, link);
		setup_lookup(current_lookup);
		do_lookup(current_lookup);
	} else {
		check_if_done();
	}
}

/*
 * If we can, clear the current lookup and start the next one running.
 * This calls try_clear_lookup, so may invalidate the lookup pointer.
 */
static void
check_next_lookup(dig_lookup_t *lookup) {

	INSIST(!free_now);

	debug("check_next_lookup(%p)", lookup);

	if (ISC_LIST_HEAD(lookup->q) != NULL) {
		debug("still have a worker");
		return;
	}
	if (try_clear_lookup(lookup)) {
		current_lookup = NULL;
		start_lookup();
	}
}

/*
 * Create and queue a new lookup as a followup to the current lookup,
 * based on the supplied message and section.  This is used in trace and
 * name server search modes to start a new lookup using servers from
 * NS records in a reply.
 */
static void
followup_lookup(dns_message_t *msg, dig_query_t *query,
		dns_section_t section) {
	dig_lookup_t *lookup = NULL;
	dig_server_t *srv = NULL;
	dns_rdataset_t *rdataset = NULL;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dns_name_t *name = NULL;
	isc_result_t result, loopresult;
	isc_buffer_t *b = NULL;
	isc_region_t r;
	isc_boolean_t success = ISC_FALSE;
	int len;

	INSIST(!free_now);

	debug("followup_lookup()");
	result = dns_message_firstname(msg, section);

	if (result != ISC_R_SUCCESS) {
		debug("firstname returned %s",
			isc_result_totext(result));
		if ((section == DNS_SECTION_ANSWER) &&
		    (query->lookup->trace || query->lookup->ns_search_only))
			followup_lookup(msg, query, DNS_SECTION_AUTHORITY);
                return;
	}

	debug("following up %s", query->lookup->textname);

	for (;;) {
		name = NULL;
		dns_message_currentname(msg, section, &name);
		for (rdataset = ISC_LIST_HEAD(name->list);
		     rdataset != NULL;
		     rdataset = ISC_LIST_NEXT(rdataset, link)) {
			loopresult = dns_rdataset_first(rdataset);
			while (loopresult == ISC_R_SUCCESS) {
				dns_rdataset_current(rdataset, &rdata);
				debug("got rdata with type %d",
				       rdata.type);
				if ((rdata.type == dns_rdatatype_ns) &&
				    (!query->lookup->trace_root ||
				     (query->lookup->nsfound < MXSERV)))
				{
					query->lookup->nsfound++;
					result = isc_buffer_allocate(mctx, &b,
								     BUFSIZE);
					check_result(result,
						      "isc_buffer_allocate");
					result = dns_rdata_totext(&rdata,
								  NULL,
								  b);
					check_result(result,
						      "dns_rdata_totext");
					isc_buffer_usedregion(b, &r);
					len = r.length-1;
					if (len >= MXNAME)
						len = MXNAME-1;
				/* Initialize lookup if we've not yet */
					debug("found NS %d %.*s",
						 (int)r.length, (int)r.length,
						 (char *)r.base);
					if (!success) {
						success = ISC_TRUE;
						lookup_counter++;
						cancel_lookup(query->lookup);
						lookup = requeue_lookup
							(query->lookup,
							 ISC_FALSE);
						lookup->doing_xfr = ISC_FALSE;
						lookup->defname = ISC_FALSE;
						if (section ==
						    DNS_SECTION_ANSWER) {
						      lookup->trace =
								ISC_FALSE;
						      lookup->ns_search_only =
								ISC_FALSE;
						}
						else {
						      lookup->trace =
								query->
								lookup->trace;
						      lookup->ns_search_only =
							query->
							lookup->ns_search_only;
						}
						lookup->trace_root = ISC_FALSE;
					}
					r.base[len] = 0;
					srv = make_server((char *)r.base);
					debug("adding server %s",
					      srv->servername);
					ISC_LIST_APPEND
						(lookup->my_server_list,
						 srv, link);
					isc_buffer_free(&b);
				}
				dns_rdata_reset(&rdata);
				loopresult = dns_rdataset_next(rdataset);
			}
		}
		result = dns_message_nextname(msg, section);
		if (result != ISC_R_SUCCESS)
			break;
	}
	if ((lookup == NULL) && (section == DNS_SECTION_ANSWER) &&
	    (query->lookup->trace || query->lookup->ns_search_only))
		followup_lookup(msg, query, DNS_SECTION_AUTHORITY);
}

/*
 * Create and queue a new lookup using the next origin from the origin
 * list, read in setup_system().
 */
static isc_boolean_t
next_origin(dns_message_t *msg, dig_query_t *query) {
	dig_lookup_t *lookup;

	UNUSED(msg);

	INSIST(!free_now);

	debug("next_origin()");
	debug("following up %s", query->lookup->textname);

	if (fixedsearch == query->lookup->origin) {
		/*
		 * This is a fixed domain search; there is no next entry.
		 * While we're here, clear out the fixedsearch alloc.
		 */
		isc_mem_free(mctx, fixedsearch);
		fixedsearch = NULL;
		query->lookup->origin = NULL;
		return (ISC_FALSE);
	}
	if (!usesearch)
		/*
		 * We're not using a search list, so don't even think
		 * about finding the next entry.
		 */
		return (ISC_FALSE);
	if (query->lookup->origin == NULL)
		/*
		 * Then we just did rootorg; there's nothing left.
		 */
		return (ISC_FALSE);
	cancel_lookup(query->lookup);
	lookup = requeue_lookup(query->lookup, ISC_TRUE);
	lookup->defname = ISC_FALSE;
	lookup->origin = ISC_LIST_NEXT(query->lookup->origin, link);
	return (ISC_TRUE);
}

/*
 * Insert an SOA record into the sendmessage in a lookup.  Used for
 * creating IXFR queries.
 */
static void
insert_soa(dig_lookup_t *lookup) {
	isc_result_t result;
	dns_rdata_soa_t soa;
	dns_rdata_t *rdata = NULL;
	dns_rdatalist_t *rdatalist = NULL;
	dns_rdataset_t *rdataset = NULL;
	dns_name_t *soaname = NULL;

	debug("insert_soa()");
	soa.mctx = mctx;
	soa.serial = lookup->ixfr_serial;
	soa.refresh = 1;
	soa.retry = 1;
	soa.expire = 1;
	soa.minimum = 1;
	soa.common.rdclass = lookup->rdclass;
	soa.common.rdtype = dns_rdatatype_soa;

	dns_name_init(&soa.origin, NULL);
	dns_name_init(&soa.mname, NULL);

	dns_name_clone(lookup->name, &soa.origin);
	dns_name_clone(lookup->name, &soa.mname);

	isc_buffer_init(&lookup->rdatabuf, lookup->rdatastore,
			sizeof(lookup->rdatastore));

	result = dns_message_gettemprdata(lookup->sendmsg, &rdata);
	check_result(result, "dns_message_gettemprdata");

	result = dns_rdata_fromstruct(rdata, lookup->rdclass,
				      dns_rdatatype_soa, &soa,
				      &lookup->rdatabuf);
	check_result(result, "isc_rdata_fromstruct");

	result = dns_message_gettemprdatalist(lookup->sendmsg, &rdatalist);
	check_result(result, "dns_message_gettemprdatalist");

	result = dns_message_gettemprdataset(lookup->sendmsg, &rdataset);
	check_result(result, "dns_message_gettemprdataset");

	dns_rdatalist_init(rdatalist);
	rdatalist->type = dns_rdatatype_soa;
	rdatalist->rdclass = lookup->rdclass;
	rdatalist->covers = dns_rdatatype_soa;
	rdatalist->ttl = 1;
	ISC_LIST_INIT(rdatalist->rdata);
	ISC_LIST_APPEND(rdatalist->rdata, rdata, link);

	dns_rdataset_init(rdataset);
	dns_rdatalist_tordataset(rdatalist, rdataset);

	result = dns_message_gettempname(lookup->sendmsg, &soaname);
	check_result(result, "dns_message_gettempname");
	dns_name_init(soaname, NULL);
	dns_name_clone(lookup->name, soaname);
	ISC_LIST_INIT(soaname->list);
	ISC_LIST_APPEND(soaname->list, rdataset, link);
	dns_message_addname(lookup->sendmsg, soaname, DNS_SECTION_AUTHORITY);
}

/*
 * Setup the supplied lookup structure, making it ready to start sending
 * queries to servers.  Create and initialize the message to be sent as
 * well as the query structures and buffer space for the replies.  If the
 * server list is empty, clone it from the system default list.
 */
void
setup_lookup(dig_lookup_t *lookup) {
	isc_result_t result;
	int len;
	dig_server_t *serv;
	dig_query_t *query;
	isc_region_t r;
	isc_buffer_t b;
	char store[MXNAME];

	REQUIRE(lookup != NULL);
	INSIST(!free_now);

	debug("setup_lookup(%p)", lookup);

	result = dns_message_create(mctx, DNS_MESSAGE_INTENTRENDER,
				    &lookup->sendmsg);
	check_result(result, "dns_message_create");

	if (lookup->new_search) {
		debug("resetting lookup counter.");
		lookup_counter = 0;
	}

	if (ISC_LIST_EMPTY(lookup->my_server_list)) {
		debug("cloning server list");
		clone_server_list(server_list, &lookup->my_server_list);
	}
	result = dns_message_gettempname(lookup->sendmsg, &lookup->name);
	check_result(result, "dns_message_gettempname");
	dns_name_init(lookup->name, NULL);

	isc_buffer_init(&lookup->namebuf, lookup->namespace,
			sizeof(lookup->namespace));
	isc_buffer_init(&lookup->onamebuf, lookup->onamespace,
			sizeof(lookup->onamespace));

	/*
	 * If the name has too many dots, force the origin to be NULL
	 * (which produces an absolute lookup).  Otherwise, take the origin
	 * we have if there's one in the struct already.  If it's NULL,
	 * take the first entry in the searchlist iff either usesearch
	 * is TRUE or we got a domain line in the resolv.conf file.
	 */
	/* XXX New search here? */
	if ((count_dots(lookup->textname) >= ndots) || lookup->defname)
		lookup->origin = NULL; /* Force abs lookup */
	else if (lookup->origin == NULL && lookup->new_search &&
		 (usesearch || have_domain)) {
		if (fixeddomain[0] != 0) {
			debug("using fixed domain %s", fixeddomain);
			if (fixedsearch != NULL)
				isc_mem_free(mctx, fixedsearch);
			fixedsearch = isc_mem_allocate(mctx,
						sizeof(struct dig_server));
			if (fixedsearch == NULL)
				fatal("Memory allocation failure in %s:%d",
				      __FILE__, __LINE__);
			strncpy(fixedsearch->origin, fixeddomain,
				sizeof(fixedsearch->origin));
			fixedsearch->origin[sizeof(fixedsearch->origin)-1]=0;
			lookup->origin = fixedsearch;
		} else
			lookup->origin = ISC_LIST_HEAD(search_list);
	}
	if (lookup->origin != NULL) {
		debug("trying origin %s", lookup->origin->origin);
		result = dns_message_gettempname(lookup->sendmsg,
						 &lookup->oname);
		check_result(result, "dns_message_gettempname");
		dns_name_init(lookup->oname, NULL);
		/* XXX Helper funct to conv char* to name? */
		len = strlen(lookup->origin->origin);
		isc_buffer_init(&b, lookup->origin->origin, len);
		isc_buffer_add(&b, len);
		result = dns_name_fromtext(lookup->oname, &b, dns_rootname,
					   ISC_FALSE, &lookup->onamebuf);
		if (result != ISC_R_SUCCESS) {
			dns_message_puttempname(lookup->sendmsg,
						&lookup->name);
			dns_message_puttempname(lookup->sendmsg,
						&lookup->oname);
			fatal("'%s' is not in legal name syntax (%s)",
			      lookup->origin->origin,
			      dns_result_totext(result));
		}
		if (lookup->trace_root) {
			dns_name_clone(dns_rootname, lookup->name);
		} else {
			len = strlen(lookup->textname);
			isc_buffer_init(&b, lookup->textname, len);
			isc_buffer_add(&b, len);
			result = dns_name_fromtext(lookup->name, &b,
						   lookup->oname, ISC_FALSE,
						   &lookup->namebuf);
		}
		if (result != ISC_R_SUCCESS) {
			dns_message_puttempname(lookup->sendmsg,
						&lookup->name);
			dns_message_puttempname(lookup->sendmsg,
						&lookup->oname);
			fatal("'%s' is not in legal name syntax (%s)",
			      lookup->textname, dns_result_totext(result));
		}
		dns_message_puttempname(lookup->sendmsg, &lookup->oname);
	} else {
		debug("using root origin");
		if (!lookup->trace_root) {
			len = strlen(lookup->textname);
			isc_buffer_init(&b, lookup->textname, len);
			isc_buffer_add(&b, len);
			result = dns_name_fromtext(lookup->name, &b,
						   dns_rootname,
						   ISC_FALSE,
						   &lookup->namebuf);
		} else {
			dns_name_clone(dns_rootname, lookup->name);
		}
		if (result != ISC_R_SUCCESS) {
			dns_message_puttempname(lookup->sendmsg,
						&lookup->name);
			isc_buffer_init(&b, store, MXNAME);
			fatal("'%s' is not a legal name syntax "
			      "(%s)", lookup->textname,
			      dns_result_totext(result));
		}
	}
	isc_buffer_init(&b, store, sizeof(store));
	/* XXX Move some of this into function, dns_name_format. */
	dns_name_totext(lookup->name, ISC_FALSE, &b);
	isc_buffer_usedregion(&b, &r);
	trying((int)r.length, (char *)r.base, lookup);
	INSIST(dns_name_isabsolute(lookup->name));

	lookup->sendmsg->id = (unsigned short)(random() & 0xFFFF);
	lookup->sendmsg->opcode = dns_opcode_query;
	lookup->msgcounter = 0;
	/*
	 * If this is a trace request, completely disallow recursion, since
	 * it's meaningless for traces.
	 */
	if (lookup->recurse && !lookup->trace && !lookup->ns_search_only) {
		debug("recursive query");
		lookup->sendmsg->flags |= DNS_MESSAGEFLAG_RD;
	}

	/* XXX aaflag */
	if (lookup->aaonly) {
		debug("AA query");
		lookup->sendmsg->flags |= DNS_MESSAGEFLAG_AA;
	}

	if (lookup->adflag) {
		debug("AD query");
		lookup->sendmsg->flags |= DNS_MESSAGEFLAG_AD;
	}

	if (lookup->cdflag) {
		debug("CD query");
		lookup->sendmsg->flags |= DNS_MESSAGEFLAG_CD;
	}

	dns_message_addname(lookup->sendmsg, lookup->name,
			    DNS_SECTION_QUESTION);

	if (lookup->trace_root)
		lookup->rdtype = dns_rdatatype_soa;

	if ((lookup->rdtype == dns_rdatatype_axfr) ||
	    (lookup->rdtype == dns_rdatatype_ixfr)) {
		lookup->doing_xfr = ISC_TRUE;
		/*
		 * Force TCP mode if we're doing an xfr.
		 * XXX UDP ixfr's would be useful
		 */
		lookup->tcp_mode = ISC_TRUE;
	}
	add_question(lookup->sendmsg, lookup->name, lookup->rdclass,
		     lookup->rdtype);

	/* XXX add_soa */
	if (lookup->rdtype == dns_rdatatype_ixfr)
		insert_soa(lookup);

	/* XXX Insist this? */
	lookup->tsigctx = NULL;
	lookup->querysig = NULL;
	if (key != NULL) {
		debug("initializing keys");
		result = dns_message_settsigkey(lookup->sendmsg, key);
		check_result(result, "dns_message_settsigkey");
	}

	lookup->sendspace = isc_mempool_get(commctx);
	if (lookup->sendspace == NULL)
		fatal("memory allocation failure");

	debug("starting to render the message");
	isc_buffer_init(&lookup->sendbuf, lookup->sendspace, COMMSIZE);
	result = dns_message_renderbegin(lookup->sendmsg, &lookup->sendbuf);
	check_result(result, "dns_message_renderbegin");
#ifndef DNS_OPT_NEWCODES_LIVE
	if (lookup->udpsize > 0 || lookup->dnssec) {
#else /* DNS_OPT_NEWCODES_LIVE */
	if (lookup->udpsize > 0 ||  || lookup->dnssec ||
	    lookup->zonename[0] !=0 || lookup->viewname[0] != 0) {
		dns_fixedname_t fname;
		isc_buffer_t namebuf, *wirebuf = NULL;
		dns_compress_t cctx;
#endif /* DNS_OPT_NEWCODES_LIVE */
		dns_optlist_t optlist;
		dns_optattr_t optattr[2];

		if (lookup->udpsize == 0)
			lookup->udpsize = 2048;
		optlist.size = 2;
		optlist.used = 0;
		optlist.next = 0;
		optlist.attrs = optattr;

#ifdef DNS_OPT_NEWCODES_LIVE
		if (lookup->zonename[0] != 0) {
			optattr[optlist.used].code = DNS_OPTCODE_ZONE;
			dns_fixedname_init(&fname);
			isc_buffer_init(&namebuf, lookup->zonename,
					strlen(lookup->zonename));
			isc_buffer_add(&namebuf, strlen(lookup->zonename));
			result = dns_name_fromtext(&(fname.name), &namebuf,
						   dns_rootname, ISC_FALSE,
						   NULL);
			check_result(result, "; illegal zone option");
			result = dns_compress_init(&cctx, 0, mctx);
			check_result(result, "dns_compress_init");
			result = isc_buffer_allocate(mctx, &wirebuf,
						     MXNAME);
			check_result(result, "isc_buffer_allocate");
			result = dns_name_towire(&(fname.name), &cctx,
						 wirebuf);
			check_result(result, "dns_name_towire");
			optattr[optlist.used].value.base =
				isc_buffer_base(wirebuf);
			optattr[optlist.used].value.length =
			        isc_buffer_usedlength(wirebuf);
			optlist.used++;
			dns_compress_invalidate(&cctx);
		}
		if (lookup->viewname[0] != 0) {
			optattr[optlist.used].code = DNS_OPTCODE_VIEW;
			optattr[optlist.used].value.base =
				lookup->viewname;
			optattr[optlist.used].value.length =
				strlen(lookup->viewname);
			optlist.used++;
		}
#endif /* DNS_OPT_NEWCODES_LIVE */
		add_opt(lookup->sendmsg, lookup->udpsize, lookup->dnssec,
			optlist);
#ifdef DNS_OPT_NEWCODES_LIVE
		if (wirebuf != NULL)
			isc_buffer_free(&wirebuf);
#endif /* DNS_OPT_NEWCODES_LIVE */
	}

	result = dns_message_rendersection(lookup->sendmsg,
					   DNS_SECTION_QUESTION, 0);
	check_result(result, "dns_message_rendersection");
	result = dns_message_rendersection(lookup->sendmsg,
					   DNS_SECTION_AUTHORITY, 0);
	check_result(result, "dns_message_rendersection");
	result = dns_message_renderend(lookup->sendmsg);
	check_result(result, "dns_message_renderend");
	debug("done rendering");

	lookup->pending = ISC_FALSE;

	for (serv = ISC_LIST_HEAD(lookup->my_server_list);
	     serv != NULL;
	     serv = ISC_LIST_NEXT(serv, link)) {
		query = isc_mem_allocate(mctx, sizeof(dig_query_t));
		if (query == NULL)
			fatal("Memory allocation failure in %s:%d",
			      __FILE__, __LINE__);
		debug("create query %p linked to lookup %p",
		       query, lookup);
		query->lookup = lookup;
		query->waiting_connect = ISC_FALSE;
		query->recv_made = ISC_FALSE;
		query->first_pass = ISC_TRUE;
		query->first_soa_rcvd = ISC_FALSE;
		query->second_rr_rcvd = ISC_FALSE;
		query->second_rr_serial = 0;
		query->servname = serv->servername;
		query->rr_count = 0;
		ISC_LINK_INIT(query, link);
		ISC_LIST_INIT(query->recvlist);
		ISC_LIST_INIT(query->lengthlist);
		query->sock = NULL;
		query->recvspace = isc_mempool_get(commctx);
		if (query->recvspace == NULL)
			fatal("memory allocation failure");

		isc_buffer_init(&query->recvbuf, query->recvspace, COMMSIZE);
		isc_buffer_init(&query->lengthbuf, query->lengthspace, 2);
		isc_buffer_init(&query->slbuf, query->slspace, 2);

		ISC_LINK_INIT(query, link);
		ISC_LIST_ENQUEUE(lookup->q, query, link);
	}
	/* XXX qrflag, print_query, etc... */
	if (!ISC_LIST_EMPTY(lookup->q) && qr) {
		printmessage(ISC_LIST_HEAD(lookup->q), lookup->sendmsg,
			     ISC_TRUE);
	}
}

/*
 * Event handler for send completion.  Track send counter, and clear out
 * the query if the send was canceled.
 */
static void
send_done(isc_task_t *_task, isc_event_t *event) {
	REQUIRE(event->ev_type == ISC_SOCKEVENT_SENDDONE);

	UNUSED(_task);

	LOCK_LOOKUP;

	isc_event_free(&event);

	debug("send_done()");
	sendcount--;
	debug("sendcount=%d", sendcount);
	INSIST(sendcount >= 0);
	check_if_done();
	UNLOCK_LOOKUP;
}

/*
 * Cancel a lookup, sending isc_socket_cancel() requests to all outstanding
 * IO sockets.  The cancel handlers should take care of cleaning up the
 * query and lookup structures
 */
static void
cancel_lookup(dig_lookup_t *lookup) {
	dig_query_t *query, *next;

	debug("cancel_lookup()");
	query = ISC_LIST_HEAD(lookup->q);
	while (query != NULL) {
		next = ISC_LIST_NEXT(query, link);
		if (query->sock != NULL) {
			isc_socket_cancel(query->sock, global_task,
					  ISC_SOCKCANCEL_ALL);
			check_if_done();
		} else {
			clear_query(query);
		}
		query = next;
	}
	if (lookup->timer != NULL)
		isc_timer_detach(&lookup->timer);
	lookup->pending = ISC_FALSE;
	lookup->retries = 0;
}

static void
bringup_timer(dig_query_t *query, unsigned int default_timeout) {
	dig_lookup_t *l;
	unsigned int local_timeout;
	isc_result_t result;

	debug("bringup_timer()");
	/*
	 * If the timer already exists, that means we're calling this
	 * a second time (for a retry).  Don't need to recreate it,
	 * just reset it.
	 */
	l = query->lookup;
	if (ISC_LIST_NEXT(query, link) != NULL)
		local_timeout = SERVER_TIMEOUT;
	else {
		if (timeout == 0) {
			local_timeout = default_timeout;
		} else
			local_timeout = timeout;
	}
	debug("have local timeout of %d", local_timeout);
	isc_interval_set(&l->interval, local_timeout, 0);
	if (l->timer != NULL)
		isc_timer_detach(&l->timer);
	result = isc_timer_create(timermgr,
				  isc_timertype_once,
				  NULL,
				  &l->interval,
				  global_task,
				  connect_timeout,
				  l, &l->timer);
	check_result(result, "isc_timer_create");
}	

static void
connect_done(isc_task_t *task, isc_event_t *event);

/*
 * Unlike send_udp, this can't be called multiple times with the same
 * query.  When we retry TCP, we requeue the whole lookup, which should
 * start anew.
 */
static void
send_tcp_connect(dig_query_t *query) {
	isc_result_t result;
	dig_query_t *next;
	dig_lookup_t *l;

	debug("send_tcp_connect(%lx)", query);

	l = query->lookup;
	query->waiting_connect = ISC_TRUE;
	query->lookup->current_query = query;
	get_address(query->servname, port, &query->sockaddr);
	
	if (specified_source &&
	    (isc_sockaddr_pf(&query->sockaddr) !=
	     isc_sockaddr_pf(&bind_address))) {
		printf(";; Skipping server %s, incompatible "
		       "address family\n", query->servname);
		query->waiting_connect = ISC_FALSE;
		next = ISC_LIST_NEXT(query, link);
		l = query->lookup;
		clear_query(query);
		if (next == NULL) {
			printf(";; No acceptable nameservers\n");
			check_next_lookup(l);
			return;
		}
		send_tcp_connect(next);
		return;
	}
	INSIST(query->sock == NULL);
	result = isc_socket_create(socketmgr,
				   isc_sockaddr_pf(&query->sockaddr),
				   isc_sockettype_tcp, &query->sock) ;
	check_result(result, "isc_socket_create");
	sockcount++;
	debug("sockcount=%d", sockcount);
	if (specified_source)
		result = isc_socket_bind(query->sock, &bind_address);
	else {
		if ((isc_sockaddr_pf(&query->sockaddr) == AF_INET) &&
		    have_ipv4)
			isc_sockaddr_any(&bind_any);
		else
			isc_sockaddr_any6(&bind_any);
		result = isc_socket_bind(query->sock, &bind_any);
	}
	check_result(result, "isc_socket_bind");
	bringup_timer(query, TCP_TIMEOUT);
	result = isc_socket_connect(query->sock, &query->sockaddr,
				    global_task, connect_done, query);
	check_result(result, "isc_socket_connect");
	/*
	 * If we're doing a nameserver search, we need to immediately
	 * bring up all the queries.  Do it here.
	 */
	if (l->ns_search_only) {
		debug("sending next, since searching");
		next = ISC_LIST_NEXT(query, link);
		if (next != NULL)
			send_tcp_connect(next);
	}
}

/*
 * Send a UDP packet to the remote nameserver, possible starting the
 * recv action as well.  Also make sure that the timer is running and
 * is properly reset.
 */
static void
send_udp(dig_query_t *query) {
	dig_lookup_t *l = NULL;
	dig_query_t *next;
	isc_result_t result;

	debug("send_udp(%lx)", query);

	l = query->lookup;
	bringup_timer(query, UDP_TIMEOUT);
	l->current_query = query;
	debug("working on lookup %p, query %p",
	      query->lookup, query);
	if (!query->recv_made) {
		/* XXX Check the sense of this, need assertion? */
		query->waiting_connect = ISC_FALSE;
		get_address(query->servname, port, &query->sockaddr);

		result = isc_socket_create(socketmgr,
					   isc_sockaddr_pf(&query->sockaddr),
					   isc_sockettype_udp, &query->sock);
		check_result(result, "isc_socket_create");
		sockcount++;
		debug("sockcount=%d", sockcount);
		if (specified_source) {
			result = isc_socket_bind(query->sock, &bind_address);
		} else {
			isc_sockaddr_anyofpf(&bind_any,
					isc_sockaddr_pf(&query->sockaddr));
			result = isc_socket_bind(query->sock, &bind_any);
		}
		check_result(result, "isc_socket_bind");

		query->recv_made = ISC_TRUE;
		ISC_LINK_INIT(&query->recvbuf, link);
		ISC_LIST_ENQUEUE(query->recvlist, &query->recvbuf,
				 link);
		debug("recving with lookup=%p, query=%p, sock=%p",
		      query->lookup, query,
		      query->sock);
		result = isc_socket_recvv(query->sock,
					  &query->recvlist, 1,
					  global_task, recv_done,
					  query);
		check_result(result, "isc_socket_recvv");
		recvcount++;
		debug("recvcount=%d", recvcount);
	}
	ISC_LIST_INIT(query->sendlist);
	ISC_LINK_INIT(&l->sendbuf, link);
	ISC_LIST_ENQUEUE(query->sendlist, &l->sendbuf,
			 link);
	debug("sending a request");
	result = isc_time_now(&query->time_sent);
	check_result(result, "isc_time_now");
	INSIST(query->sock != NULL);
	result = isc_socket_sendtov(query->sock, &query->sendlist,
				    global_task, send_done, query,
				    &query->sockaddr, NULL);
	check_result(result, "isc_socket_sendtov");
	sendcount++;
	/*
	 * If we're doing a nameserver search, we need to immediately
	 * bring up all the queries.  Do it here.
	 */
	if (l->ns_search_only) {
		debug("sending next, since searching");
		next = ISC_LIST_NEXT(query, link);
		if (next != NULL)
			send_udp(next);
	}
}

/*
 * IO timeout handler, used for both connect and recv timeouts.  If
 * retries are still allowed, either resend the UDP packet or queue a
 * new TCP lookup.  Otherwise, cancel the lookup.
 */
static void
connect_timeout(isc_task_t *task, isc_event_t *event) {
	dig_lookup_t *l=NULL;
	dig_query_t *query=NULL, *cq;

	UNUSED(task);
	REQUIRE(event->ev_type == ISC_TIMEREVENT_IDLE);

	debug("connect_timeout()");

	LOCK_LOOKUP;
	l = event->ev_arg;
	query = l->current_query;
	isc_event_free(&event);

	INSIST(!free_now);

	if ((query != NULL) && (query->lookup->current_query != NULL) &&
	    (ISC_LIST_NEXT(query->lookup->current_query, link) != NULL)) {
		debug("trying next server...");
		cq = query->lookup->current_query;
		if (!l->tcp_mode)
			send_udp(ISC_LIST_NEXT(cq, link));
		else
			send_tcp_connect(ISC_LIST_NEXT(cq, link));
		UNLOCK_LOOKUP;
		return;
	}

	if (l->retries > 1) {
		if (!l->tcp_mode) {
			l->retries--;
			debug("resending UDP request to first server");
			send_udp(ISC_LIST_HEAD(l->q));
		} else {
			debug("making new TCP request, %d tries left",
			      l->retries);
			cancel_lookup(l);
			l->retries--;
			requeue_lookup(l, ISC_TRUE);
		}
	}
	else {
		fputs(l->cmdline, stdout);
		printf(";; connection timed out; no servers could be "
		       "reached\n");
		cancel_lookup(l);
	}
	UNLOCK_LOOKUP;
}

/*
 * Event handler for the TCP recv which gets the length header of TCP
 * packets.  Start the next recv of length bytes.
 */
static void
tcp_length_done(isc_task_t *task, isc_event_t *event) {
	isc_socketevent_t *sevent;
	isc_buffer_t *b=NULL;
	isc_region_t r;
	isc_result_t result;
	dig_query_t *query=NULL;
	dig_lookup_t *l;
	isc_uint16_t length;

	REQUIRE(event->ev_type == ISC_SOCKEVENT_RECVDONE);
	INSIST(!free_now);

	UNUSED(task);

	debug("tcp_length_done()");

	LOCK_LOOKUP;
	sevent = (isc_socketevent_t *)event;
	query = event->ev_arg;

	recvcount--;
	INSIST(recvcount >= 0);

	if (sevent->result == ISC_R_CANCELED) {
		isc_event_free(&event);
		l = query->lookup;
		clear_query(query);
		check_next_lookup(l);
		UNLOCK_LOOKUP;
		return;
	}
	if (sevent->result != ISC_R_SUCCESS) {
		result = isc_buffer_allocate(mctx, &b, 256);
		check_result(result, "isc_buffer_allocate");
		result = isc_sockaddr_totext(&query->sockaddr, b);
		check_result(result, "isc_sockaddr_totext");
		isc_buffer_usedregion(b, &r);
		printf(";; communications error to %.*s: %s\n",
		       (int)r.length, r.base,
		       isc_result_totext(sevent->result));
		isc_buffer_free(&b);
		l = query->lookup;
		isc_socket_detach(&query->sock);
		sockcount--;
		debug("sockcount=%d", sockcount);
		INSIST(sockcount >= 0);
		isc_event_free(&event);
		clear_query(query);
		check_next_lookup(l);
		UNLOCK_LOOKUP;
		return;
	}
	b = ISC_LIST_HEAD(sevent->bufferlist);
	ISC_LIST_DEQUEUE(sevent->bufferlist, &query->lengthbuf, link);
	length = isc_buffer_getuint16(b);
	if (length > COMMSIZE) {
		isc_event_free(&event);
		fatal("Length of %X was longer than I can handle!",
		      length);
	}
	/*
	 * Even though the buffer was already init'ed, we need
	 * to redo it now, to force the length we want.
	 */
	isc_buffer_invalidate(&query->recvbuf);
	isc_buffer_init(&query->recvbuf, query->recvspace, length);
	ENSURE(ISC_LIST_EMPTY(query->recvlist));
	ISC_LINK_INIT(&query->recvbuf, link);
	ISC_LIST_ENQUEUE(query->recvlist, &query->recvbuf, link);
	debug("recving with lookup=%p, query=%p",
	       query->lookup, query);
	result = isc_socket_recvv(query->sock, &query->recvlist, length, task,
				  recv_done, query);
	check_result(result, "isc_socket_recvv");
	recvcount++;
	debug("resubmitted recv request with length %d, recvcount=%d",
	      length, recvcount);
	isc_event_free(&event);
	UNLOCK_LOOKUP;
}

/*
 * For transfers that involve multiple recvs (XFR's in particular),
 * launch the next recv.
 */
static void
launch_next_query(dig_query_t *query, isc_boolean_t include_question) {
	isc_result_t result;
	dig_lookup_t *l;

	INSIST(!free_now);

	debug("launch_next_query()");

	if (!query->lookup->pending) {
		debug("ignoring launch_next_query because !pending");
		isc_socket_detach(&query->sock);
		sockcount--;
		debug("sockcount=%d", sockcount);
		INSIST(sockcount >= 0);
		query->waiting_connect = ISC_FALSE;
		l = query->lookup;
		clear_query(query);
		check_next_lookup(l);
		return;
	}

	isc_buffer_clear(&query->slbuf);
	isc_buffer_clear(&query->lengthbuf);
	isc_buffer_putuint16(&query->slbuf, query->lookup->sendbuf.used);
	ISC_LIST_INIT(query->sendlist);
	ISC_LINK_INIT(&query->slbuf, link);
	ISC_LIST_ENQUEUE(query->sendlist, &query->slbuf, link);
	if (include_question) {
		ISC_LINK_INIT(&query->lookup->sendbuf, link);
		ISC_LIST_ENQUEUE(query->sendlist, &query->lookup->sendbuf,
				 link);
	}
	ISC_LINK_INIT(&query->lengthbuf, link);
	ISC_LIST_ENQUEUE(query->lengthlist, &query->lengthbuf, link);

	result = isc_socket_recvv(query->sock, &query->lengthlist, 0,
				  global_task, tcp_length_done, query);
	check_result(result, "isc_socket_recvv");
	recvcount++;
	debug("recvcount=%d",recvcount);
	if (!query->first_soa_rcvd) {
		debug("sending a request in launch_next_query");
		result = isc_time_now(&query->time_sent);
		check_result(result, "isc_time_now");
		result = isc_socket_sendv(query->sock, &query->sendlist,
					  global_task, send_done, query);
		check_result(result, "isc_socket_sendv");
		sendcount++;
		debug("sendcount=%d", sendcount);
	}
	query->waiting_connect = ISC_FALSE;
#if 0
	check_next_lookup(query->lookup);
#endif
	return;
}

/*
 * Event handler for TCP connect complete.  Make sure the connection was
 * successful, then pass into launch_next_query to actually send the
 * question.
 */
static void
connect_done(isc_task_t *task, isc_event_t *event) {
	isc_result_t result;
	isc_socketevent_t *sevent = NULL;
	dig_query_t *query = NULL, *next;
	dig_lookup_t *l;
	isc_buffer_t *b = NULL;
	isc_region_t r;

	UNUSED(task);

	REQUIRE(event->ev_type == ISC_SOCKEVENT_CONNECT);
	INSIST(!free_now);

	debug("connect_done()");

	LOCK_LOOKUP;
	sevent = (isc_socketevent_t *)event;
	query = sevent->ev_arg;

	INSIST(query->waiting_connect);

	query->waiting_connect = ISC_FALSE;

	if (sevent->result == ISC_R_CANCELED) {
		debug("in cancel handler");
		isc_socket_detach(&query->sock);
		sockcount--;
		INSIST(sockcount >= 0);
		debug("sockcount=%d", sockcount);
		query->waiting_connect = ISC_FALSE;
		isc_event_free(&event);
		l = query->lookup;
		clear_query(query);
		check_next_lookup(l);
		UNLOCK_LOOKUP;
		return;
	}
	if (sevent->result != ISC_R_SUCCESS) {
		debug("unsuccessful connection: %s",
		      isc_result_totext(sevent->result));
		result = isc_buffer_allocate(mctx, &b, 256);
		check_result(result, "isc_buffer_allocate");
		result = isc_sockaddr_totext(&query->sockaddr, b);
		check_result(result, "isc_sockaddr_totext");
		isc_buffer_usedregion(b, &r);
		/* XXX isc_sockaddr_format */
		if (sevent->result != ISC_R_CANCELED)
			printf(";; Connection to %.*s(%s) for %s failed: "
			       "%s.\n", (int)r.length, r.base,
			       query->servname, query->lookup->textname,
			       isc_result_totext(sevent->result));
		isc_socket_detach(&query->sock);
		sockcount--;
		INSIST(sockcount >= 0);
		/* XXX Clean up exitcodes */
		if (exitcode < 9)
			exitcode = 9;
		debug("sockcount=%d", sockcount);
		isc_buffer_free(&b);
		query->waiting_connect = ISC_FALSE;
		isc_event_free(&event);
		l = query->lookup;
		if (l->current_query != NULL)
			next = ISC_LIST_NEXT(l->current_query, link);
		else
			next = NULL;
		clear_query(query);
		if (next != NULL) {
			bringup_timer(next, TCP_TIMEOUT);
			send_tcp_connect(next);
		} else {
			check_next_lookup(l);
		}
		UNLOCK_LOOKUP;
		return;
	}
	launch_next_query(query, ISC_TRUE);
	isc_event_free(&event);
	UNLOCK_LOOKUP;
}

/*
 * Check if the ongoing XFR needs more data before it's complete, using
 * the semantics of IXFR and AXFR protocols.  Much of the complexity of
 * this routine comes from determining when an IXFR is complete.
 * ISC_FALSE means more data is on the way, and the recv has been issued.
 */
static isc_boolean_t
check_for_more_data(dig_query_t *query, dns_message_t *msg,
		    isc_socketevent_t *sevent)
{
	dns_rdataset_t *rdataset = NULL;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dns_rdata_soa_t soa;
	isc_result_t result;
	isc_buffer_t b;
	isc_region_t r;
	char abspace[MXNAME];
	isc_boolean_t atlimit=ISC_FALSE;

	debug("check_for_more_data()");

	/*
	 * By the time we're in this routine, we know we're doing
	 * either an AXFR or IXFR.  If there's no second_rr_type,
	 * then we don't yet know which kind of answer we got back
	 * from the server.  Here, we're going to walk through the
	 * rr's in the message, acting as necessary whenever we hit
	 * an SOA rr.
	 */

	result = dns_message_firstname(msg, DNS_SECTION_ANSWER);
	if (result != ISC_R_SUCCESS) {
		puts("; Transfer failed.");
		return (ISC_TRUE);
	}
	do {
		dns_name_t *name;
		name = NULL;
		dns_message_currentname(msg, DNS_SECTION_ANSWER,
					&name);
		for (rdataset = ISC_LIST_HEAD(name->list);
		     rdataset != NULL;
		     rdataset = ISC_LIST_NEXT(rdataset, link)) {
			result = dns_rdataset_first(rdataset);
			if (result != ISC_R_SUCCESS)
				continue;
			do {
				query->rr_count++;
				if (query->rr_count >= rr_limit)
					atlimit = ISC_TRUE;
				dns_rdata_reset(&rdata);
				dns_rdataset_current(rdataset, &rdata);
				/*
				 * If this is the first rr, make sure
				 * it's an SOA
				 */
				if ((!query->first_soa_rcvd) &&
				    (rdata.type != dns_rdatatype_soa)) {
					puts("; Transfer failed.  "
					     "Didn't start with "
					     "SOA answer.");
					return (ISC_TRUE);
				}
				if ((!query->second_rr_rcvd) &&
				    (rdata.type != dns_rdatatype_soa)) {
					query->second_rr_rcvd = ISC_TRUE;
					query->second_rr_serial = 0;
					debug("got the second rr as nonsoa");
					continue;
				}

				/*
				 * If the record is anything except an SOA
				 * now, just continue on...
				 */
				if (rdata.type != dns_rdatatype_soa)
					goto next_rdata;
				/* Now we have an SOA.  Work with it. */
				debug("got an SOA");
				result = dns_rdata_tostruct(&rdata,
							    &soa,
							    mctx);
				check_result(result,
					     "dns_rdata_tostruct");
				if (!query->first_soa_rcvd) {
					query->first_soa_rcvd =
						ISC_TRUE;
					query->first_rr_serial =
						soa.serial;
					debug("this is the first %d",
					       query->lookup->ixfr_serial);
					if (query->lookup->ixfr_serial >=
					    soa.serial) {
						dns_rdata_freestruct(&soa);
						goto doexit;
					}
					dns_rdata_freestruct(&soa);
					goto next_rdata;
				}
				if (query->lookup->rdtype ==
				    dns_rdatatype_axfr) {
					debug("doing axfr, got second SOA");
					dns_rdata_freestruct(&soa);
					goto doexit;
				}
				if (!query->second_rr_rcvd) {
					if (soa.serial ==
					    query->first_rr_serial) {
						debug("doing ixfr, got "
						      "empty zone");
						dns_rdata_freestruct(&soa);
						goto doexit;
					}
					debug("this is the second %d",
					       query->lookup->ixfr_serial);
					query->second_rr_rcvd = ISC_TRUE;
					query->second_rr_serial =
						soa.serial;
					dns_rdata_freestruct(&soa);
					goto next_rdata;
				}
				if (query->second_rr_serial == 0) {
					/*
					 * If the second RR was a non-SOA
					 * record, and we're getting any
					 * other SOA, then this is an
					 * AXFR, and we're done.
					 */
					debug("done, since axfr");
					dns_rdata_freestruct(&soa);
					goto doexit;
				}
				/*
				 * If we get to this point, we're doing an
				 * IXFR and have to start really looking
				 * at serial numbers.
				 */
				if (query->first_rr_serial == soa.serial) {
					debug("got a match for ixfr");
					if (!query->first_repeat_rcvd) {
						query->first_repeat_rcvd =
							ISC_TRUE;
						dns_rdata_freestruct(&soa);
						goto next_rdata;
					}
					debug("done with ixfr");
					dns_rdata_freestruct(&soa);
					goto doexit;
				}
				debug("meaningless soa %d",
				       soa.serial);
				dns_rdata_freestruct(&soa);
			next_rdata:
				result = dns_rdataset_next(rdataset);
			} while (result == ISC_R_SUCCESS);
		}
		result = dns_message_nextname(msg, DNS_SECTION_ANSWER);
	} while (result == ISC_R_SUCCESS);
	if (atlimit) {
	doexit:
		isc_buffer_init(&b, abspace, MXNAME);
		result = isc_sockaddr_totext(&sevent->address, &b);
		check_result(result,
			     "isc_sockaddr_totext");
		isc_buffer_usedregion(&b, &r);
		received(b.used, r.length,
			 (char *)r.base, query);
		if (atlimit)
			if (exitcode < 7)
				exitcode = 7;
		return (ISC_TRUE);
	}
	launch_next_query(query, ISC_FALSE);
	return (ISC_FALSE);
}

/*
 * Event handler for recv complete.  Perform whatever actions are necessary,
 * based on the specifics of the user's request.
 */
static void
recv_done(isc_task_t *task, isc_event_t *event) {
	isc_socketevent_t *sevent = NULL;
	dig_query_t *query = NULL;
	isc_buffer_t *b = NULL;
	dns_message_t *msg = NULL;
	isc_result_t result;
	isc_buffer_t ab;
	char abspace[MXNAME];
	isc_region_t r;
	dig_lookup_t *n, *l;
	isc_boolean_t docancel = ISC_FALSE;
	unsigned int local_timeout;

	UNUSED(task);
	INSIST(!free_now);

	debug("recv_done()");

	LOCK_LOOKUP;
	recvcount--;
	debug("recvcount=%d", recvcount);
	INSIST(recvcount >= 0);

	query = event->ev_arg;
	debug("lookup=%p, query=%p", query->lookup, query);

	l = query->lookup;

	REQUIRE(event->ev_type == ISC_SOCKEVENT_RECVDONE);
	sevent = (isc_socketevent_t *)event;

	if ((l->tcp_mode) && (l->timer != NULL))
		isc_timer_touch(l->timer);
	if ((!l->pending && !l->ns_search_only)
	    || cancel_now) {
		debug("no longer pending.  Got %s",
			isc_result_totext(sevent->result));
		query->waiting_connect = ISC_FALSE;

		isc_event_free(&event);
		clear_query(query);
		check_next_lookup(l);
		UNLOCK_LOOKUP;
		return;
	}

	if (sevent->result == ISC_R_SUCCESS) {
		b = ISC_LIST_HEAD(sevent->bufferlist);
		ISC_LIST_DEQUEUE(sevent->bufferlist, &query->recvbuf, link);
		result = dns_message_create(mctx, DNS_MESSAGE_INTENTPARSE,
					    &msg);
		check_result(result, "dns_message_create");

		if (key != NULL) {
			if (l->querysig == NULL) {
				debug("getting initial querysig");
				result = dns_message_getquerytsig(
					     l->sendmsg,
					     mctx, &l->querysig);
				check_result(result,
					     "dns_message_getquerytsig");
			}
			result = dns_message_setquerytsig(msg,
						 l->querysig);
			check_result(result, "dns_message_setquerytsig");
			result = dns_message_settsigkey(msg, key);
			check_result(result, "dns_message_settsigkey");
			msg->tsigctx = l->tsigctx;
			if (l->msgcounter != 0)
				msg->tcp_continuation = 1;
			l->msgcounter++;
		}
		debug("before parse starts");
		if (l->besteffort)
			result = dns_message_parse(msg, b,
					   DNS_MESSAGEPARSE_PRESERVEORDER
					   |DNS_MESSAGEPARSE_BESTEFFORT);
		else
			result = dns_message_parse(msg, b,
					   DNS_MESSAGEPARSE_PRESERVEORDER);
		if (result != ISC_R_SUCCESS && 
		    result != DNS_R_RECOVERABLE ) {
			printf(";; Got bad packet: %s\n",
			       dns_result_totext(result));
			hex_dump(b);
			query->waiting_connect = ISC_FALSE;
			dns_message_destroy(&msg);
			isc_event_free(&event);
			clear_query(query);
			cancel_lookup(l);
			check_next_lookup(l);
			UNLOCK_LOOKUP;
			return;
		}
		if (result == DNS_R_RECOVERABLE)
			printf(";; Warning: Message parser reports malformed "
			       "message packet.\n");
		if (((msg->flags & DNS_MESSAGEFLAG_TC) != 0) 
		    && ! l->ignore && !l->tcp_mode) {
			printf(";; Truncated, retrying in TCP mode.\n");
			n = requeue_lookup(l, ISC_TRUE);
			n->tcp_mode = ISC_TRUE;
			dns_message_destroy(&msg);
			isc_event_free(&event);
			clear_query(query);
			cancel_lookup(l);
			check_next_lookup(l);
			UNLOCK_LOOKUP;
			return;
		}			
		if ((msg->rcode == dns_rcode_servfail) &&
		    l->servfail_stops) {
			dig_query_t *next = ISC_LIST_NEXT(query, link);
			if (l->current_query == query)
				l->current_query = NULL;
			if (next != NULL) {
				debug("sending query %lx\n", next);
				if (l->tcp_mode)
					send_tcp_connect(next);
				else
					send_udp(next);
			}
			/*
			 * If our query is at the head of the list and there
			 * is no next, we're the only one left, so fall
			 * through to print the message.
			 */
			if ((ISC_LIST_HEAD(l->q) != query) ||
			    (ISC_LIST_NEXT(query, link) != NULL)) {
				printf(";; Got SERVFAIL reply from %s, "
				       "trying next server\n",
				       query->servname);
				clear_query(query);
				check_next_lookup(l);
				dns_message_destroy(&msg);
				isc_event_free(&event);
				UNLOCK_LOOKUP;
				return;
			}
		}

		if (key != NULL) {
			result = dns_tsig_verify(&query->recvbuf, msg,
						 NULL, NULL);
			if (result != ISC_R_SUCCESS) {
				printf(";; Couldn't verify signature: %s\n",
				       dns_result_totext(result));
				validated = ISC_FALSE;
			}
			l->tsigctx = msg->tsigctx;
			if (l->querysig != NULL) {
				debug("freeing querysig buffer %p",
				       l->querysig);
				isc_buffer_free(&l->querysig);
			}
			result = dns_message_getquerytsig(msg, mctx,
						     &l->querysig);
			check_result(result,"dns_message_getquerytsig");
			debug("querysig 3 is %p", l->querysig);
		}
		debug("after parse");
		if (l->xfr_q == NULL) {
			l->xfr_q = query;
			/*
			 * Once we are in the XFR message, increase
			 * the timeout to much longer, so brief network
			 * outages won't cause the XFR to abort
			 */
			if ((timeout != INT_MAX) &&
			    (l->timer != NULL) &&
			    l->doing_xfr ) {
				if (timeout == 0) {
					if (l->tcp_mode)
						local_timeout = TCP_TIMEOUT;
					else
						local_timeout = UDP_TIMEOUT;
				} else {
					if (timeout < (INT_MAX / 4))
						local_timeout = timeout * 4;
					else
						local_timeout = INT_MAX;
				}
				debug("have local timeout of %d",
				       local_timeout);
				isc_interval_set(&l->interval,
						 local_timeout, 0);
				result = isc_timer_reset(l->timer,
						      isc_timertype_once,
						      NULL,
						      &l->interval,
						      ISC_FALSE);
				check_result(result, "isc_timer_reset");
			}
		}
		if (l->xfr_q == query) {
			if ((l->trace)||
			    (l->ns_search_only)) {
				debug("in TRACE code");
				printmessage(query, msg, ISC_TRUE);
				if ((msg->rcode != 0) &&
				    (l->origin != NULL)) {
					if (!next_origin(msg, query)) {
						printmessage(query, msg,
							     ISC_TRUE);
						isc_buffer_init(&ab, abspace,
								MXNAME);
						result = isc_sockaddr_totext(
							&sevent->address,
							&ab);
						check_result(result,
						      "isc_sockaddr_totext");
						isc_buffer_usedregion(&ab, &r);
						received(b->used, r.length,
							 (char *)r.base,
							 query);
					}
				} else {
					result = dns_message_firstname
						(msg,DNS_SECTION_ANSWER);
					if ((result != ISC_R_SUCCESS) ||
					    l->trace_root)
						followup_lookup(msg, query,
							DNS_SECTION_AUTHORITY);
				}
			} else if ((msg->rcode != 0) &&
				 (l->origin != NULL)) {
				if (!next_origin(msg, query)) {
					printmessage(query, msg,
						     ISC_TRUE);
					isc_buffer_init(&ab, abspace, MXNAME);
					result = isc_sockaddr_totext(
							     &sevent->address,
							     &ab);
					check_result(result,
						     "isc_sockaddr_totext");
					isc_buffer_usedregion(&ab, &r);
					received(b->used, r.length,
						 (char *)r.base,
						 query);
				}
			} else {
				printmessage(query, msg, ISC_TRUE);
			}
		} else if ((dns_message_firstname(msg, DNS_SECTION_ANSWER)
			    == ISC_R_SUCCESS) &&
			   l->ns_search_only &&
			   !l->trace_root ) {
			printmessage(query, msg, ISC_TRUE);
		}

		if (l->pending)
			debug("still pending.");
		if (l->doing_xfr) {
			if (query != l->xfr_q) {
				dns_message_destroy(&msg);
				isc_event_free (&event);
				query->waiting_connect = ISC_FALSE;
				UNLOCK_LOOKUP;
				return;
			}
			docancel = check_for_more_data(query, msg, sevent);
			if (docancel) {
				dns_message_destroy(&msg);
				clear_query(query);
				cancel_lookup(l);
				check_next_lookup(l);
			}
			if (msg != NULL)
				dns_message_destroy(&msg);
			isc_event_free(&event);
		}
		else {
			if ((msg->rcode == 0) ||
			    (l->origin == NULL)) {
				isc_buffer_init(&ab, abspace, MXNAME);
				result = isc_sockaddr_totext(&sevent->address,
							     &ab);
				check_result(result, "isc_sockaddr_totext");
				isc_buffer_usedregion(&ab, &r);
				received(b->used, r.length,
					 (char *)r.base,
					 query);
			}
			query->lookup->pending = ISC_FALSE;
			if (!query->lookup->ns_search_only ||
			    query->lookup->trace_root) {
				dns_message_destroy(&msg);
				cancel_lookup(l);
			}
			if (msg != NULL)
				dns_message_destroy(&msg);
			isc_event_free(&event);
			clear_query(query);
			check_next_lookup(l);
		}
		UNLOCK_LOOKUP;
		return;
	}
	/*
	 * In truth, we should never get into the CANCELED routine, since
	 * the cancel_lookup() routine clears the pending flag.
	 * XXX Is this true anymore, since the bulk changes?
	 */
	if (sevent->result == ISC_R_CANCELED) {
		debug("in recv cancel handler");
		query->waiting_connect = ISC_FALSE;
		isc_event_free(&event);
		clear_query(query);
		check_next_lookup(l);
		UNLOCK_LOOKUP;
		return;
	}
	printf(";; communications error: %s\n",
	       isc_result_totext(sevent->result));
	isc_socket_detach(&query->sock);
	sockcount--;
	debug("sockcount=%d", sockcount);
	INSIST(sockcount >= 0);
	isc_event_free(&event);
	clear_query(query);
	check_next_lookup(l);
	UNLOCK_LOOKUP;
	return;
}

/*
 * Turn a name into an address, using system-supplied routines.  This is
 * used in looking up server names, etc... and needs to use system-supplied
 * routines, since they may be using a non-DNS system for these lookups.
 */
void
get_address(char *host, in_port_t port, isc_sockaddr_t *sockaddr) {
	struct in_addr in4;
	struct in6_addr in6;
#if defined(HAVE_ADDRINFO) && defined(HAVE_GETADDRINFO)
	struct addrinfo *res = NULL;
	int result;
#else
	struct hostent *he;
#endif

	debug("get_address()");

	/*
	 * Assume we have v4 if we don't have v6, since setup_libs
	 * fatal()'s out if we don't have either.
	 */
	if (have_ipv6 && inet_pton(AF_INET6, host, &in6) == 1)
		isc_sockaddr_fromin6(sockaddr, &in6, port);
	else if (inet_pton(AF_INET, host, &in4) == 1)
		isc_sockaddr_fromin(sockaddr, &in4, port);
	else {
#if defined(HAVE_ADDRINFO) && defined(HAVE_GETADDRINFO)
		debug ("before getaddrinfo()");
		is_blocking = ISC_TRUE;
		result = getaddrinfo(host, NULL, NULL, &res);
		is_blocking = ISC_FALSE;
		if (result != 0) {
			fatal("Couldn't find server '%s': %s",
			      host, gai_strerror(result));
		}
		memcpy(&sockaddr->type.sa,res->ai_addr, res->ai_addrlen);
		sockaddr->length = res->ai_addrlen;
		isc_sockaddr_setport(sockaddr, port);
		freeaddrinfo(res);
#else
		debug ("before gethostbyname()");
		is_blocking = ISC_TRUE;
		he = gethostbyname(host);
		is_blocking = ISC_FALSE;
		if (he == NULL)
		     fatal("Couldn't find server '%s' (h_errno=%d)",
			   host, h_errno);
		INSIST(he->h_addrtype == AF_INET);
		isc_sockaddr_fromin(sockaddr,
				    (struct in_addr *)(he->h_addr_list[0]),
				    port);
#endif
	}
}

/*
 * Initiate either a TCP or UDP lookup
 */
void
do_lookup(dig_lookup_t *lookup) {

	REQUIRE(lookup != NULL);

	debug("do_lookup()");
	lookup->pending = ISC_TRUE;
	if (lookup->tcp_mode)
		send_tcp_connect(ISC_LIST_HEAD(lookup->q));
	else
		send_udp(ISC_LIST_HEAD(lookup->q));
}

/*
 * Start everything in action upon task startup.
 */
void
onrun_callback(isc_task_t *task, isc_event_t *event) {
	UNUSED(task);

	isc_event_free(&event);
	LOCK_LOOKUP;
	start_lookup();
	UNLOCK_LOOKUP;
}

/*
 * Make everything on the lookup queue go away.  Mainly used by the
 * SIGINT handler.
 */
void
cancel_all(void) {
	dig_lookup_t *l, *n;
	dig_query_t *q, *nq;

	debug("cancel_all()");

	if (is_blocking) {
		/*
		 * If we get here while another thread is blocking, there's
		 * really nothing we can do to make a clean shutdown
		 * without waiting for the block to complete.  The only
		 * way to get the system down now is to just exit out,
		 * and trust the OS to clean up for us.
		 */
		fputs("Abort.\n", stderr);
		exit(1);
	}
	LOCK_LOOKUP;
	if (free_now) {
		UNLOCK_LOOKUP;
		return;
	}
	cancel_now = ISC_TRUE;
	if (current_lookup != NULL) {
		if (current_lookup->timer != NULL)
			isc_timer_detach(&current_lookup->timer);
		q = ISC_LIST_HEAD(current_lookup->q);
		while (q != NULL) {
			debug("cancelling query %p, belonging to %p",
			       q, current_lookup);
			nq = ISC_LIST_NEXT(q, link);
			if (q->sock != NULL) {
				isc_socket_cancel(q->sock, NULL,
						  ISC_SOCKCANCEL_ALL);
			} else {
				clear_query (q);
			}
			q = nq;
		}
	}
	l = ISC_LIST_HEAD(lookup_list);
	while (l != NULL) {
		n = ISC_LIST_NEXT(l, link);
		ISC_LIST_DEQUEUE(lookup_list, l, link);
		try_clear_lookup(l);
		l = n;
	}
	UNLOCK_LOOKUP;
}

/*
 * Destroy all of the libs we are using, and get everything ready for a
 * clean shutdown.
 */
void
destroy_libs(void) {
	void *ptr;
	dig_server_t *s;
	dig_searchlist_t *o;

	debug("destroy_libs()");
	if (is_blocking) {
		/*
		 * If we get here while another thread is blocking, there's
		 * really nothing we can do to make a clean shutdown
		 * without waiting for the block to complete.  The only
		 * way to get the system down now is to just exit out,
		 * and trust the OS to clean up for us.
		 */
		fputs("Abort.\n", stderr);
		exit(1);
	}
	if (global_task != NULL) {
		debug("freeing task");
		isc_task_detach(&global_task);
	}
	/*
	 * The taskmgr_destroy() call blocks until all events are cleared
	 * from the task.
	 */
	if (taskmgr != NULL) {
		debug("freeing taskmgr");
		isc_taskmgr_destroy(&taskmgr);
        }
	LOCK_LOOKUP;
	REQUIRE(sockcount == 0);
	REQUIRE(recvcount == 0);
	REQUIRE(sendcount == 0);

	INSIST(ISC_LIST_HEAD(lookup_list) == NULL);
	INSIST(current_lookup == NULL);
	INSIST(!free_now);

	free_now = ISC_TRUE;

	if (fixedsearch != NULL) {
		debug("freeing fixed search");
		isc_mem_free(mctx, fixedsearch);
		fixedsearch = NULL;
	}
	s = ISC_LIST_HEAD(server_list);
	while (s != NULL) {
		debug("freeing global server %p", s);
		ptr = s;
		s = ISC_LIST_NEXT(s, link);
		isc_mem_free(mctx, ptr);
	}
	o = ISC_LIST_HEAD(search_list);
	while (o != NULL) {
		debug("freeing search %p", o);
		ptr = o;
		o = ISC_LIST_NEXT(o, link);
		isc_mem_free(mctx, ptr);
	}
	if (commctx != NULL) {
		debug("freeing commctx");
		isc_mempool_destroy(&commctx);
	}
	if (socketmgr != NULL) {
		debug("freeing socketmgr");
		isc_socketmgr_destroy(&socketmgr);
	}
	if (timermgr != NULL) {
		debug("freeing timermgr");
		isc_timermgr_destroy(&timermgr);
	}
	if (key != NULL) {
		debug("freeing key %p", key);
		dns_tsigkey_detach(&key);
	}
	if (namebuf != NULL)
		isc_buffer_free(&namebuf);

	if (is_dst_up) {
		debug("destroy DST lib");
		dst_lib_destroy();
		is_dst_up = ISC_FALSE;
	}
	if (entp != NULL) {
		debug("detach from entropy");
		isc_entropy_detach(&entp);
	}

	UNLOCK_LOOKUP;
	DESTROYLOCK(&lookup_lock);
	if (memdebugging != 0)
		isc_mem_stats(mctx, stderr);
	if (mctx != NULL)
		isc_mem_destroy(&mctx);
}
