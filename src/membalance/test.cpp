/*
 *  MEMBALANCE daemon
 *
 *  test.cpp - Testing routines
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

#include "membalanced.h"


/******************************************************************************
*                         release build: no tests                             *
******************************************************************************/

#ifndef DEVEL
int execute_test(int argc, char** argv, char** message)
{
	*message = xstrdup("not a development build");
	return 1;
}

static void test_call_unexpected(const char* proc) __noreturn__;

static void test_call_unexpected(const char* proc)
{
	fatal_msg("invoking test routine in a release build: %s", proc);
}

long test_get_xen_free_memory(void)
{
	test_call_unexpected("get_xen_free_memory");
}

long test_get_xen_free_slack(void)
{
	test_call_unexpected("get_xen_free_slack");
}

bool test_trim_to_quota(domain_info* dom)
{
	test_call_unexpected("trim_to_quota");
}

void test_read_domain_reports(void)
{
	test_call_unexpected("read_domain_reports");
}

void test_do_resize_domain(domain_info* dom, long size, char action)
{
	test_call_unexpected("do_resize_domain");
}

int test_xcinfo_collect(xc_domaininfo_t** ppinfo)
{
	test_call_unexpected("xcinfo_collect");
}

void test_debugger(void)
{
}
#else


/******************************************************************************
*                           forward declarations                              *
******************************************************************************/

class pseudo_domain;
static int test_echo(int argc, char** argv, char** message);
static int test_memsched(int argc, char** argv, char** message);
static int invalid_test(char** message, const char* verb);
static long get_free_pages(const char* msg = NULL);
static void exercise_sched_freemem(void);
static void print_summary(bool use_pd_rate = false);
static pseudo_domain* find_pd(long domain_id);
static void reeval_target(domain_info* dom, pseudo_domain* pd);
static void validate_free(const char* proc);


/******************************************************************************
*                              test dispatcher                                *
******************************************************************************/

int execute_test(int argc, char** argv, char** message)
{
	if (argc == 0)
		return invalid_test(message, NULL);

	const char* verb = argv[0];
	argc--;  argv++;

	if (streq(verb, "echo"))
		return test_echo(argc, argv, message);
	else if (streq(verb, "memsched"))
		return test_memsched(argc, argv, message);
	else
		return invalid_test(message, verb);
}

static int invalid_test(char** message, const char* verb)
{
	*message = verb ? xprintf("unknown test id (%s)", verb)
	                : xstrdup("missing test id");
	return 1;
}


/******************************************************************************
*                                 echo test                                   *
******************************************************************************/

static int test_echo(int argc, char** argv, char** message)
{
	int k, len;
	char* p;

	for (len = 0, k = 0;  k < argc;  k++)
	{
		if (k)  len++;
		len += strlen(argv[k]);
	}

	p = (char*) xmalloc(len + 1);
	*message = p;

	for (k = 0;  k < argc;  k++)
	{
		if (k)  *p++ = ' ';
		strcpy(p, argv[k]);
		p += strlen(p);
	}

	*p = '\0';

	return 0;
}


/******************************************************************************
*                               memsched test                                 *
******************************************************************************/

/*
 * categories of rate and domain size (low / medium / high)
 */
typedef enum __cat
{
	C_LOW = 0,
	C_MID = 1,
	C_HIGH = 2
}
category_t;

/*
 * Categories of rate:
 *
 *     C_LOW:    RATE <= RATE_LOW
 *     C_MID:    RATE = ] RATE_LOW ... RATE_HIGH [
 *     C_HIGH:	 RATE >= RATE_HIGH
 */
inline static char rate_category_code(domain_info* dom, long rate)
{
	if (rate >= dom->rate_high)
		return 'H';
	else if (rate <= dom->rate_low)
		return 'L';
	else
		return ' ';
}


/* domain info object for testing */
class test_domain_info : public domain_info
{
public:
	test_domain_info(long domain_id) : domain_info(domain_id)
	{
		test_data.n_tick = 0;
		test_data.c_rate = (category_t) (random() % 3);
	}

	void init_test_domain(void);
	void test_tick(void);

protected:
	struct {
		long 	    n_tick;
		category_t  c_rate;
	} test_data;
};

/*
 * pseudo-domain represents simulated Xen-level domain,
 * a "physical" peer for domain_indo
 */
class pseudo_domain
{
public:
	long	domain_id;
	/* total domain allocation is @tot_pages + @outstanding_pages */
	long    tot_pages;
	long    outstanding_pages;
	char*	report;
	long	rate;

public:
	pseudo_domain(long domain_id)
	{
		this->domain_id = domain_id;
		this->report = NULL;
	}

	~pseudo_domain()
	{
		free_ptr(report);
	}
};

inline static long pages(pseudo_domain* pd)
{
	return pd->tot_pages + pd->outstanding_pages;
}

typedef	std::map<long, pseudo_domain*> domid2pd;

/*
 * state of simulated host machine
 */
static struct
{
	domid2pd  domains;
	long	  phys_pages;
	long 	  dom0_pages;
	long	  slack_kbs;
}
pseudo;

inline static long mb2kb(long mb)	{ return 1024 * mb; }
inline static long gb2kb(long gb)	{ return 1024 * 1024 * gb; }

inline static long kb2pages(long kb)	{ return roundup(kb, pagesize_kbs) / pagesize_kbs; }
inline static long mb2pages(long mb)	{ return kb2pages(mb2kb(mb)); }
inline static long gb2pages(long gb)    { return kb2pages(gb2kb(gb)); }

static struct __test_config
{
	/*
	 * flip domain rate category/general memory behavior every
	 * @domain_flip_rate_tick ticks
	 */
	int domain_flip_rate_tick;

	/*
	 * exercise sched_freemem(...) every @sched_freemem_freq ticks
	 */
	int sched_freemem_freq;
}
test_config =
{
	.domain_flip_rate_tick = 40,
	.sched_freemem_freq = 500
};

/* global (static) data */
static long test_seed;		/* stored pseudo-radomness seed */
static u_long test_cycle;	/* test cycle */

static int test_memsched(int argc, char** argv, char** message)
{
	domain_info* pdom;
	test_domain_info* dom;
	pseudo_domain* pd;
	const int ndoms = 10;
	long domain_id;
	long delta;
	int k;
	long free_pages;
	bool badarg = false;
	long arg_cycle = -1;

	/*
	 * Prepare to establish randomness
	 */
	test_seed = time(NULL) ^ getpid();

	/*
	 * Parse arguments
	 */
	if (argc == 0)
	{
		/* default args */
	}
	else if (argc == 2)
	{
		if (a2long(argv[0], &test_seed) &&
		    a2long(argv[1], &arg_cycle))
		{
			/* valid args */
		}
		else
		{
			badarg = true;
		}
	}
	else
	{
		badarg = true;
	}

	if (badarg)
	{
		*message = xstrdup("args: <seed> <cycle#>");
		return 1;
	}

	const long test_dmem_min = gb2kb(1);
	const long test_dmem_quota = gb2kb(4);
	const long test_dmem_max = gb2kb(32);

	/*
	 * Establish randomness
	 */
	srandom(test_seed);
	srand48(test_seed);

	/*
	 * Disconnect memembalanced from real system
	 */
	doms.unmanaged.clear();
	doms.managed.clear();
	doms.pending.clear();
	doms.qid.clear();
	config.interval = 1;
	memsched_pause_level = 0;

	testmode = 1;
	test_cycle = 0;

	/*
	 * RAM = 96 GB
	 * Dom0 = 1 GB
	 * Free slack = 15% of RAM
	 * host_reserved_hard = (as set in config)
	 * host_reserved_soft = (as set in config)
	 *                      or hard + 10% of (RAM - Dom0 - slack)
	 */
	pseudo.phys_pages = gb2pages(96);
	pseudo.dom0_pages = mb2pages(1024);
	pseudo.slack_kbs = (long) (0.15 * (pseudo.phys_pages * pagesize_kbs));

	if (!config.isset_host_reserved_soft())
	{
		delta = pagesize_kbs * (pseudo.phys_pages - pseudo.dom0_pages) - pseudo.slack_kbs;
		delta = max(0, delta);
		delta = (long) (delta * 0.1);
		config.set_host_reserved_soft(config.host_reserved_hard + delta);
	}

	if (config.host_reserved_hard < 0 ||
	    config.host_reserved_soft < 0 ||
	    config.host_reserved_soft < config.host_reserved_hard)
	{
		fatal_msg("bug: test_memsched: inconsistent host_reserved_soft/hard");
	}

	for (k = 0;  k < ndoms;  k++)
	{
		domain_id = k + 1;
		dom = new test_domain_info(domain_id);
		pd = new pseudo_domain(domain_id);

		dom->dmem_min = test_dmem_min;
		dom->dmem_quota = test_dmem_quota;
		dom->dmem_max = test_dmem_max;

		dom->init_test_domain();

		pd->tot_pages = kb2pages(dom->xs_mem_target) +
				kb2pages(dom->xs_mem_videoram) +
			        kb2pages(dom->xen_data_size);
		pd->outstanding_pages = 0;

		pseudo.domains[domain_id] = pd;
		doms.managed[domain_id] = dom;
		dom->on_enter_managed();
	}

	sched_slept(config.interval * 100 * MSEC_PER_SEC);

	for (;; test_cycle++)
	{
		if (arg_cycle >= 0 && test_cycle == (u_long) arg_cycle)
		{
			debug_level = 100;
			notice_msg("---------------------------");
			notice_msg("Test cycle %lu", test_cycle);
			notice_msg("Engaged maximum debug level");
			notice_msg("---------------------------");
		}

		foreach_managed_domain(pdom)
		{
			dom = (test_domain_info*) pdom;
			dom->test_tick();
		}

		if (debug_level >= 30)
		{
			notice_msg(" ");
			notice_msg("Pre sched_memory domain data:");
			notice_msg(" ");
			print_summary(true);
			notice_msg(" ");
		}

		sched_memory();

		free_pages = get_free_pages("bug: test_memsched: free pages < 0");
		if (free_pages * (long) pagesize_kbs - pseudo.slack_kbs < config.host_reserved_hard)
			fatal_msg("bug: test_memsched: below host_reserved_hard");

		if (debug_level >= 3)
			print_summary();

		/* once in a while request free memory */
		if (0 == test_cycle % test_config.sched_freemem_freq)
		{
			exercise_sched_freemem();
			if (debug_level >= 2)
				print_summary();
		}

 		/*
		 * sched_memory() does not invoke getnow() or time()
		 * other than for timestmaps in log output, so we
		 * do not need adding any intercepts for those
		 */

		// usleep(config.interval * USEC_PER_SEC);
	}

	/* NOTREACHED */

	return 0;
}

/*
 * Test sched_freemem, simulating "membalancectl free-memory" command
 */
static void exercise_sched_freemem(void)
{
	domain_info* dom;
	pseudo_domain* pd;
	long amt, kbs, aside = 0;
	int resp;
	u_quad_t freemem_with_slack = 0;
	u_quad_t freemem_less_slack = 0;

	static int flags = 0;
	flags++;

	bool above_slack = 0 != (flags & (1 << 0));
	bool draw_reserved_hard = 0 != (flags & (1 << 1));
	bool must = 0 != (flags & (1 << 2));

	/* free space */
	amt = get_free_pages("bug: exercise_sched_freemem: free pages < 0");
	amt *= pagesize_kbs;

	/* + freeable space */
	foreach_managed_domain(dom)
	{
		pd = find_pd(dom->domain_id);
		if (!pd)
			continue;

		kbs = pages(pd) * pagesize_kbs  - dom->xen_data_size;

		if (kbs > dom->dmem_min)
			amt += kbs - dom->dmem_min;
	}

	/* adjust for @above_slack */
	if (above_slack)
		aside += pseudo.slack_kbs;

	/* adjust for @draw_reserved_hard */
	if (!draw_reserved_hard)
		aside += config.host_reserved_hard;

	amt -= roundup(aside, memquant_kbs);
	if (amt <= 0)
		return;

	memsched_pause_level++;
	resp = sched_freemem(amt, above_slack, draw_reserved_hard, must, 0,
			     &freemem_with_slack,
			     &freemem_less_slack);
	memsched_pause_level--;

	if (resp != 'A')
	{
		error_msg("bug: exercise_sched_freemem: resp is %c", resp);
	}
	else if (above_slack)
	{
		if ((long) freemem_less_slack < amt)
		{
			print_summary();
			error_msg("bug: exercise_sched_freemem: freemem_less_slack (%lu) < amt (%ld)",
				  freemem_less_slack, amt);
		}
	}
	else
	{
		if ((long) freemem_with_slack < amt)
		{
			print_summary();
			error_msg("bug: exercise_sched_freemem: freemem_with_slack (%lu) < amt (%ld)",
				  freemem_with_slack, amt);
		}
	}
}

/*
 * Print summary of domain and memory status
 */
static void print_summary(bool use_pd_rate)
{
	FILE* fp = log_fp;
	domain_info* dom;
	pseudo_domain* pd;
	long kbs, free_pages, free_kbs, free_soft, free_hard;
	char sign_soft = '+';
	char sign_hard = '+';

	free_pages = get_free_pages("bug: print_summary: free pages < 0");

	free_kbs = free_pages * pagesize_kbs - pseudo.slack_kbs;
	if (free_kbs < 0)
		fatal_msg("bug: print_summary: free kbs < 0");

	fprintf(fp, "id   name   size MB.KB   rate  slow-rate\n");
	fprintf(fp, "-- -------- ----------  ------ ---------\n");

	foreach_managed_domain(dom)
	{
		pd = find_pd(dom->domain_id);
		if (!pd)
			continue;
		kbs = pages(pd) * pagesize_kbs;

		fprintf(fp, "%2ld ", dom->domain_id);
		fprintf(fp, "%-8s ", dom->vm_name);
		fprintf(fp, "%5ld.%04ld  ", kbs / 1024, kbs % 1024);
		if (use_pd_rate)
		{
			fprintf(fp, "%4ld %c    ",
				pd->rate,
				rate_category_code(dom, pd->rate));
		}
		else if (dom->valid_data)
		{
			fprintf(fp, "%4ld %c    ",
				dom->rate,
				rate_category_code(dom, dom->rate));
			fprintf(fp, "%4ld %c",
				dom->slow_rate,
				rate_category_code(dom, dom->slow_rate));
		}
		fprintf(fp, "\n");
	}

	free_hard = free_kbs - config.host_reserved_hard;
	if (free_hard < 0)
	{
		sign_hard = '-';
		free_hard = -free_hard;
	}

	free_soft = free_kbs - config.host_reserved_soft;
	if (free_soft < 0)
	{
		sign_soft = '-';
		free_soft = -free_soft;
	}

	fprintf(fp, "\n");
	fprintf(fp, "free mem:      %5ld.%04ld\n", free_kbs / 1024, free_kbs % 1024);
	fprintf(fp, "over soft: [%c] %5ld.%04ld\n", sign_soft, free_soft / 1024, free_soft % 1024);
	fprintf(fp, "over hard: [%c] %5ld.%04ld\n", sign_hard, free_hard / 1024, free_hard % 1024);
	fprintf(fp, "\n");
}

/*
 * Locate pseudo_domain by @domain_id.
 * If not found, return NULL.
 */
static pseudo_domain* find_pd(long domain_id)
{
	domid2pd::const_iterator it = pseudo.domains.find(domain_id);
	if (it == pseudo.domains.end())  return NULL;
	return it->second;
}

/*
 * Called at the start of the test
 */
void test_domain_info::init_test_domain(void)
{
	vm_name = xprintf("test-%ld", domain_id);
	vm_uuid = NULL;
	qid = xstrdup("11111111-1111-1111-1111-111111111111");

	config_file_status = TriTrue;
	ctrl_mode = ctrl_modes_allowed = CTRL_MODE_AUTO;

	xc_dmem_max = dmem_max;
	xc_dmem_quota = dmem_quota;
	xc_dmem_min = dmem_min;

	xs_mem_max = dmem_max;
	xc_maxmem = dmem_max;

	xen_data_size = 18 * pagesize_kbs;

	dmem_incr = xc_dmem_incr = config.dmem_incr;
	dmem_decr = xc_dmem_decr = config.dmem_decr;
	rate_high = xc_rate_high = config.rate_high;
	rate_low = xc_rate_low = config.rate_low;
	rate_zero = xc_rate_zero = config.rate_zero;
	guest_free_threshold = xc_guest_free_threshold = config.guest_free_threshold;
	startup_time = xc_startup_time = config.startup_time;
	trim_unresponsive = xc_trim_unresponsive = config.trim_unresponsive;
	trim_unmanaged = config.trim_unmanaged;
	xc_trim_unmanaged = (tribool) config.trim_unmanaged;

	/*
	 * Take initial memory allocation half-way between min and quota
	 */
	xc_memory = (dmem_min + dmem_quota) / 2;
	xc_memory = roundup(xc_memory, pagesize_kbs);	/* G + V*/

	xs_mem_videoram = mb2kb(16);    		/* V */
	xs_mem_target = xc_memory - xs_mem_videoram;    /* (G + V) - V */
}

/*
 * Called at each test tick
 */
void test_domain_info::test_tick(void)
{
	double xrate = 0;
	double fx = drand48();

	/*
	 * once in a while flip rate between low-mid-high ranges
	 */
	if ((++test_data.n_tick % test_config.domain_flip_rate_tick) == 0)
		test_data.c_rate = (category_t) (random() % 3);

	switch (test_data.c_rate)
	{
	case C_LOW:
		xrate = rate_low * fx * 0.99;
		break;
	case C_MID:
		xrate = rate_low + (rate_high - rate_low) * (0.1 + 0.8 * fx);
		break;
	case C_HIGH:
		xrate = rate_high * (1.01 + fx * 10);
		break;
	}

	pseudo_domain* pd = find_pd(domain_id);
	if (!pd)
		fatal_msg("bug: test_tick: missing pseudo-domain");

	char struct_version = 'A';
	const char* xprogname = "memprobed";
	const char* xprogversion = "0.1";
	unsigned long long report_seq = (unsigned long) test_data.n_tick;
	long kbs_sec = (long) xrate;
	long kbs = kbs_sec * config.interval;
	double free_pct = 5.0;

	free_ptr(pd->report);
	pd->report = xprintf(
		    "%c\n"
		    "action: report\n"
		    "progname: %s\n"
		    "progversion: %s\n"
		    "seq: %llu\n"
		    "kb: %lu\n"
		    "kbsec: %lu\n"
		    "freepct: %f\n",
		struct_version,
		xprogname,
		xprogversion,
		report_seq++,
		(unsigned long) kbs,
		(unsigned long) kbs_sec,
		free_pct);

	pd->rate = kbs_sec;
}


/******************************************************************************
*                                 intercepts                                  *
******************************************************************************/

void test_debugger(void)
{
	if (testmode)
	{
		fprintf(stderr, "\n");
		fprintf(stderr, "test_debugger: breaking to debugger ...\n");
		fprintf(stderr, "test_seed = %ld, test_cycle = %lu\n", test_seed, test_cycle);
		fflush(stderr);

		raise(SIGTRAP);
		// __builtin_trap();
	}
}

/*
 * Get the number of free pages remaining in a pseudo-host.
 */
static long get_free_pages(const char* msg)
{
	long free_pages = pseudo.phys_pages - pseudo.dom0_pages;

	for (domid2pd::const_iterator it = pseudo.domains.begin();
	     it != pseudo.domains.end();
	     ++it)
	{
		pseudo_domain* pd = it->second;
		free_pages -= pages(pd);
	}

	if (msg && free_pages < 0)
		fatal_msg("%s", msg);

	return free_pages;
}

/*
 * Intercept for get_xen_free_memory(...)
 */
long test_get_xen_free_memory(void)
{
	long free_pages;

	free_pages = get_free_pages("bug: test_get_xen_free_memory: free memory < 0");
	return free_pages * pagesize_kbs;
}

/*
 * Intercept for get_xen_free_slack(...)
 */
long test_get_xen_free_slack(void)
{
	return pseudo.slack_kbs;
}

/*
 * Intercept for read_domain_reports(...)
 */
void test_read_domain_reports(void)
{
	domain_info* dom;
	pseudo_domain* pd;

	foreach_managed_domain(dom)
	{
		free_ptr(dom->report_raw);
		pd = find_pd(dom->domain_id);
		if (pd && pd->report && pd->report[0])
		{
			dom->report_raw = pd->report;
			pd->report = NULL;
		}
	}
}

/*
 * Intercept for domid2xcinfo::collect(...)
 */
int test_xcinfo_collect(xc_domaininfo_t** ppinfo)
{
	int ndoms = pseudo.domains.size();
	xc_domaininfo_t* pinfo;

	pinfo = (xc_domaininfo_t*) xmalloc(ndoms * sizeof(xc_domaininfo_t));
	memset(pinfo, 0, ndoms * sizeof(xc_domaininfo_t));
	*ppinfo = pinfo;

	for (domid2pd::const_iterator it = pseudo.domains.begin();
	     it != pseudo.domains.end();
	     ++it)
	{
		pseudo_domain* pd = it->second;
		pinfo->domain = pd->domain_id;
		pinfo->flags = XEN_DOMINF_running;
		pinfo->tot_pages = pd->tot_pages;
		pinfo->outstanding_pages = pd->outstanding_pages;
		pinfo++;
	}

	return ndoms;
}

/*
 * Intercept for trim_to_quota(...)
 */
bool test_trim_to_quota(domain_info* dom)
{
	long curr_memsize, delta, dx;

	/* quota not defined? */
	if (dom->dmem_quota < 0)
		return false;

	pseudo_domain* pd = find_pd(dom->domain_id);
	if (!pd)
		return false;

	/* if not above quota, do nothing */
	curr_memsize = pages(pd) * pagesize_kbs - dom->xen_data_size;
	if (curr_memsize <= dom->dmem_quota)
		return false;

	debug_msg(5, "trimming domain %s down to quota, %ld kbs -> %ld kbs",
		      dom->printable_name(), curr_memsize, dom->dmem_quota);

	if ((dom->dmem_quota % pagesize_kbs) ||
	    (dom->dmem_min % pagesize_kbs) ||
	    (dom->xen_data_size % pagesize_kbs))
	{
		fatal_msg("bug: test_trim_to_quota: invalid domain settings");
	}

	delta = curr_memsize - dom->dmem_quota;
	if (delta <= 0 || (delta % pagesize_kbs))
		fatal_msg("bug: test_trim_to_quota: not multiple of pages");
	delta /= pagesize_kbs;

	dx = min(delta, pd->outstanding_pages);
	pd->outstanding_pages -= dx;
	delta -= dx;

	dx = min(delta, pd->tot_pages);
	pd->tot_pages -= dx;
	delta -= dx;

	if (delta > 0)
		fatal_msg("bug: test_trim_to_quota: invalid trim (delta > 0)");

	curr_memsize = pages(pd) * pagesize_kbs - dom->xen_data_size;
	if (curr_memsize < dom->dmem_min)
		fatal_msg("bug: test_trim_to_quota: invalid trim (curr_memsize < dom->dmem_min)");

	reeval_target(dom, pd);

	validate_free("test_trim_to_quota");

	return true;
}

/*
 * Intercept for do_resize_domain(...)
 */
void test_do_resize_domain(domain_info* dom, long size, char action)
{
	pseudo_domain* pd = find_pd(dom->domain_id);
	if (!pd)
		return;

	long xsize, npg, xpg, free_pages, pages_pd;

	xsize = size + dom->xen_data_size;
	if (xsize % memquant_kbs)
		fatal_msg("bug: test_do_resize_domain: size not multiple of pages");

	/* new domain size-allocation (in pages) */
	npg = xsize / pagesize_kbs;
	pages_pd = pages(pd);

	/* validate the intent vs. actual size */
	switch (action)
	{
	case '+':
		/* caller intends to expand */
		if (npg < pages_pd)
			error_msg("bug: test_do_resize_domain: expanding to a smaller size");
		break;

	case '-':
		/* caller intends to shrink */
		if (npg > pages_pd)
			error_msg("bug: test_do_resize_domain: shrinking to a larger size");
		break;

	case 0:
		break;
	}

	/* perform the request */
	if (npg > pages_pd)
	{
		/* do expand */
		free_pages = get_free_pages("bug: test_do_resize_domain: free_pages < 0");

		/* extra pages needed to grow */
		xpg = npg - pages_pd;
		xpg = min(xpg, free_pages);
		pd->tot_pages += xpg;
	}
	else if (npg < pages_pd)
	{
		/* do shrink */
		pd->tot_pages = npg;
		pd->outstanding_pages = 0;
	}
	else
	{
		/* keep size: do nothing */
	}

	reeval_target(dom, pd);
	validate_free("test_do_resize_domain");
}

/* reevaluate dom->xs_mem_target */
static void reeval_target(domain_info* dom, pseudo_domain* pd)
{
	dom->xs_mem_target = pages(pd) * pagesize_kbs -
		              dom->xen_data_size -
			      dom->xs_mem_videoram;
}

/* validate the amount of remaining free space */
static void validate_free(const char* proc)
{
	long fpg = get_free_pages();
	if (fpg < 0)
		fatal_msg("bug: %s: free_pages < 0", proc);
	if (fpg * pagesize_kbs < roundup(pseudo.slack_kbs + config.host_reserved_hard, pagesize_kbs))
		error_msg("bug: %s: free < slack + reserved_hard", proc);
}

/*
 * Intercept for xen_domain_uptime(...)
 */
long test_xen_domain_uptime(long domain_id)
{
	/* returned value if not too important for this test purposes */
	return 1;
}

#endif // DEVEL

