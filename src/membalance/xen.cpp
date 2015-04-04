/*
 *  MEMBALANCE daemon
 *
 *  xen.cpp - Xen control functions
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

#include "membalanced.h"


/******************************************************************************
*                          Xen interface routines                             *
******************************************************************************/

/*
 * Resize the domain.
 * This initiates the process of domain size changing.
 *
 * Our ability to return a meaningful error code is handicapped by the loss of
 * error information inside libxl_set_memory_target.
 *
 * Currently returns:
 *
 *     0 => success
 *     -1 (errno = ESRCH) => domain does not exist
 *
 */
static int set_memory_target(domain_info* dom, long reqsize)
{
	xc_domaininfo_t info;
	long rsize;
	int rc;

	if (reqsize % memquant_kbs)
	{
		error_msg("bug: safe_libxl_set_memory_target: "
			  "target_memkb is not multiple of allocation quant");
		reqsize = roundup(reqsize, memquant_kbs);
	}

	/*
	 * The implementaion of libxl_set_memory_target(...) is such that its
	 * return status is flaky and very little can be inferred from it.
	 * Therefore we are forced to read data back from Xenstore.
	 */
	rc = libxl_set_memory_target(xl_ctx, (uint32_t) dom->domain_id, reqsize, 0, 1);

	/*
	 * Read back domain size and see if it was actually set to @target_memkb
	 */
	rsize = get_xen_dom_target(dom->domain_id);
	if (rsize == -1)       /* domain record no longer exists in Xenstore? */
		goto dead;
	dom->xs_mem_target = rsize;

	if (dom->xs_mem_target + dom->xs_mem_videoram == reqsize)
		return 0;

	/*
	 * A discreapancy exists between the requested size and the resultant target.
	 * Check if domain physically exists and is not dead.
	 */
	rc = xc_domain_getinfolist(xc_handle, (uint32_t) dom->domain_id, 1, &info);

	/* no such domain? */
	if (rc < 0 && errno == ESRCH)
		goto dead;
	if (rc < 0)
		fatal_perror("unable to get Xen domain information (xc_domain_getinfolist)");
	if (rc == 0 || info.domain != dom->domain_id)
		goto dead;
	if (info.flags & (XEN_DOMINF_dying | XEN_DOMINF_shutdown))
		goto dead;

	/*
	 * Domain is alive and still we were unable to resize it,
	 * the exact cause is buried inside libxl_set_memory_target(...)
	 */
	error_msg("unable to resize domain %s, requested size: %lu, actual size: %ld",
		  dom->printable_name(),
		  (unsigned long) reqsize,
		  dom->xs_mem_target + dom->xs_mem_videoram);

	/*
	 * What else can we do?
	 */
	return 0;

dead:
	errno = ESRCH;
	return -1;
}

/*
 * If domain is above quota, trim it down to quota.
 * Return @true if initiated trimming, otherwise @false.
 */
bool trim_to_quota(domain_info* dom)
{
	long goal;
	int rc;

	if (testmode)
		return test_trim_to_quota(dom);

	/* quota not defined? */
	if (dom->dmem_quota < 0)
		return false;

	/* if not above quota, do nothing */
	goal = roundup(dom->xs_mem_target + dom->xs_mem_videoram, pagesize_kbs);
	if (goal <= dom->dmem_quota)
		return false;

	debug_msg(5, "trimming domain %s down to quota, %ld kbs -> %ld kbs",
		      dom->printable_name(), goal, dom->dmem_quota);

	rc = set_memory_target(dom, dom->dmem_quota);

	/* if error is other than "no such domain" (anymore), then log a message */
	if (rc < 0 && errno != ESRCH)
	{
		warning_perror("unable to trim domain %s memory allocation down to its quota",
			       dom->printable_name());
	}

	if (rc < 0)
		return false;

	return true;
}

/*
 * Execute domain shrinking or expansion
 */
void do_resize_domain(domain_info* dom, long size, char action)
{
	if (testmode)
	{
		test_do_resize_domain(dom, size, action);
		return;
	}

	int rc = set_memory_target(dom, size);

	/* if error is other than "no such domain" (anymore), then log a message */
	if (rc < 0 && errno != ESRCH)
	{
		const char* verb = "change";
		switch (action)
		{
		case '+':  verb = "expand";  break;
		case '-':  verb = "shrink";  break;
		case 0:    break;
		}
		warning_perror("unable to %s memory allocation for domain %s",
			       verb, dom->printable_name());
	}
}

void do_expand_domain(domain_info* dom, long size)
{
	do_resize_domain(dom, size, '+');
}

void do_shrink_domain(domain_info* dom, long size)
{
	do_resize_domain(dom, size, '-');
}

/*
 * Wait until either Xen free memory reaches @free_target or a timeout
 * of @timeout_ms miliseconds expires.
 *
 * Return the attained amount of Xen free memory.
 */
long xen_wait_free_memory(long free_target, int timeout_ms)
{
	struct timespec ts0;
	bool ts0_set = false;
	long xfree;

	if (testmode)
		timeout_ms = 0;

	for (;;)
	{
		xfree = get_xen_free_memory();
		if (xfree >= free_target)
			return xfree;

		if (ts0_set)
		{
			if (timespec_diff_ms(getnow(), ts0) >= timeout_ms)
				return xfree;
		}
		else if (timeout_ms == 0)
		{
			return xfree;
		}
		else
		{
			ts0 = getnow();
			ts0_set = true;
		}

		usleep(100 * USEC_PER_MSEC);
	}
}

static void get_xen_physinfo(xc_physinfo_t* info)
{
	zap(*info);
	if (xc_physinfo(xc_handle, info))
		fatal_perror("unable to get Xen information (xc_physinfo)");
}

/*
 * Get free physical memory (kbs), not accounting for slack
 */
long get_xen_free_memory(void)
{
	if (testmode)
		return test_get_xen_free_memory();

	xc_physinfo_t info;
	get_xen_physinfo(&info);

	long np = info.free_pages + info.scrub_pages;
	np -= info.outstanding_pages;
	np = max(0, np);
	return np * pagesize_kbs;
}

/*
 * Get total physical memory (kbs)
 */
long get_xen_physical_memory(void)
{
	xc_physinfo_t info;
	get_xen_physinfo(&info);
	return pagesize_kbs * (long) info.total_pages;
}

/*
 * Get Dom0 minimal size (kbs)
 */
long get_xen_dom0_minsize(void)
{
	const libxl_version_info* info = libxl_get_version_info(xl_ctx);
	char* cmd = NULL;
	const char* delim_opt =  " \t";
	const char* delim_subopt =  ",";
	char* saveptr;
	char* token;
	char* dom0_mem = NULL;
	char* dom0_mem_min = NULL;
	char* dom0_mem_ini = NULL;
	const char* cp;
	long dom0_size = -1;

	if (!info)
		fatal_msg("unable to retrieve Xen information (libxl_get_version_info)");

	CHECK(info->commandline != NULL);
	cmd = xstrdup(info->commandline);

	/*
	 * Examples of command line:
	 *
	 *      com1=115200,8n1 guest_loglvl=all dom0_mem=750M console=com1
	 *      com1=115200,8n1 guest_loglvl=all dom0_mem=min:512M,max:1024M,750M console=com1
	 *
	 * dom0_mem=[min:<min_amt>,][max:<max_amt>,][<amt>]
	 * <min_amt>: The minimum amount of memory which should be allocated for dom0.
	 * <max_amt>: The maximum amount of memory which should be allocated for dom0.
	 * <amt>:     The precise amount of memory to allocate for dom0.
	 */
	for (token = strtok_r(cmd, delim_opt, &saveptr);
	     token != NULL;
	     token = strtok_r(NULL, delim_opt, &saveptr))
	{
		if (starts_with(token, "dom0_mem="))
		{
			dom0_mem = xstrdup(token + strlen("dom0_mem="));
			break;
		}
	}

	CHECK(dom0_mem);

	for (token = strtok_r(dom0_mem, delim_subopt, &saveptr);
	     token != NULL;
	     token = strtok_r(NULL, delim_subopt, &saveptr))
	{
		if (starts_with(token, "min:"))
		{
			free_ptr(dom0_mem_min);
			dom0_mem_min = xstrdup(token + strlen("min:"));
		}
		else if (*token && NULL == strchr(token, ':'))
		{
			free_ptr(dom0_mem_ini);
			dom0_mem_ini = xstrdup(token);
		}
	}

	cp = dom0_mem_min ? dom0_mem_min : dom0_mem_ini;
	CHECK(cp);

	CHECK(parse_kb("Xen boot command line", "dom0_mem", cp, "kb", &dom0_size));
	CHECK(dom0_size > 0);

cleanup:

	/*
	 * If was unable to get it from Xen command line,
	 * get current size from xenstore as a fallback
	 */
	if (dom0_size <= 0)
	{
		dom0_size = get_xen_dom0_target();
	}

	free_ptr(cmd);
	free_ptr(dom0_mem);
	free_ptr(dom0_mem_min);
	free_ptr(dom0_mem_ini);

	return dom0_size;
}

/*
 * Return domain uptime (in seconds).
 * If domain does not exist (e.g. dead), return -1.
 * May not be called for Dom0.
 *
 * Note: if a domain is restored from saved state or migrated, returned value
 *       will reflect uptime of current domain reincarnation, without prior
 *       history. This is the best we can do with current Xen.
 */
long xen_domain_uptime(long domain_id)
{
	if (domain_id == 0)
		return -1;

	if (testmode)
		return test_xen_domain_uptime(domain_id);

	uint32_t ui32_time = libxl_vm_get_start_time(xl_ctx, domain_id);
	if (ui32_time == (uint32_t) -1)
		return -1;

	long s_time = (long) ui32_time;

	struct timeval now;
	gettimeofday(&now, NULL);
	long n_time = (long) now.tv_sec;

	if (n_time < s_time)
		return 0;

	return n_time - s_time;
}

/*
 * Wait up to @timeout_ms milliseconds for Xen free memory amount to stabilize.
 * Return the amount of available free memory after the stabilization.
 * If free memory fails to stabilize within the specificed timeout period,
 * return last current free space value.
 * Hopefully one day Xen will provide memory commitments data and the need
 * for this routine will be gone.
 */
long xen_wait_free_memory_stable(int timeout_ms)
{
	if (timeout_ms <= 0 || testmode)
		return get_xen_free_memory();

	/*
	 * The implementation is very simplistic. It waits for several samples
	 * to match and return the reading. If runs out of timeout, return the
	 * latest reading.
	 *
	 * It is probably sufficient since in practice most adjustments settle
	 * after a short time.
	 *
	 * If we were in the need of more sophisticated algorithm, we could use
	 * heuristics like:
	 *
	 *   - if free space is trending to increase and is exceeding the requested
	 *     amount, return currently available space
	 *
	 *   - if free space is decreasing, but available free space adjusted for
	 *     the trend rate over say next 1 sec leaves still more than the
	 *     requested amount, then return this residual
	 *
	 *   - etc.
	 */

	struct timespec ts0 = getnow();
	long xf, prev_xf = 0;
	int match = 0;
	const int match_stable_at = 5;

	for (;;)
	{
		xf = get_xen_free_memory();

		if (match == 0 || prev_xf == xf)
		{
			match++;
		}
		else
		{
			match = 0;
		}

		prev_xf = xf;

		if (match == match_stable_at)
			return xf;

		if (timespec_diff_ms(getnow(), ts0) >= timeout_ms)
		{
			error_msg("domain memory adjustment did not stabilize after %d ms", timeout_ms);
			return xf;
		}

		usleep(100 * USEC_PER_MSEC);
	}
}

/******************************************************************************
*                         helper class: dom2xcinfo                            *
******************************************************************************/

domid2xcinfo::domid2xcinfo()
{
	info = NULL;
}

domid2xcinfo::~domid2xcinfo()
{
	free_ptr(info);
}

void domid2xcinfo::reset(void)
{
	clear();
	free_ptr(info);
}

/* collect domain information */
void domid2xcinfo::collect(void)
{
	static int max_domains = 50;
	xc_domaininfo_t* pinfo = NULL;
	int k, rc;

	reset();

	if (testmode)
	{
		rc = test_xcinfo_collect(&pinfo);
	}
	else for (;;)
	{
		free_ptr(pinfo);
		pinfo = (xc_domaininfo_t*) xmalloc(max_domains * sizeof(xc_domaininfo_t));
		rc = xc_domain_getinfolist(xc_handle, 0, max_domains, pinfo);
		if (rc < 0)
			fatal_perror("unable to get Xen domain information (xc_domain_getinfolist)");
		if (rc != max_domains)
			break;
		max_domains *= 2;
	}

	this->info = pinfo;

	/* map: dom id -> xcinfo */
	for (k = 0;  k < rc;  k++, pinfo++)
		(*this)[(long) pinfo->domain] = pinfo;
}

/* get information element for specific domain */
const xc_domaininfo_t* domid2xcinfo::get(long domain_id) const
{
	domid2xcinfo::const_iterator it = find(domain_id);
	if (it == end())
		return NULL;
	else
		return it->second;
}

