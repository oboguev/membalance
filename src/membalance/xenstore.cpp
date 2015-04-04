/*
 *  MEMBALANCE daemon
 *
 *  xenstore.cpp - Xenstore related routines
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 *
 *  NB:
 *
 *  Xenstore uses isolation level = SERIALIZABLE.
 *  A per-connection snapshot of the database is taken when transaction begins,
 *  and all read/write/etc. operations are performed against this snapshot.
 *  If transaction commits successfully, the snapshot content replaces the active
 *  database and becomes the active database.
 *
 *  Transaction is committed if database generation# at commit time is still the
 *  same as recorded at transaction start time. Generation# is incremented at
 *  successful commit time.
 */

#include "membalanced.h"

/*
 * Membalance utilizes the following data layout in xenstore.
 *
 * "Report" keys are used by guest domains (memprobed) to report their status
 * and are written frequently to by guest domains, and host can have manu guests
 * running.
 *
 * Therefore we host "report" keys under /tool/membalance/domain/{qid} rather than
 * under /local/domain/<domid> so as to avoid generating frequent xenstore watch
 * events on /local/domain, whereas /tool/membalance/domain is not listened to.
 *
 * For each DomU that ever has been in a managed state, an unique generated {qid} is assigned:
 *
 *     /local/domain/<domid>/membalance/report_path = "/tool/membalance/domain/{qid}/report"  [Dom0:rw, DomU:r]
 *     /tool/membalance/domain/{qid}/domid = <domid>  [Dom0:rw]
 *     /tool/membalance/domain/{qid}/report           [Dom0:rw, DomU:rw]
 *
 * Global:
 *
 *     /tool/membalance/interval                      [Dom0:rw, all managed DomU's:r]
 *
 */

/******************************************************************************
*                             local definitions                               *
******************************************************************************/

/* no xenstore transaction */
#define XS_TRANSACTION_NULL 0


/******************************************************************************
*                              static data                                    *
******************************************************************************/

static bool initialized_xs = false;       /* xenstore connection initialized */

static xs_transaction_t xst = XS_TRANSACTION_NULL;      /* xenstore transaction */

/*
 * Root path in xenstore for domains and per-domain data
 */
static const char local_domain_path[] = "/local/domain";

/*
 * Path in xenstore for report update interval.
 * Created and written by MEMBALANCED, read only by MEMPROBED.
 */
static const char membalance_interval_path[] = "/tool/membalance/interval";

/*
 * Per-domain membalance keys are hosted under this root.
 */
static const char membalance_domain_root_path[] = "/tool/membalance/domain";

/*
 * Path in xenstore that points the location of report key for a domain.
 * Created and read by MEMBALANCED, read by MEMPROBED.
 * Relative to domain path (/local/domain/<domid>).     						.
 */
static const char membalance_report_link_path[] = "membalance/report_path";

/*
 * libxl logging information
 */
static xentoollog_logger_stdiostream *xtl_logger = NULL;
static const xentoollog_level xtl_loglevel = XTL_WARN;  // or XTL_DEBUG


/******************************************************************************
*                          forward declarations                               *
******************************************************************************/

static bool is_local_domain_path(const char* path, long* p_domid, const char** p_subpath);
static tribool read_xs(const char* path, long* p_value, long minval);
static void retry_wait(int cycle);
static bool is_valid_membalance_report_link_path(const char* keyvalue,
						 size_t report_path_size,
						 char* qid);


/******************************************************************************
*                                routines                                     *
******************************************************************************/

/*
 * Initialize xenstore structure.
 *
 * Opens xs connection and subscribres to events under /local/domain path.
 * Aborts if unable to open xs connection or subscribe in a reasonable time.
 *
 * @pollfds defines of signal-handling descriptors to be watched if has to wait
 * during the initialization.
 *
 * Will not touch pollfds[NPFD_XS].
 */
void initialize_xs(struct pollfd* pollfds)
{
	bool displayed_msg = false;
	struct timespec ts0;
	int64_t wsec;
	int k;

	/* already initialized? */
	if (initialized_xs)
		return;

	/*
	 * Open connection to xenstore.
	 * Then try to set watch on /local/domain key.
	 *
	 * If we are running as a daemon during system startup, there
	 * can be a race condition with continuing Xen initialization
	 * (/proc/xen including /proc/xen/privcmd may be unavailable yet,
	 * still to be created even though Xen service startup completion
	 * has already been signalled), so wait for a while if necessary.
	 */
	for (ts0 = getnow();;)
	{
		if (!xs)
			xs = xs_open(0);

		if (xs && xs_watch(xs, local_domain_path, local_domain_path))
			break;

		/* time since the start */
		wsec = timespec_diff_ms(getnow(), ts0) / MSEC_PER_SEC;

		if (run_as_daemon && wsec < config.max_xen_init_retries)
		{
			if (wsec >= config.xen_init_retry_msg && !displayed_msg)
			{
				notice_msg("waiting for Xen to initialize");
				displayed_msg = true;
			}

			if (pollfds != NULL)
			{
				int npollfds = NPFD_COUNT - 1;
				for (k = 0;  k < npollfds;  k++)
					pollfds[k].revents = 0;
				if (poll(pollfds, npollfds, 1 * MSEC_PER_SEC) < 0)
					fatal_perror("poll");
				handle_signals(pollfds, NULL);
			}
			else
			{
				sleep(1);
			}
		}
		else
		{
			if (!xs)
				fatal_perror("unable to open connection to xenstore");
			else
				fatal_perror("unable to set a watch on xenstore key (%s)", local_domain_path);
		}
	}


	debug_msg(2, "initialized xs connection");

	initialized_xs = true;
}

/*
 * Abort current xenstore transaction if any
 * and shutdown connection to xenstore
 */
void shutdown_xs(void)
{
	abort_xs();

	if (xs != NULL)
	{
		xs_close(xs);
		xs = NULL;
	}
}

/*
 * Start XenStore transaction.
 * On error, log error message and abort.
 */
void begin_xs(void)
{
	int nretries = 0;

	if (xst != XS_TRANSACTION_NULL)
		fatal_msg("bug: begin_xs inside a transaction");

	for (;;)
	{
		xst = xs_transaction_start(xs);
		if (xst != XS_TRANSACTION_NULL)
			break;

		/*
		 * ENOSPC means too many concurrent transactions from the domain
		 */
		if (errno == ENOSPC)
		{
			if (++nretries > config.max_xs_retries)
			{
				error_perror("unable to start xenstore transaction (retry limit exceeded)");
				break;
			}

			retry_wait(nretries);
			debug_msg(2, "retrying to start xenstore transaction");
		}
		else
		{
			error_perror("unable to start xenstore transaction");
			break;
		}
	}

	if (xst != XS_TRANSACTION_NULL)
	{
		debug_msg(40, "started transaction");
	}
	else
	{
		::terminate();
	}
}


/*
 * Abort current xenstore transaction if any
 */
void abort_xs(void)
{
	if (xst != XS_TRANSACTION_NULL)
	{
		xs_transaction_end(xs, xst, TRUE);
		xst = XS_TRANSACTION_NULL;
		debug_msg(40, "aborted transaction");
	}
}

/*
 * Commit current xenstore transaction.
 *
 * Returns:
 * 	XSTS_OK	       success
 * 	XSTS_RETRY     retry the transaction
 *	XSTS_NORETRY   out of retry limit (message logged)
 *	XSTS_FAIL      commit failed (message logged)
 */
XsTransactionStatus commit_xs(int* p_nretries)
{
	if (xst == XS_TRANSACTION_NULL)
		fatal_msg("bug: commit_xs outside of a transaction");

	if (xs_transaction_end(xs, xst, FALSE))
	{
		debug_msg(40, "committed transaction");
		xst = XS_TRANSACTION_NULL;
		return XSTS_OK;
	}

	xst = XS_TRANSACTION_NULL;

	if (errno == EAGAIN)
	{
		if (++(*p_nretries) > config.max_xs_retries)
		{
			error_perror("unable to write xenstore (transaction retry limit exceeded)");
			return XSTS_NORETRY;
		}

		retry_wait(*p_nretries);

		debug_msg(2, "restarting xenstore transaction");
		return XSTS_RETRY;
	}
	else
	{
		error_perror("unable to write xenstore (xs_transaction_end)");
		return XSTS_FAIL;
	}
}

/*
 * Backoff time till next retry cycle.
 */
static void retry_wait(int cycle)
{
	if (cycle < config.max_xs_retries / 2)
		return;

	/*
	 * Increase pre-retry wait time linearly from 20ms to 5 sec between
	 * attempt [max_xs_retries / 2] and [max_xs_retries].
	 *
	 * If contention ever gets really high and deemed to cause a problem,
	 * can implement randomized truncated exponential back-off.
	 */
	const double w1 = 20;
	const double w2 = 5 * MSEC_PER_SEC;
	const int n1 = config.max_xs_retries / 2;
	const int n2 = config.max_xs_retries;

	double w = w1 + (cycle - n1) * (w2 - w1) / (n2 - n1);

	usleep((int) (USEC_PER_MSEC * w));
}

/*
 * Check if a transaction is currently active
 */
bool in_transaction_xs(void)
{
	return xst != XS_TRANSACTION_NULL;
}

/*
 * Begin a pseudo-transaction that involves just one read or write operation
 */
void begin_singleop_xs(void)
{
	if (xst != XS_TRANSACTION_NULL)
		fatal_msg("bug: begin_singleop_xs inside a transaction");
}

/*
 * Abort a pseudo-transaction that involves just one read or write operation
 */
void abort_singleop_xs(void)
{
	if (xst != XS_TRANSACTION_NULL)
		fatal_msg("bug: abort_singleop_xs inside a real transaction");
}

/*
 * Commit a pseudo-transaction that involves just one read or write operation
 */
XsTransactionStatus commit_singleop_xs(int* p_nretries)
{
	if (xst != XS_TRANSACTION_NULL)
		fatal_msg("bug: commit_singleop_xs inside a real transaction");
	return XSTS_OK;
}

/*
 * Enumerate all local domains
 */
void enumerate_local_domains(domid_set& ids)
{
	char** domain_dir = NULL;
	unsigned int domain_dir_count, j;
	long domain_id;

	ids.clear();

	begin_singleop_xs();
	domain_dir = xs_directory(xs, xst, local_domain_path, &domain_dir_count);
	if (!domain_dir)
		fatal_perror("unable to enumerate domains in xenstore (%s)", local_domain_path);
	abort_singleop_xs();

	for (j = 0;  j < domain_dir_count;  j++)
	{
		/* ignore non-numeric keys */
		if (!a2long(domain_dir[j], &domain_id) || domain_id < 0)
			continue;
		ids.insert(domain_id);
	}

	free_ptr(domain_dir);
}


/*
 * Called when a watched value in xenstore has been changed
 */
void handle_xs_watch(void)
{
	char* path;
	char* token;
	char** vec;
	unsigned int k, num = 0;
	long domid;
	const char* subpath;

	vec = xs_read_watch(xs, &num);

	if (vec == NULL)
	{
		error_perror("cannot read xenstore watch");
		return;
	}

	for (k = 0;  k < num;  k += 2)
	{
		path  = vec[k + XS_WATCH_PATH];
		token = vec[k + XS_WATCH_TOKEN];

		if (is_local_domain_path(path, &domid, &subpath))
			handle_xs_watch_event(domid, subpath);
	}

	free_ptr(vec);
}

/*
 *  Check if specified path is for a local domain:
 *
 *      /local/domain/5            => true, domid = 5, subpath = "" (empty)
 *      /local/domain/5/abc/xyz    => true, domid = 5, subpath = "abc/xyz"
 *      /some/other/path	   => false
 */
static bool is_local_domain_path(const char* path, long* p_domid, const char** p_subpath)
{
	int len = countof(local_domain_path) - 1;
	long domid;
	char* p = NULL;

	if (0 != strncmp(path, local_domain_path, len))
		return false;

	path += len;
	if (*path++ != '/')
		return false;

	errno = 0;
	domid = strtol(path, &p, 10);
	if (errno || p == path || domid < 0)
		return false;

	if (*p == '\0')
	{
		*p_domid = domid;
		*p_subpath = p;
		return true;
	}
	else if (*p == '/')
	{
		*p_domid = domid;
		*p_subpath = p + 1;
		return true;
	}
	else
	{
		return false;
	}
}

/*
 * Try to process pending domain.
 *
 * Normally called once a second until the data has been collected,
 * but under heavy system load an interval between successive calls
 * can take longer than a second.
 *
 * If domain still exists, return *@p_is_dead = @false and
 * return: @true if domain can and ready be managed.
 *         @false if domain is to be unmanaged.
 *         @maybe if still undetermined and should be tried again later.
 *
 * If domain does not exist anymore, return *@p_is_dead = @true
 * and return TriFalse.
 */
tribool domain_info::process_pending(bool* p_is_dead)
{
	tribool res = TriMaybe;
	bool do_abort_xs = false;

	*p_is_dead = false;

	/* do not manage Dom0 unless permitted */
	if (domain_id == 0 && !config.dom0_mode)
		return TriFalse;

	/*
	 * with increasing number of completed passes,
	 * reduce scan frequency (cycle is 1 second)
	 */
	++pending_cycle;
	if (pending_cycle <= 3)
	{
		/* do every cycle */
	}
	else if (pending_cycle <= 6)
	{
		/* do every other cycle */
		if (++pending_skipped < 2)
			return TriMaybe;
	}
	else if (pending_cycle <= 20)
	{
		/* do every 5th cycle */
		if (++pending_skipped < 5)
			return TriMaybe;
	}
	else
	{
		/* do every 10th cycle */
		if (++pending_skipped < 10)
			return TriMaybe;
	}

	pending_skipped = 0;

	debug_msg(5, "processing pending domain %s", printable_name());

	if (!is_xs_data_complete())
	{
		/*
		 * read data items from xenstore in a transaction
		 */
		begin_xs();
		do_abort_xs = true;

		/*
		 * read domain data from xenstore
		 */
		switch (process_pending_read_xs())
		{
		case 'c':  goto cleanup;	/* try again later */
		case 'd':  goto dead;           /* domain is dead */
		case 'u':  goto unmanage;       /* unmanage */
		default:   break;		/* proceed (have data) */
		}

		/*
		 * Release xenstore transaction before further steps
		 */
		abort_xs();
		do_abort_xs = false;
	}
	else if (TriFalse == domain_alive(domain_id))
	{
		/*
		 * Re-check domain still exists
		 */
		goto dead;
	}

	/*
	 * Read domain memory allocation as reported by xenctrl to calculate
	 * the adjustment between domain size reported by xenctrl and target as
	 * recorded in xenstore and accepted by libxl_set_memory_target(...).
	 *
	 * This adjustment is composed of various parts described in
	 * $(XENSRC)/docs/misc/libxl_memory.txt, some of which are calculated
	 * internally by XL and this calculation cannot be reproduced by us outside
	 * of XL, but we do not need to: we only need to capture the total
	 * adjustment. This of course assumes the size of this Xen-private per-domain
	 * data is static and does not vary within domain lifetime.
	 *
	 * See details in sched.cpp under "Domain size calculus, fine details".
	 *
	 * A flaky part:
	 *
	 * If a domain is in the process of being resized, its memory allocation
	 * can be in a flux moving towards the target. Target record in xenstore
	 * may also itself be subject to change if system administrator or another
	 * application is trying to resize the domain in parallel to us. Thus the
	 * size of Xen-private domain memory part cannot be reliably determined on
	 * the basis of a single sample. To overcome this issue, we sample "xen part"
	 * size several times with 1 sec intervals and accept the value only if it
	 * remains stable (and domain is also runnable, so resizing process is not
	 * frozen pending the resumption of the domain). This is still imperfect,
	 * but is the best we can do from the outside of Xen.
	 *
	 * xen_data_size_phase legend:
	 *
	 *     0 => no data yet
	 *     1 => read data 1 time
	 *     ...
	 *     N => read data N times
	 *     config.xen_private_data_size_samples => reading complete
	 *
	 * Even more flaky part:
	 *
	 * The size of Xen-private per-domain area actually is not static.
	 * See details in sched.cpp under "Domain size calculus, fine details".
	 */
	if (xen_data_size_phase < config.xen_private_data_size_samples)
	{
		long xds = xen_data_size;

		switch (eval_xen_data_size())
		{
		case '1':    goto again_1sec;   	/* repeat in 1 sec */
		case 'c':    goto cleanup;		/* repeat later */
		case 'd':    goto dead;			/* domain is dead */
		case 'u':    goto unmanage;		/* unmanage */
		default:     break;     		/* proceed (have data) */
		}

		if (xen_data_size_phase == 0 || xds != xen_data_size)
		{
			xen_data_size_phase = 1;
			goto again_1sec;
		}
		else if (++xen_data_size_phase < config.xen_private_data_size_samples)
		{
			goto again_1sec;
		}
	}

	/*
	 * Read and parse domain config file to extract per-domain membalance settings.
	 * Result status will be stored in @config_file_status.
	 */
	retrieve_config_file_settings();

	/*
	 * If successfully gathered all configuration, try to resolve
	 * config file settings vs. global config data
	 */
	if (config_file_status == TriTrue)
	{
		if (resolved_config_seq == config.seq)
		{
			/* already resolved fine with current config */
			res = TriTrue;
		}
		else if (resolve_settings())
		{
			resolved_config_seq = config.seq;
			res = TriTrue;
		}
		else
		{
			res = TriFalse;
		}
	}
	else if (config_file_status == TriFalse)
	{
		res = TriFalse;
	}

	/*
	 * If @res == @maybe  - stay pending and try again later (until times out)
	 *         == @false  - transition pending -> unmanaged
	 *         == @true   - transition pending -> managed
	 */

cleanup:
	if (do_abort_xs)
		abort_xs();
	return res;

dead:
	*p_is_dead = true;
	/* fall through to unmanage */

unmanage:
	if (do_abort_xs)
		abort_xs();
	return TriFalse;

again_1sec:
	if (do_abort_xs)
		abort_xs();
	pending_skipped = 100;
	return TriMaybe;
}

/*
 * Check if xenstore data has been gathered
 */
bool domain_info::is_xs_data_complete(void)
{
	/*
	 * Postpone reading domain config file until xenstore record for
	 * the domain has been initialized
	 */
	CHECK(vm_name);
	CHECK(vm_uuid);
	CHECK(xs_mem_max >= 0);
	CHECK(xs_mem_target >= 0);
	CHECK(xs_mem_videoram != XS_MEM_VIDEORAM_UNSET);
	CHECK(xs_start_time || domain_id == 0);

	return true;

cleanup:

	return false;
}

/*
 * Read xenstore data for a pending domain.
 * Return:
 *      'd'  => domain is dead
 *      'u'  => unmanage domain
 *      'c'  => try again later
 *      'p'  => proceeed (data is available)
 */
char domain_info::process_pending_read_xs(void)
{
	char path[256];
	unsigned int len;
	char* p;
	char* xp;
	bool exists = false;

	if (!vm_name)
	{
		sprintf(path, "%s/%ld/name", local_domain_path, domain_id);
		p = (char*) xs_read(xs, xst, path, &len);
		if (!p)  goto path_read_error;
		vm_name = p;
		exists = true;
	}

	if (!vm_uuid && domain_id != 0)
	{
		/* note: Dom0 tree does not have vm subkey */
		static const char vmp[] = "/vm/";
		sprintf(path, "%s/%ld/vm", local_domain_path, domain_id);
		p = (char*) xs_read(xs, xst, path, &len);
		if (!p)  goto path_read_error;
		xp = p;
		if (0 != strncmp(xp, vmp, countof(vmp) - 1))
		{
			error_msg("unexpected value of xenstore key (%s), "
				  "will not manage domain %ld (name: %s)",
				  path, domain_id,
				  vm_name ? vm_name : "<unknown>");
			goto unmanage;
		}
		xp += countof(vmp) - 1;
		vm_uuid = xstrdup(xp);
		free(p);
		exists = true;
	}

	if (xs_mem_max == -1)
	{
		sprintf(path, "%s/%ld/memory/static-max", local_domain_path, domain_id);
		switch (read_xs(path, &xs_mem_max, 0))
		{
		case TriTrue:   exists = true;  break;
		case TriFalse:  goto unmanage;
		case TriMaybe:  break;
		}
	}

	if (xs_mem_target == -1)
	{
		sprintf(path, "%s/%ld/memory/target", local_domain_path, domain_id);
		switch (read_xs(path, &xs_mem_target, 0))
		{
		case TriTrue:   exists = true;  break;
		case TriFalse:  goto unmanage;
		case TriMaybe:  break;
		}
	}

	if (xs_mem_videoram == XS_MEM_VIDEORAM_UNSET)
	{
		sprintf(path, "%s/%ld/memory/videoram", local_domain_path, domain_id);
		switch (read_xs(path, &xs_mem_videoram, -1))
		{
		case TriTrue:   exists = true;  break;
		case TriFalse:  goto unmanage;
		case TriMaybe:  break;
		}
	}

	if (domain_id == 0 && xs_mem_videoram == XS_MEM_VIDEORAM_UNSET)
	{
		/* Dom0 does not have videoram setting in xenstore */
		if (xs_mem_max != -1 && xs_mem_target != -1)
			xs_mem_videoram = 0;
	}

	if (!xs_start_time && domain_id != 0 && vm_uuid)
	{
		/*
		 * One of the latest markers in DomU initialization.
		 * Must be present for xen_domain_uptime(...) to work.
		 * Not used for Dom0.
		 */
		sprintf(path, "/vm/%s/start_time", vm_uuid);
		p = (char*) xs_read(xs, xst, path, &len);
		if (!p)  goto path_read_error;
		xs_start_time = p;
	}

	/*
	 * Check if domain exists or has been destroyed
	 */
	if (!exists && TriFalse == domain_alive(domain_id))
		return 'd';

	/*
	 * Postpone determining xen private data size and reading domain
	 * config file until xenstore record for the domain has been
	 * initialized
	 */
	if (!is_xs_data_complete())
		return 'c';

	return 'p';

unmanage:
	return 'u';

path_read_error:
	if (errno != ENOENT && errno != ENOTDIR)
		fatal_perror("cannot read xenstore path %s", path);

	if (!exists && TriFalse == domain_alive(domain_id))
		return 'd';

	return 'c';
}

/*
 * Read long value from xenstore.
 *
 * @minval defines minimum valid value
 *
 * Returns:  @true - read successfully
 *           @false - key value present but is not valid (message issued)
 *           @maybe - key value is not present
 *
 * Aborts with message on serious xenstore error.
 */
tribool read_value_from_xs(domain_info* dom, const char* subpath,
			   long* p_value, long minval)
{
	tribool res;
	char path[256];

	snprintf(path, countof(path), "%s/%ld/%s",
		 local_domain_path, dom->domain_id, subpath);

	begin_singleop_xs();
	res = read_xs(path, p_value, minval);
	abort_singleop_xs();

	return res;
}

/*
 * Read non-negative long value from xenstore.
 *
 * Returns:  @true - read successfully
 *           @maybe - key value is not present
 *
 * Aborts with message on serious xenstore error.
 *
 * When returns @true, old *@p_value (if not NULL) is freed, and new result
 * is stored in @p_value.
 *
 * When returns @maybe, *@p_value is unaltered.
 */
tribool read_value_from_xs(domain_info* dom, const char* subpath, char** p_value)
{
	char path[256];
	char* p;
	int sv_errno;
	unsigned int len;

	snprintf(path, countof(path), "%s/%ld/%s",
		 local_domain_path, dom->domain_id, subpath);

	begin_singleop_xs();
	p = (char*) xs_read(xs, xst, path, &len);
	sv_errno = errno;
	abort_singleop_xs();

	if (p)
	{
		if (*p_value)
			free(*p_value);
		*p_value = p;
		return TriTrue;
	}
	else if (sv_errno == ENOENT || sv_errno == ENOTDIR)
	{
		return TriMaybe;
	}
	else
	{
		fatal_perror("cannot read xenstore path %s", path);
	}
}

/*
 * Read long value from xenstore.
 *
 * @minval defines minimum valid value
 *
 * Returns:  @true - read successfully
 *           @false - key value present but is not valid (message issued)
 *           @maybe - key value is not present
 *
 * Aborts with message on serious xenstore error.
 *
 * If consistency with other operations is desired, must be called inside
 * a xenstore transaction.
 */
static tribool read_xs(const char* path, long* p_value, long minval)
{
	char* cp;
	unsigned int len;
	long v;

	cp = (char*) xs_read(xs, xst, path, &len);
	if (!cp)
	{
		if (errno == ENOENT || errno == ENOTDIR)
			return TriMaybe;
		fatal_perror("cannot read xenstore path %s", path);
        }

	if (!a2long(cp, &v))
	{
		error_msg("xenstore path %s: not numeric", path);
		free(cp);
		return TriFalse;
	}

	if (v < minval)
	{
		error_msg("xenstore path %s: invalid value (< %ld)", path, minval);
		free(cp);
		return TriFalse;
	}

	*p_value = v;
	free(cp);

	return TriTrue;
}

/*
 * Determine the size of Xen data for a pending domain.
 * Return:
 *      'd'  => domain is dead
 *      'u'  => unmanage domain
 *      '1'  => try again later in 1 second
 *      'c'  => try again later in perhaps more than 1 second
 *      'p'  => proceeed (data is available)
 *
 * If returning 'p', result is stored in this->xen_data_size.
 */
char domain_info::eval_xen_data_size(void)
{
	xc_domaininfo_t info;
	long curr_size;
	int rc;

	rc = xc_domain_getinfolist(xc_handle, (uint32_t) domain_id, 1, &info);

	/* no such domain? */
	if (rc < 0 && errno == ESRCH)
		return 'd';
	if (rc < 0)
		fatal_perror("unable to get Xen domain information (xc_domain_getinfolist)");
	if (rc == 0 || info.domain != domain_id)
		return 'd';

	/* wait till domain is runnable */
	if (!is_runnable(&info))
		return 'c';

	/*
	 * If Xen free memory is totally depleted, then memory allocated to a domain
	 * can stay below the target allocation for an extended time. This gap should
	 * not be mistook for xen_data_size.
	 *
	 * The check below is heuristic and inherently unreliable, but this while
	 * routine is. Perhaps one day Xen will expose an interface for determining
	 * xen_data_size and obviate the need for this routine.
	 *
	 * In practical terms though, Xen free memory slack should take care of this
	 * check being almost never triggered.
	 */
	if (get_xen_free_memory() < (long) pagesize_kbs * 100)
		return '1';

	/* still being constructed? */
	if (info.outstanding_pages)
		return '1';

	curr_size = info.tot_pages * pagesize_kbs;
	xen_data_size = curr_size - roundup(xs_mem_target + xs_mem_videoram, memquant_kbs);

	if (xen_data_size < 0)
		return '1';

	if (xen_data_size % memquant_kbs)
	{
		/* sanity check, should not happen */
		error_msg("xen_data_size is not multiple of pages for domain %s",
			  printable_name());
		xen_data_size -= xen_data_size % memquant_kbs;
	}

	return 'p';
}

/*
 * Iniialize libxl logging and context (for membalanced)
 */
void initialize_xl(void)
{
	FILE* fp_xtl_errlog_default = log_fp ? log_fp : stderr;
	FILE* fp_xtl_errlog = fp_xtl_errlog_default;
	xentoollog_logger* xtl;
	const libxl_version_info *xeninfo;

	/* create xl logger */
	if (debug_level < 20)
	{
		fp_xtl_errlog = fopen("/dev/null", "w");
		if (!fp_xtl_errlog)
			fp_xtl_errlog = fp_xtl_errlog_default;
	}

	xtl_logger = xtl_createlogger_stdiostream(fp_xtl_errlog, xtl_loglevel,  0);
	if (!xtl_logger)
		fatal_perror("unable to initialize XTL logger");
	xtl = (xentoollog_logger*) xtl_logger;

	/* create xl context */
	if (libxl_ctx_alloc(&xl_ctx, LIBXL_VERSION, 0, xtl))
		fatal_perror("unable to initialize XL context");

	/* create xenctrl interface */
	xc_handle = xc_interface_open(xtl, xtl, 0);
	if (!xc_handle)
		fatal_perror("unable to open interface to xenctrl");

	/* get xen info */
	xeninfo = libxl_get_version_info(xl_ctx);
	if (!xeninfo)
		fatal_msg("unable to retrieve Xen information (libxl_get_version_info)");

	if ((u_long) xeninfo->pagesize != pagesize_kbs * 1024)
		fatal_msg("unexpected Xen page size");
}

/*
 * Iniialize libxl logging and context (for membalancectl)
 */
void initialize_xl_ctl(void)
{
	xentoollog_logger* xtl;

	xtl_logger = xtl_createlogger_stdiostream(stderr, XTL_WARN,  0);
	if (!xtl_logger)
		fatal_perror("unable to initialize XTL logger");
	xtl = (xentoollog_logger*) xtl_logger;

	/* create xl context */
	if (libxl_ctx_alloc(&xl_ctx, LIBXL_VERSION, 0, xtl))
		fatal_perror("unable to initialize XL context");
}

/*
 * Check if domain exists and is alive.
 * Return:
 *     @true - exists
 *     @false - does not exsist
 *     @maybe - unable to find out
 *
 * Can be called either inside or outside of Xenstore transaction.
 *
 * However if used in conjunctions with key read/writes, a common
 * transaction context shared by domain_alive(...) and these operations
 * is required to ensure the consistency between them, otherwise other
 * xenstore client may delete the domain in between the domain_alive(...)
 * check and performing subsequent key read/write operations.
 */
tribool domain_alive(long domain_id)
{
	char path[256];
	char* p;
	unsigned int len;
	bool quit_xst = false;
	int e;

	if (!in_transaction_xs())
	{
		begin_singleop_xs();
		quit_xst = true;
	}

	sprintf(path, "%s/%ld", local_domain_path, domain_id);
	p = (char*) xs_read(xs, xst, path, &len);
	e = errno;

	if (quit_xst)
		abort_singleop_xs();

	if (p != NULL)
	{
		free(p);
		return TriTrue;
	}
	else if (e == ENOENT || e == ENOTDIR)
	{
		return TriFalse;
	}
	else
	{
		error_perror("error reading xenstore (%s)", path);
		return TriMaybe;
	}
}

/*
 * Domains just transitioned pending->managed.
 *
 * Create domain-specific membalance structure (keys) in xenstore and assign them
 * approriate access rights for Dom0 and target domain.
 *
 * Return @true if successful, @false if not.
 *
 * On error log a message, except if the error is due to non-existent parent
 * key, in which case the domain is being destroyed.
 */
bool init_membalance_report(long domain_id)
{
	int nretries = 0;
	struct xs_permissions perms[2];
	const int nperms = 2;
	char link_path[256];
	char domid_path[256];
	char report_path[256];
	char* keyvalue;
	unsigned int len;
	uuid_t qid_uuid;
	char qid[UUID_STRING_SIZE];
	bool have_qid = false;
	char buf[256];
	const char* key;

	if (!contains(doms.managed, domain_id))
		fatal_msg("init_membalance_report: domain %ld is not managed", domain_id);

	perms[0].id = 0;
	perms[0].perms = (typeof(perms[0].perms)) (XS_PERM_READ | XS_PERM_WRITE);

	perms[1].id = (typeof(perms[1].id)) domain_id;

	sprintf(link_path, "%s/%ld/%s", local_domain_path, domain_id, membalance_report_link_path);

	for (;;)
	{
		begin_xs();

		/* check if domain is dead */
		if (domain_alive(domain_id) != TriTrue)
		{
			abort_xs();
			return false;
		}

		/* check if the structure already exists */
		keyvalue = (char*) xs_read(xs, xst, key = link_path, &len);

		if (keyvalue != NULL &&
		    is_valid_membalance_report_link_path(keyvalue, countof(report_path), qid))
		{
			strcpy(report_path, keyvalue);
			free(keyvalue);

			/* blank out report key */
			if (!xs_write(xs, xst, key = report_path, "", strlen("")))
				goto key_write_error;
		}
		else
		{
			if (keyvalue == NULL)
			{
				if (errno != ENOENT && errno != ENOTDIR)
					goto key_read_error;
			}
			else
			{
				/* link path was invalid, so re-create the structure */
				free(keyvalue);
			}

			/*
			 * generate unique @qid and format @domid_path and @report_path
			 */
			for (;;)
			{
				/* format paths */
				if (!have_qid)
				{
					uuid_generate(qid_uuid);
					uuid_unparse(qid_uuid, qid);

					sprintf(domid_path, "%s/%s/domid", membalance_domain_root_path, qid);
					sprintf(report_path, "%s/%s/report", membalance_domain_root_path, qid);
				}

				/* check if generated qid is unique */
				keyvalue = (char*) xs_read(xs, xst, key = domid_path, &len);
				if (keyvalue == NULL)
				{
					if (errno != ENOENT && errno != ENOTDIR)
						goto key_read_error;

					/* got unique qid */
					have_qid = true;
					break;
				}

				/* not unique - generate again */
				free(keyvalue);
				have_qid = false;
			}

			/* create @domid_path = <domid> */
			sprintf(buf, "%ld", domain_id);
			if (!xs_write(xs, xst, key = domid_path, buf, strlen(buf)))
				goto key_write_error;

			/* create @report_path = blank */
			if (!xs_write(xs, xst, key = report_path, "", strlen("")))
				goto key_write_error;

			/* set protection on @report_path */
			perms[1].perms = (typeof(perms[1].perms)) (XS_PERM_READ | XS_PERM_WRITE);
			if (!xs_set_permissions(xs, xst, key = report_path, perms, nperms))
				goto key_setperm_error;

			/* create @link_path = @report_path */
			if (!xs_write(xs, xst, key = link_path, report_path, strlen(report_path)))
				goto key_write_error;

			/* set protection on @link_path */
			perms[1].perms = XS_PERM_READ;
			if (!xs_set_permissions(xs, xst, key = link_path, perms, nperms))
				goto key_setperm_error;
		}

		/* commit the changes */
		switch (commit_xs(&nretries))
		{
		case XSTS_RETRY:    continue;
		case XSTS_NORETRY:  return false;
		case XSTS_FAIL:     return false;
		case XSTS_OK:
			doms.managed[domain_id]->set_qid(qid);
			doms.qid[domain_id] = std::string(qid);
			return true;
		}
	}

	/* exception handlers */
key_read_error:
	error_perror("unable to read xenstore key (%s)", key);
	goto cleanup;

key_write_error:
	error_perror("unable to write xenstore key (%s)", key);
	goto cleanup;

key_setperm_error:
	error_perror("unable to set xenstore key permissions (%s)", key);
	goto cleanup;

cleanup:
	abort_xs();
	return false;
}

/*
 * Check if @keyvalue has expected structure:
 *
 *     {membalance_domain_root_path}/{qid}/report
 *
 * and length < @report_path_size.
 *
 * If yes, return @true and copy out @qid.
 * if no, return @false.
 *
 * Preserves errno.
 */
static bool is_valid_membalance_report_link_path(const char* keyvalue,
					       size_t report_path_size,
					       char* qid)
{
	const char* cp = keyvalue;
	const char* ep;
	char* p;
	int len;

	if (strlen(cp) >= report_path_size)
		return false;

	len = strlen(membalance_domain_root_path);
	if (0 != strncmp(cp, membalance_domain_root_path, len))
		return false;

	cp += len;
	if (*cp++ != '/')
		return false;

	ep = strchr(cp, '/');
	if (!(ep && streq(ep, "/report")))
		return false;

	if (ep - cp != UUID_STRING_SIZE - 1)
		return false;

	for (p = qid; *cp != '/'; )
		*p++ = *cp++;

	*p = '\0';

	return true;
}

/*
 * Called on startup.
 *
 * Delete /tool/membalance/domain/{qid} keys for domains that no longer exist.
 * Populate doms.qid (domain_id -> qid map).
 */
void resync_qid(void)
{
	char** domain_dir = NULL;
	unsigned int domain_dir_count, j;
	bool changed;
	string_set qids;
	char* keyvalue;
	unsigned int len;
	char path[256];
	int nretries = 0;
	long domain_id;

	for (;;)
	{
		begin_xs();

		changed = false;
		qids.clear();
		doms.qid.clear();

		/*
		 * enumerate all qids
		 */
		domain_dir = xs_directory(xs, xst, membalance_domain_root_path, &domain_dir_count);
		if (!domain_dir)
		{
			if (errno == ENOENT || errno == ENOTDIR)
			{
				abort_xs();
				return;
			}
			else
			{
				fatal_perror("unable to enumerate membalance domain data in xenstore (%s)",
					     membalance_domain_root_path);
			}
		}

		for (j = 0;  j < domain_dir_count;  j++)
			insert(qids, domain_dir[j]);
		free_ptr(domain_dir);

		/*
		 * examine all qids
		 */
		for (string_set::const_iterator it = qids.begin(); it != qids.end();  ++it)
		{
			/*
			 * get domain id for this qid
			 */
			const char* qid = (*it).c_str();
			sprintf(path, "%s/%s/domid", membalance_domain_root_path, qid);

			keyvalue = (char*) xs_read(xs, xst, path, &len);
			if (keyvalue == NULL)
			{
				if (errno == ENOENT || errno == ENOTDIR)
					continue;
				fatal_perror("unable to read xenstore key (%s)", path);
			}

			if (!a2long(keyvalue, &domain_id) || domain_id < 0)
			{
				error_msg("invalid value of xenstore key (%s) = (%s)",
					  path, keyvalue);
				free(keyvalue);
				continue;
			}

			free(keyvalue);

			/*
			 * check if domain still exists
			 */
			switch (domain_alive(domain_id))
			{
			case TriTrue:
				/* domain alive, enlist its qid */
				doms.qid[domain_id] = *it;
				break;

			case TriFalse:
				/* domain is gone, delete its qid keys */
				sprintf(path, "%s/%s", membalance_domain_root_path, qid);
				if (xs_rm(xs, xst, path))
				{
					changed = true;
				}
				else if (errno != ENOENT && errno != ENOTDIR)
				{
					error_perror("unable to remove xenstore key (%s)", path);
				}
				break;

			case TriMaybe:
				/* at worst, leave stale qid keys alone */
				break;
			}
		}

		/*
		 * If we deleted no keys, abort the transaction, otherwise commit it
		 */
		if (!changed)
		{
			abort_xs();
			return;
		}
		else switch (commit_xs(&nretries))
		{
		case XSTS_OK:	    return;
		case XSTS_RETRY:    continue;
		case XSTS_NORETRY:  /* fall through */
		case XSTS_FAIL:     fatal_perror("unable to commit transaction (resync_qid)");
		}
	}
}


/*
 * Destroy membalance xenstore structures
 * for a terminating domain
 */
void qid_dead(long domain_id)
{
	char path[256];
	int nretries = 0;

	if (!contains(doms.qid, domain_id))
		return;

	sprintf(path, "%s/%s",
		membalance_domain_root_path,
		doms.qid[domain_id].c_str());

	begin_singleop_xs();

	if (xs_rm(xs, xst, path) ||
	    errno == ENOENT ||
	    errno == ENOTDIR)
	{
		doms.qid.erase(domain_id);
	}
	else
	{
		error_perror("unable to remove xenstore key (%s)", path);
	}

	if (commit_singleop_xs(&nretries) != XSTS_OK)
		error_perror("unable to remove xenstore key (%s)", path);
}

/*
 * Write the value if @interval to @membalance_interval_path if needed,
 * i.e. if @update_interval_in_xs = @true.
 * Reset @update_interval_in_xs to @false.
 *
 * Change protection on xenstore key @membalance_interval_path
 * to Dom0 = rw, all managed domains = r.
 *
 * On error, abort.
 */
void update_membalance_interval_and_protection(void)
{
	int nretries = 0;
	struct xs_permissions* perms = NULL;
	int nperms = doms.managed.size() + 1;
	domid2info::const_iterator it;
	int k = 0;
	domain_info* dom;

	perms = (struct xs_permissions*) malloc(nperms * sizeof(struct xs_permissions));
	if (!perms)
		out_of_memory();

	perms[0].id = 0;
	perms[0].perms = (enum xs_perm_type) (XS_PERM_READ | XS_PERM_WRITE);

	foreach_managed_domain(dom)
	{
		k++;
		perms[k].id = dom->domain_id;
		perms[k].perms = XS_PERM_READ;
	}

	for (;;)
	{
		begin_xs();

		if (update_interval_in_xs)
		{
			char buf[32];
			sprintf(buf, "%d", config.interval);
			if (!xs_write(xs, xst, membalance_interval_path, buf, strlen(buf)))
				fatal_perror("unable to write xenstore key (%s)", membalance_interval_path);
		}

		if (!xs_set_permissions(xs, xst, membalance_interval_path, perms, nperms))
			fatal_perror("unable to change xenstore key permissions (%s)", membalance_interval_path);

		switch (commit_xs(&nretries))
		{
		case XSTS_OK:	    goto cleanup;
		case XSTS_RETRY:    continue;
		case XSTS_NORETRY:  ::terminate();
		case XSTS_FAIL:     ::terminate();
		}
	}

cleanup:

	update_interval_in_xs = false;

	free(perms);
}

/*
 * Write the value if @interval to @membalance_interval_path if needed,
 * i.e. if @update_interval_in_xs = @true.
 * Reset @update_interval_in_xs to @false.
 *
 * On error, abort.
 */
void update_membalance_interval(void)
{
	int nretries = 0;
	char buf[32];

	if (!update_interval_in_xs)
		return;

	sprintf(buf, "%d", config.interval);

	begin_singleop_xs();

	if (!xs_write(xs, xst, membalance_interval_path, buf, strlen(buf)) ||
	    commit_singleop_xs(&nretries) != XSTS_OK)
	{
		fatal_perror("unable to write xenstore key (%s)",
			     membalance_interval_path);
	}

	update_interval_in_xs = false;
}


/*
 * For each domain managed by membalance read its xenstore "report" key
 * and reset the key
 */
void read_domain_reports(void)
{
	if (testmode)
	{
		test_read_domain_reports();
		return;
	}

	char report_path[256];
	int nretries = 0;
	char* p;
	unsigned int len;
	bool done = false;
	domain_info* dom;

	while (!done)
	{
		bool changed = false;

		begin_xs();

		foreach_managed_domain(dom)
		{
			free_ptr(dom->report_raw);

			sprintf(report_path, "%s/%s/report",
				membalance_domain_root_path,
				dom->qid);

			p = (char*) xs_read(xs, xst, report_path, &len);
			if (!p)
				fatal_perror("unable to read xenstore key (%s)", report_path);

			if (*p)
			{
				dom->report_raw = p;

				if (!xs_write(xs, xst, report_path, "", strlen("")))
					fatal_perror("unable to write xenstore key (%s)", report_path);

				changed = true;
			}
			else
			{
				free(p);
			}
		}

		if (!changed)
		{
			abort_xs();
			done = true;
		}
		else switch (commit_xs(&nretries))
		{
		case XSTS_OK:	    done = true; break;
		case XSTS_RETRY:    break;
		case XSTS_NORETRY:  ::terminate();
		case XSTS_FAIL:     ::terminate();
		}
	}
}

/*
 * Get Xen free memory slack (kbs)
 */
long get_xen_free_slack(void)
{
	if (testmode)
		return test_get_xen_free_slack();

	char path[256];
	long lv;

	sprintf(path, "%s/0/memory/freemem-slack", local_domain_path);
	switch (read_xs(path, &lv, 0))
	{
	case TriTrue:   return lv;
	case TriMaybe:  fatal_msg("missing xenstore key: %s", path);
	case TriFalse:  /* fall through */
	default:        ::terminate();
	}
}

/*
 * Get Xen Dom0 size
 * Returns size in kbs, aborts if Xenstore key does not exists
 */
long get_xen_dom0_target(void)
{
	long lv = get_xen_dom_target(0);
	if (lv < 0)
		fatal_msg("missing xenstore key: Dom0 memory target size");
	return lv;
}

/*
 * Get Xen domain target size, as recorded in Xenstore
 * Returns size in kbs, or -1 if the key no longer exists
 */
long get_xen_dom_target(long domain_id)
{
	char path[256];
	long lv;

	sprintf(path, "%s/%ld/memory/target", local_domain_path, domain_id);
	switch (read_xs(path, &lv, 0))
	{
	case TriTrue:   return lv;
	case TriMaybe:  return -1;
	case TriFalse:  /* fall through */
	default:        ::terminate();
	}
}

