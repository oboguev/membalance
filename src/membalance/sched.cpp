/*
 *  MEMBALANCE daemon
 *
 *  sched.cpp - Memory allocation scheduler
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

#include "membalanced.h"


/******************************************************************************
*                            local declarations                               *
******************************************************************************/

class eval_force_context
{
public:
	/*
	 * Highest data rate in the domain set.
	 */
	long rmax;

	/*
	 * Base value for force function, indexed as
	 *
	 *     base[rate_category][size_category]
	 */
	double base[3][3];

public:
	void eval_resist_force_context(const domvector& vec, int vk1, int vk2);
	void eval_expand_force_context(const domvector& vec, int vk1, int vk2);

protected:
	void pre_eval(void)
	{
		/* fill in with "undefined" */
		rmax = 0;
	}

	void post_eval(void)
	{
	}
};

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


/******************************************************************************
*                               static data                                   *
******************************************************************************/

/*
 * Algorithm tick count, incremented on each invocation of sched_memory(...).
 * Start with non-zero, so we can use 0 to designate "never seen" and it
 * also won't fall into any "xxx ticks back" calculation schemes.
 */
static unsigned long sched_tick = 1000;

/*
 * Current amount of free memory (less slack), can be negative if
 * a part of slack if allocated
 */
static long host_free;

/*
 * Xen free memory, as read by stage_collect_data().
 */
static long xen_free0;

/*
 * Host memory outstanding lien, as calculated by stage_collect_data().
 */
static long host_lien0;

/*
 * map: domain id -> xc_domaininfo_t*
 */
static domid2xcinfo id2xcinfo;

/*
 * Contraction-resist force of "soft" free space
 * between host_reserved_soft and host_reserved_hard.
 */
static const double resist_force_free_soft = 45;

/* floating point resolution */
static const double eps = DBL_EPSILON;


/******************************************************************************
*                           forward declarations                              *
******************************************************************************/

static void stage_collect_data(void);
static void record_memory_info(domain_info* dom, const xc_domaininfo_t* xcinfo);
static void reset_preshrink(domain_info* dom);
static void sched_reserved_hard(void);
static long hard_reclaim(long goal);
static void hard_reclaim_round_1(long& goal);
static void hard_reclaim_round_2(long& goal);
static void hard_reclaim_round_3(long& goal);
static void hard_reclaim_round_4(long& goal);
static void hard_reclaim_round_5(long& goal);
static void eval_resist_force(const domvector& vec);
static void eval_resist_force(const domvector& vec, int vk1, int vk2);
static void eval_resist_force(eval_force_context& ctx, domain_info* dom);
static void eval_expand_force(const domvector& vec);
static void eval_expand_force(const domvector& vec, int vk1, int vk2);
static void eval_expand_force(eval_force_context& ctx, domain_info* dom);
static void sched_reserved_soft(void);
static long soft_reclaim(long goal);
static void soft_reclaim_round_1(long& goal);
static void soft_reclaim_round_2(long& goal);
static void soft_reclaim_round_3(long& goal);
static void sched_rebalance(void);
static bool expand_into_freemem(domain_info* dom, long need);
static void rebalance_domains(domain_info* dom,
			      long need,
			      eval_force_context& resist_force_context,
			      domvector& vec_shrink);
static long free_allocate(double expand_force, long need);
static void insert_into_vec_expand(domvector& vec, domain_info* dom);
static void insert_into_vec_shrink(domvector& vec, domain_info* dom);
static void do_resize_domains(void);
static void unrecognized_domain_state(const xc_domaininfo_t* xcinfo);
static void log_resize(domain_info* dom, const char* action);
static void log_unexpanded(domvector& vec_up, bool warn, bool partial, long prev_goal);
static long mem_released_by(const domvector& vec_down, const domid2xcinfo& xinfo);
static long eval_allocate(domain_info* dom, long curr_size, long prev_alloc,
			  const domvector& vec_down,
			  const domid2xcinfo& xinfo,
			  long allocated);
static void regoal(domain_info* dom, long size);
static long mem_shortage(domvector& vec_up, bool partial, long prev_goal);
static long eval_memory_lien(void);
static void print_plan(const domvector& vec_down, const domvector& vec_up);
static void print_reclaimed(void);


/******************************************************************************
*                                   inlines                                   *
******************************************************************************/

/*
 * Check if domain is in regular runnable state (i.e. running or blocked)
 */
inline static bool runnable(const xc_domaininfo_t* xcinfo)
{
	if (xcinfo->flags & (XEN_DOMINF_dying |
			     XEN_DOMINF_shutdown |
			     XEN_DOMINF_paused))
	{
		return false;
	}
	else if (false && !(xcinfo->flags & (XEN_DOMINF_blocked | XEN_DOMINF_running)))
	{
		/*
		 * Synchroniation inside Xen getdomaininfo(...) is imperfect,
		 * so sometimes we'll see domains that are neither running
		 * nor blocked, nor dying, nor shutdown, nor paused. This
		 * reflects a transition state, most likely between blocked
		 * and running or vice versa. It is still good for us as
		 * runnable, so this else-if branch is disabled.
		 */
		unrecognized_domain_state(xcinfo);
		return false;
	}
	else
	{
		return true;
	}
}

inline static bool runnable(domain_info* dom)
{
	return runnable(dom->xcinfo);
}

static void unrecognized_domain_state(const xc_domaininfo_t* xcinfo)
{
	long domain_id = xcinfo->domain;
	domain_info* dom = doms.managed[domain_id];
	warning_msg("Domain %s is in an unrecognized state (xc flags = 0x%x)",
		    dom->printable_name(),
		    (unsigned int) xcinfo->flags);
}

/*
 * Categories of rate:
 *
 *     C_LOW:    RATE <= RATE_LOW
 *     C_MID:    RATE = ] RATE_LOW ... RATE_HIGH [
 *     C_HIGH:	 RATE >= RATE_HIGH
 */
inline static category_t rate_category(domain_info* dom, long rate)
{
	if (rate >= dom->rate_high)
		return C_HIGH;
	else if (rate <= dom->rate_low)
		return C_LOW;
	else
		return C_MID;
}

/*
 * Categories of domain size for contraction-resistance force:
 *
 *     C_LOW:    DMEM_SIZE <= DMEM_MIN
 *     C_MID:    RATE = ] DMEM_MIN ... DMEM_QUOTA ]
 *     C_HIGH:	 DMEM_SIZE > DMEM_QUOTA
 *
 */
inline static category_t size_resist_category(domain_info* dom, long size)
{
	if (size > dom->dmem_quota)
		return C_HIGH;
	else if (size <= dom->dmem_min)
		return C_LOW;
	else
		return C_MID;
}

/*
 * Categories of domain size for expansion force:
 *
 *     C_LOW:    DMEM_SIZE < DMEM_MIN
 *     C_MID:    RATE = [ DMEM_MIN ... DMEM_QUOTA [
 *     C_HIGH:	 DMEM_SIZE >= DMEM_QUOTA
 *
 */
inline static category_t size_expand_category(domain_info* dom, long size)
{
	if (size >= dom->dmem_quota)
		return C_HIGH;
	else if (size < dom->dmem_min)
		return C_LOW;
	else
		return C_MID;
}

/*
 * Check if domain is protected from shrinking in the current tick due to been
 * just recently expanded. This protection applies only against meeting
 * @host_reserved_soft constraint and domain rebalancing, but not against
 * meeting @host_reserved_hard constraint.
 */
inline static bool is_shrink_soft_protected(domain_info* dom)
{
	return (sched_tick - dom->last_expand_tick) <= config.shrink_protection_time;
}


/******************************************************************************
*                             main scheduling routine                         *
******************************************************************************/

/*
 * Memory scheduling algorithm dynamically balances host memory allocation
 * between participating domains according to their current memory demand
 * measured as domain data map-in rate (hard page faults rate + file system
 * cache block read-in rate, kb/sec) over current sampling interval.
 *
 * The algorithm is invoked every INTERVAL seconds - but only if there is
 * at least one currently managed domain. If there are no managed domains,
 * the algorithm is not invoked. (Once there appear managed domains, tick
 * count is incremented accordingly to refelect elapsed sleep time.)
 *
 * If automatic domain memory allocation is paused by system administrator,
 * the algorithm is invoked only to collect data (stage 1 as described below),
 * but does not perform any adjustments (stages 2-4 are not invoked).
 *
 * The algorithm tries to provide good memory-supply responsivness for domains
 * that need to expand, while avoiding premature domain contraction and
 * upsize-downsize thrashing. The algorithm's motto is "expand fast, contract
 * slow" (wherever possible at all). To this end:
 *
 *     - Algorithm allows domains to grow in a time of plenty and trims
 *       domains only when there is actual demand for memory. When there
 *       is no demand, domains are allowed to keep memory allocated to them.
 *
 *     - Algorithm reserves a part of memory (defined by HOST_RESERVED_SOFT)
 *       for domains in a substantial need of memory, otherwise keeping it
 *       free and immediatelly available for expansion of a domain when it
 *       comes in a substantial need of extra memory.
 *
 *     - Algorithm employs SLOW_RATE moving average to track recent history
 *       of domain's data map-in rate in order to avoid premature contraction
 *       of a domain just because it did not show memory demand for a tick
 *       or two.
 *
 *     - Maximum domain expansion amount at a normal interval (@dmem_incr)
 *       is larger by default than trimming amount (@dmem_decr), unless the
 *       memory is in very short supply, albeit the values are aqjustable
 *       by system administrator.
 *
 * The algorithm uses the following data items.
 *
 * Global dynamic values:
 *
 *     HOST_FREE = current amount of host free memory
 *
 * Global settings:
 *
 *     INTERVAL = interval between successive invocations of the algorithm
 *
 *     HOST_RESERVED_HARD = host memory to leave alone and never consume
 *      		    for allocation during domain size auto-adjustment
 *      		    by membalance:
 *
 *      	            1) membalance will not expand existing domains if such an
 *      		       expansion would leave HOST_FREE below HOST_RESERVED_HARD
 *
 *      	            2) if membalance detects that HOST_FREE dropped below
 *      		       HOST_RESERVED_HARD (e.g. because a new virtual machine
 *      		       has been launched or a domain was expanded manually
 *      		       by the system administrator) membalance will try to
 *      		       reclaim memory from domains managed by membalance to
 *      		       bring HOST_FREE up to HOST_RESERVED_HARD again
 *      		       or as close to it as possible for membalance
 *
 *      		    this amount is in addition to (on top of)
 *      		    Xen free memory slack
 *
 *     HOST_RESERVED_SOFT = host memory between HOST_RESERVED_SOFT and HOST_RESERVED_HARD
 *      		    is made available only for domains in significant need of memory
 *
 *      		    1) domains with DMEM_SIZE < DMEM_QUOTA and RATE > RATE_LOW
 *
 *      		    2) domains any size with data map-in rate >= RATE_HIGH
 *
 *      		    as follows:
 *
 *      		    1) if HOST_FREE <= HOST_RESERVED_SOFT, domain can
 *      		       only expand beyond DMEM_QUOTA when its data
 *      		       map-in RATE >= RATE_HIGH
 *
 *      		       this is the only constraint HOST_RESERVED_SOFT
 *      		       imposes on domain expansion
 *
 *      		       if domain size is below DMEM_QUOTA, domain still
 *      		       can expand up to DMEM_QUOTA if its RATE > RATE_LOW,
 *      		       regardless of HOST_RESERVED_SOFT
 *
 *      		    2) if HOST_FREE drops below (<) HOST_RESERVED_SOFT,
 *      		       membalance will try to shrink those domains that
 *      		       have rate < RATE_HIGH and size above their DMEM_QUOTA
 *      		       down to but not below DMEM_QUOTA, and will also
 *      		       try to shrink domains sized below QUOTA and having
 *      		       RATE <= RATE_LOW
 *
 *      		    in other words, a domain can expand above DMEM_QUOTA
 *      		    at the expense of free memory only if its RATE >= RATE_HIGH
 *      		    *and* FREE > FREE_RESERVED_SOFT
 *
 *      		    note that if FREE memory drops down to FREE_RESERVED_SOFT,
 *      		    a domain can still expand at the expense of shrinking other domains
 *      		    (rather than consuming free memory), as explained below in the
 *      		    description of stage 4
 *
 *      		    also, if free memory drops down to FREE_RESERVED_SOFT,
 *      		    membalanced will initiate shrinking of domains, as explained
 *      		    in (2) above and in stage 3 description below
 *
 *      		    as a cumulative outcome, it ensures that memory beyond
 *      		    HOST_RESERVED_SOFT (but constrained by HOST_RESERVED_HARD)
 *      		    will be allocated only to domains (a) within their DMEM_QUOTA
 *      		    once they needs it (RATE > RATE_LOW) and (b) domains beyond
 *      		    DMEM_QUOTA but only having RATE >= RATE_HIGH
 *
 *      		    this amount is in addition to (on top of) Xen free memory slack
 *
 * Per-domain dynamic values:
 *
 *     DMEM_SIZE = currently allocated memory size
 *
 *     RATE = reported data map rate for the current cycle
 *
 *            note that memory demand reporting by domains is not exactly
 *            synchronized between domains (synchronized only up to approximately
 *            INTERVAL seconds) and not synchronized with execution of memory
 *            balancing algorithm in membalanced daemon (also synchronized only
 *            up to approximately INTERVAL seconds), therefore when membalanced
 *            executes its next cycle, RATE value may have not been reported by
 *            a domain yet, or reported value may reflect RATE value reported
 *            on the basis of paging/file cache read partly before the previous
 *            adjustment and partly after it (if domain size adjustment has
 *            been performed during the previous algorithm tick)
 *
 *            therefore:
 *
 *            if no value has been reported for the current tick, value reported
 *            in the previous cycle is re-used
 *
 *            for this reason values of DMEM_INCR and DMEM_DECR should be moderate,
 *            typically not bigger than 5% and certainly not over 10%, to prevent
 *            rapid swings and overreaction of membalance mechanism based on
 *            incomplete input
 *
 *            if domain reported no value for over two cycles, it is excluded
 *            from normal participation in membalance (consideration for shrinking
 *            or expanding) until it starts reporting rate value again
 *
 *            membalance can still shrink non-reporting domain but only as the last
 *            resort measure when trying to meet the goals of HOST_RESERVED_HARD
 *            or manual memory request by system administrator and all other means
 *            of memory extraction have been exhausted (i.e. all participating
 *            domains have been shrunk to their DMEM_MIN size)
 *
 * Per-domain settings:
 *
 *     DMEM_MAX   =  maximum size a domain can be expanded to
 *
 *      	     this approximates domain size on a memory-abundant system
 *      	     in case a domain keeps creating data map-in load;
 *
 *      	     this is a practical domain size limit for high-RATE domains
 *
 *     DMEM_QUOTA =  memory quota for a domain
 *
 *      	     quota setting approximates domain size on a memory-constrained
 *      	     system in case a domain keeps creating data map-in load;
 *
 *      	     this is a practical domain size limit for low- or mid-RATE domains
 *
 *     DMEM_MIN   =  minimum size that domain can be shrunk to in case of memory shortage
 *
 * 		     low-rate domains are likely to be sized between DMEM_MIN and DMEM_QUOTA
 *
 *      	     this is a practical domain size for very low-RATE domains
 *
 *     RATE_HIGH  =  high-rate threshold for data map-in rate (kb/s)
 *
 *     RATE_LOW   =  low-rate threshold for data map-in rate (kb/s)
 *
 *     RATE_ZERO  =  data map-in rate <= RATE_ZERO is considered to be
 *                   the same as zero rate and shall not cause domain expansion
 *
 *     DMEM_INCR  =  memory amount to expand domain by (as % of current allocation)
 *
 *     DMEM_DECR  =  memory amount to shrink domain by (as % of current allocation)
 *
 *     GUEST_FREE_THRESHOLD = if guest free memory (as a percentage of guest
 *      		      total system memory) is > GUEST_FREE_THRESHOLD,
 *      		      regard domain's data map-in rate as zero, regardless
 *      		      of the reported rate
 *
 * Memory balancing algorithm runs periodically (as defined by the INTERVAL setting,
 * unless paused by system manager or there are no domains to manage).
 *
 * At each algorithm tick, the algorithm executes in four stages:
 *
 * Stage 1 (data collection):
 *
 *     - Collect data from the domains: for each domain managed by membalance read
 *       its xenstore "report" key and reset the key.
 *
 *     - If in-domain guest operating system has plenty of free memory
 *       (reported guest free memory > GUEST_FREE_THRESHOLD percent of total
 *       guest system memory), treat reported data map-in rate as 0,
 *       regardless of the reported rate value.
 *
 *     - If domain data map-in rate is <= RATE_ZERO, treat reported data map-in
 *       rate as 0, regardless of the reported rate value.
 *
 *     - Disregard domains in the following states: paused, crashed, dying, shutdown.
 *       Only manage domains in the states: running, blocked.
 *
 *     - Disregard non-participating domains (membalance = off).
 *
 *     - Disregard currently non-participating domains (that have not reported data
 *       within more than one cycle/tick).
 *
 *     - If domain has not reported data for the current cycle, presume reading to be the
 *       same as for the last cycle.
 *
 *     - If parameter TRIM_UNRESPONSIVE is set and domain did not provide rate
 *       data reports for over TRIM_UNRESPONSIVE seconds, and current domain size is
 *       over DMEM_QUOTA, the domain is trimmed town to DMEM_QUOTA.
 *
 *     - Calculate values of SLOW_RATE and FAST_RATE.
 *
 *       These are the values of RATE that have the history of RATE factored into
 *       them and are used further to compute domain pressure force to expand, and
 *       also domain force of resistance to contraction. We use these effective
 *       values of rate with history factored into them instead of just the latest
 *       reading of RATE so the algorithm stays tuned to domain rate trend and is
 *       not thrown off by temporarily and transient abrupt in rate reading for
 *       just one or two ticks.
 *
 *       FAST_RATE is used to calculate domain pressure force to expand.
 *       SLOW_RATE is used to calculate domain force to resist contraction.
 *
 *       The value of SLOW_RATE is defined as:
 *
 *           SLOW_RATE = max(slow_moving_average(RATE), RATE)
 *
 *       Slow moving average is taken over a number of recent samples.
 *       In no event SLOW_RATE is less than the current RATE reading.
 *
 *       Right now moving average for SLOW_RATE is hardwired to be taken over
 *       the readings for 5 recent intervals, with decreasing weights for older
 *       intervals. In the future it may be made tunable both on global and
 *       per-domain basis to cover a longer history than currently used five
 *       recent ticks data.
 *
 *       Currently, heuristically, FAST_RATE is just RATE, i.e. most recent
 *       reading of RATE. In the future the formula for calculating FAST_RATE
 *       may me modified to temper down the initial response for infrequent
 *       one-interval intermittent spikes.
 *
 * If automatic memory adjustment is paused by system administrator, further
 * stages are not performed.
 *
 * Stage 2 (free memory HARD constraint):
 *
 *     Meet HOST_RESERVED_HARD constraint.
 *
 *     If HOST_FREE dropped below HOST_RESERVED_HARD, try to squeeze managed
 *     domains to bring HOST_FREE back to HOST_RESERVED_HARD or as close to it
 *     as possible.
 *
 *     This stage is performed in several rounds, executed until either the target
 *     for HOST_FREE getting back to HOST_RESERVED_HARD is met, or all rounds are
 *     exhausted and membalanced is still unable to meet the target for
 *     HOST_RESERVED_HARD.
 *
 *     Rounds start with trimming down domains that are least likely to suffer
 *     as the result of the trimming and proceed towards domain that are likely
 *     to suffer more heavily if trimmed.
 *
 *     Round 1. Select domains with RATE <= RATE_LOW and sort them by the time
 *     they had rate <= RATE_LOW. Starting from domain that had low rate for
 *     longest time and towards domain that had it for shortest time, trim
 *     each domain by up to DMEM_DECR (but never below DMEM_MIN).
 *
 *     Round 2. Select domains with RATE < RATE_HIGH and size > DMEM_QUOTA,
 *     excepting domains already trimmed in the previous round, and sort them
 *     by the time they had rate < RATE_HIGH. Starting from domain that had
 *     rate < RATE_HIGH for longest time and towards domain that had it for
 *     shortest time, trim each domain by up to DMEM_DECR (but never below
 *     DMEM_QUOTA).
 *
 *     Round 3. If still cannot meet the target, select domains with RATE < RATE_HIGH
 *     and size > DMEM_QUOTA, regardless of whether they were already trimmed in the
 *     previous rounds, and sort them by the time they had rate < RATE_HIGH. Starting
 *     from domain that had rate < RATE_HIGH for longest time and towards domain that
 *     had it for shortest time, and regarless of any previous trimming amount, trim
 *     each domain [additionally] by up to DMEM_DECR (but never below DMEM_QUOTA)
 *     on top of any previous trimming.
 *
 *     Round 4. Calculate pressure-resistance function (as described below in the
 *     write-up for stage 4) for every domain above DMEM_QUOTA (the advisory value
 *     of RATE we use in this calculation is made not perfectly valid by previous
 *     contractions, but this is the best we can do at this point). Sort the list
 *     by the value of the function. Starting from lowest-pressure domain towards
 *     highest-pressure domains, trim each domain by up to DMEM_DECR (but not
 *     brining it below DMEM_QUOTA) until the deficit is satisfied.
 *
 *     Treat domains that have not reported their rate lately as having zero rate.
 *
 *     If was unable to satisfy the deficit within one pass, repeat passes until
 *     either the deficit is satisfied or all domains are brought down to
 *     DMEM_QUOTA.
 *
 *     Round 5. If the deficit is still not satisfied at this point, try to
 *     satisfy it by gradually trimming domains from DMEM_QUOTA towards DMEM_MIN.
 *     Recalculate pressure-resistance function for each domain using the last
 *     sampled RATE value for this domain (this is suboptimal, but the best we
 *     can do at this point). Starting from the lowest-pressure domain towards
 *     highest-pressure domain, trim each domain by up to DMEM_DECR (but not
 *     brining it below DMEM_MIN) until the deficit is satisfied.
 *
 *     Treat domains that have not reported their rate lately as having zero rate,
 *     except if a domain is very young (domain uptime is less than @startup_time --
 *     a parameter assignable on a per-domain basis but also having global default),
 *     in which case the domain is given a benefit of the doubt and ascribed the
 *     rate just above @rate_high.
 *
 *     If was unable to satisfy the deficit within one pass, repeat passes until
 *     either the deficit is satisfied or all domains are brought down to
 *     DMEM_MIN.
 *
 *     If deficit is still not satisfied at this point, there is nothing further
 *     membalance can do, since all domains are now down to their DMEM_MIN size,
 *     and membalance may not try to trim domains any further. If all domains are
 *     already at their DMEM_MIN or below, quit the algorithm.
 *
 * Stage 3 (free memory SOFT constraint):
 *
 *     Try to meet HOST_RESERVED_SOFT constraint.
 *
 *     If HOST_FREE dropped below HOST_RESERVED_SOFT, try to trim managed
 *     domains to bring HOST_FREE back to HOST_RESERVED_SOFT or as close to it
 *     as possible.
 *
 *     Unlike the adjustment for HOST_RESERVED_HARD, the adjustment to
 *     HOST_RESERVED_SOFT does not have to be performed instantaneously and
 *     can be performed gradually over a number of cycles (algorithm ticks).
 *
 *     Unlike in HOST_RESERVED_HARD stage, trimming of domains within a single
 *     tick is limited and no excessive trimming is performed to reach the
 *     HOST_RESERVED_SOFT target. Rather, the intent is to reach it gradually
 *     over a number of ticks by only a moderate trimming during each tick.
 *
 *     This stage is executed in up to three rounds.
 *
 *     Round 1. Select domains with RATE <= RATE_LOW and size > DMEM_QUOTA.
 *     Sort the list by the time domain RATE was <= RATE_LOW. Starting
 *     from the domains that had low rate for longest time, and towards
 *     domains that had it for shortest time, trim each by up to DMEM_DECR
 *     (but not below DMEM_QUOTA) until the deficit is satisfied (i.e.
 *     HOST_FREE is increased up to HOST_RESERVED_SOFT).
 *
 *     If domain has already been previously trimmed in current algorithm tick,
 *     it can only be trimmed at this stage to the extent that the total trim
 *     does not exceed DMEM_DECR.
 *
 *     Round 2. If deficit is still not satisifed, repeat in a similar
 *     fashion with domains with RATE <= RATE_LOW but size <= DMEM_QUOTA.
 *     In this round domains are trimmed by up to DMEM_DECR but not below
 *     DMEM_MIN. Similarly to the previous round, trimming is limited by
 *     DMEM_DECR including the trim applied previously within the same tick.
 *
 *     Round 3. If deficit is still not satisifed, repeat in a similar
 *     fashion with domains with RATE < RATE_HIGH and size > DMEM_QUOTA.
 *
 *     If still unable to satisfy the deficit, leave it off at this point and
 *     try again at next tick.
 *
 *     To minimize the probability of a jitter of reallocating memory back and
 *     forth between very similar domains (domain upsize/downsize jitter),
 *     a domain that has been expanded not more than @shrink_protection_time
 *     ticks back is not considered elgigible for shrinking at stages 3 and 4
 *     (stages "meeting soft free memory constraint" and "domain rebalancing").
 *     However this protection is not in effect for stage 2 (stage "meeting
 *     hard free memory constraint").
 *
 *     Outlined stage 3 algorithm can also be expressed in terms of effective
 *     pressure function for free memory region (see below in stage 4 write-up).
 *
 *     Future enhancements to the algorithm may also take into account longer
 *     term RATE history for a domain in a more sophisticated way, e.g. its
 *     averaging over long-term period and absolute values of rate.
 *
 * Stage 4 (domain balancing):
 *
 *     Perform dynamic domain expansion or contraction, according to current
 *     memory demand (pressure) of specific domains.
 *
 *     Domains with high memory pressure inside them can be expanded by using
 *     free memory if available or at the cost of shrinking domains with low
 *     memory pressure.
 *
 *     Domains are always expanded at the expense of available free memory first
 *     if free memory is available, and are expanded at the cost of shrinking
 *     other domains only if free memory is unavailable (including due to the
 *     constraints imposed by HOST_RESERVED_SOFT and HOST_RESERVED_HARD).
 *
 *     There are two memory pressure functions calculated for each domain:
 *
 *     Pressure-out (exert) function indicates "outward-directed" force for
 *     the expansion of this domain.
 *
 *     Pressure-resistance function indicates how strongly a domain resists an
 *     attempt to contract it.
 *
 *     (By the way of an example, consider an analogy in the physical world:
 *     a stone rock would have very high pressure-resistance function, but zero
 *     pressure-out function. Same applies to a domain at DMEM_MIN and
 *     rate <= RATE_LOW).
 *
 *     When domain X wants to be expanded, but free memory is unavailable for
 *     the expansion of X, it can expand at the cost of shrinking another
 *     domain Y if pressure-out(X) > pressure-res(Y).
 *
 *     Pressure functions are calculated according to domain size vs. its
 *     DMEM_MIN and DMEM_QUOTA, and its data map-in rate vs. its RATE_LOW
 *     and RATE_HIGH values as follows:
 *
 *      				      (force_resist)  (force_expand)
 *             RATE          DMEM_SIZE         pressure-res    pressure-out
 *
 *         >= RATE_HIGH       > QUOTA             50 + x          50 + x
 *         >= RATE_HIGH    MIN ... QUOTA         100 + x         100 + x
 *         >= RATE_HIGH       <= MIN      	   500    	   300
 *
 *         LOW ... HIGH       > QUOTA     	  30 + x          30 + x
 *         LOW ... HIGH    MIN ... QUOTA  	  60 + x          60 + x
 *         LOW ... HIGH       <= MIN      	   500    	   200
 *
 *         <= RATE_LOW        > QUOTA     	     0    	    0
 *         <= RATE_LOW     MIN ... QUOTA  	    40    	    0
 *         <= RATE_LOW        <= MIN      	   500    	    0
 *
 *         Value of "x" ranges from 0.0 to 1.0 and is calculated depending on
 *         domain RATE. The exact formula for calculating "x" is unimportant,
 *         since its only purpose is to represent relative ordering of domains
 *         within the same tier (i.e. having the same pre-"+" base function
 *         value base, or that is to say in the same RATE and DMEM_SIZE category
 *         in the table). Currently "x" is calculated as x = RATE / RMAX
 *         where RMAX is a maximum rate for all managed domains being balanced.
 *
 *         Currently "x" is calculated on the basis of latest reported
 *         RATE values for each domain, however in the future the algorithm
 *         may take into account longer-term history of RATE, for instance
 *         domains that had low RATE for long period of time may be more
 *         eligible for shrinking than domains that had low RATE for only
 *         a short period of time, and before that exhibited a spike of
 *         higher RATE activity.
 *
 *         Future version of the algorithm may also introduce the following
 *         fine tuning: among domains with roughly the same RATE, domains
 *         with substantially larger DMEM_SIZE may be more eligible for
 *         shrinking and less eligible for expansion than domains with
 *         smaller DMEM_SIZE and similar RATE.
 *
 *         Important: if a domain has been contracted within the current
 *         algorithm tick to a full value of its DMEM_DECR (or even beyond it
 *         for the sake of meeting HOST_RESERVED_HARD target),
 *         its pressure-resistance function goes to 500, making the domain
 *         ineligible for further shrinking within the current tick.
 *
 *         Important: Domain expansion force is defined only for domains that
 *         have recently reported rate data. Domain contraction-resistance
 *         force however is defined also for domains that did not report data
 *         recently. Although Such domains do not participate in rebalancing
 *         (stage 4), nor SOFT-targeting (stage 3), nor most rounds of
 *         HARD-targeting stage 2, rounds 1-3), but they do participate in
 *         HARD-targeting (stage2) rounds 4 and 5. For this special use case,
 *         such domains do have contraction-resistance force defined as follows:
 *
 *      				      (force_resist)
 *                           DMEM_SIZE         pressure-res
 *
 *                            > QUOTA               32
 *                         MIN ... QUOTA            62
 *                            <= MIN      	   500
 *
 *
 *     Host free memory area also has effective pressure function ascribed to it,
 *     defined as:
 *
 *      		        (force_resist)   (force_expand)
 *            HOST_FREE 	 pressure-res     pressure-out
 *
 *         > RESERVED_SOFT            0 	        0
 *         SOFT ... HARD	     45 	     35 - 45
 *         <= RESERVED_HARD        1000                450
 *
 *         During stage 3, when HOST_FREE is in SOFT...HARD range,
 *         expansion-pressure function is effectively applied in two steps.
 *         First it is effectively applied with value of 35, i.e. able to squeeze
 *         domains with RATE = LOW...HIGH and DMEM_SIZE > DMEM_QUOTA.
 *         If it were impossible to satisfy HOST_RESERVED_SOFT target
 *         this way, then expansion-pressure function is applied again,
 *         now with value of 45 and able to contract domains in the category
 *         RATE <= RATE_LOW and with size DMEM_MIN...DMEM_QUOTA.
 *
 *     Stage 4 of the algorithm runs as follows.
 *
 *     First find out if there are any domains wishing to expand.
 *     A domain wants to expand if its pressure-out function is > 0.
 *     In no event expand domain above DMEM_MAX.
 *     If there are no domains to expand, quit.
 *
 *     Sort domains wishing to expand by their pressure-out function and try to
 *     accommodate their expansion requests in the sort order, starting from
 *     the domain with the highest value of pressure-out function and moving
 *     towards domains with lower pressure-out value.
 *
 *     Domain expansion target is normally indicated by DMEM_INCR. However if
 *     domain's DMEM_SIZE < DMEM_MIN, then the target is expansion to DMEM_MIN.
 *
 *     Free memory provides the first source for request accommodation.
 *
 *     Free memory supply for all domains is constrained by HOST_RESERVED_HARD
 *     (which is on top of Xen free memory slack, so memory reserved for Xen
 *     free memory slack is left untouched as well).
 *
 *     Free memory supply for domains with RATE < RATE_HIGH and executing above
 *     their DMEM_QUOTA is also constrained by HOST_RESERVED_SOFT.
 *
 *     A domain expands into free memory to the extent possible, as indicated
 *     by pressure-resistance function of host free memory area (note that
 *     if allocation request causes remaining free memory size to cross
 *     HOST_RESERVED_SOFT or HOST_RESERVED_HARD threshold, then the
 *     allocation is performed accounting for such a crossing, i.e. domain
 *     pressure-out may be high enough to push to the threshold, but
 *     not necessarily beyond the threshold.)
 *
 *     If domain expansion request cannot be satisfied at the expense of free
 *     memory, try to satisfy it at the cost of shrinking other domains.
 *
 *     Sort domains (using another vector, distinct from expansion-order vector)
 *     in the order of their pressure-resistance function, starting from
 *     the lowest value towards the highest. Going from the first
 *     (lowest-resistance) element in this vector towards last (highest resistance),
 *     perform the following:
 *
 *         - if shrink-candidates-vec-domain == expand-candidates-vec-domain,
 *           skip this shrink-candidates-vec-domain
 *
 *         - if pressure-resistance(shrink-candidates-vec-domain) >=
 *           pressure-out(expand-candidates-vec-domain), terminate the rebalancing
 *           process, as even the weakest shrink candidate will not yield its memory
 *           to the strongest expansion candidate
 *
 *         - expand expand-candidate-vec-domain at the cost of shrinking
 *           shrink-candidate-vec-domain
 *
 *           total expansion of expand-vec-domain in the tick cycle cannot
 *           exceed DMEM_INCR, except for domains sized below DMEM_MIN and
 *           expanding to DMEM_MIN
 *
 *           total contraction of shrink-vec-domain in the whole algorithm tick
 *           cannot exceed DMEM_DECR
 *
 *           since pressure-resistance function of a domain can increase while
 *           taking memory out of it if the domain reaches the thresholds of DMEM_QUOTA
 *           or DMEM_MIN, pressure-resistance function may need to be recalculated
 *           at this point and shrink-vector re-sorted (and shrink candidate scanning
 *           re-started from the lowest index again); and similarly for expansion
 *           candidates vector if the expansion crosses the thresholds of DMEM_QUOTA
 *           or DMEM_MIN of the domain being expanded
 *
 *           once domain has been shrunk by DMEM_DECR or more during the current
 *           algorithm tick (this also includes shrinking at stages 2 and 3),
 *           its pressure-resistance function value increases to 500 to reflect
 *           this domain is no longer eligible to be shrunk during the current
 *           tick, and shrink-candidates-vector must be re-sorted, and scanning
 *           re-started from the lowest index again
 *
 *         - to reduce calculations, domains that are not eligible to be expanded
 *           or contracted are removed from the corresponding vectors
 *
 *     To minimize the probability of a jitter of reallocating memory back and
 *     forth between very similar domains (domain upsize/downsize jitter),
 *     a domain that has been expanded not more than @shrink_protection_time
 *     ticks back is not considered elgigible for shrinking at stages 3 and 4
 *     (stages "meeting soft free memory constraint" and "domain rebalancing").
 *     However this protection is not in effect for stage 2 (stage "meeting
 *     hard free memory constraint").
 *
 * In all stages of algorithm:
 *
 *     DMEM_INCR and DMEM_DECR are rounded to the closest memory allocation
 *     quant (typically, Xen page)
 *
 *     domain is never expanded above DMEM_MAX and never contracted below
 *     DMEM_MIN
 *
 *     if domain has been contracted or expanded by amount that causes its
 *     DMEM_SIZE to reach or cross DMEM_MIN or DMEM_QUOTA, its pressure
 *     functions are re-calculated at this point
 *
 *     if domain has been shrunk by DMEM_DECR or more in the current cycle,
 *     its pressure-resistance function goes up to 500
 *
 * If domain has not recently provided data, it is largely left alone and does
 * not participate in rebalancing (stage 4), SOFT-targeted trimming (stage 3)
 * and most of HARD-targeted trimming (rounds 1, 2 and 3 of stage 2). It does
 * however participate in HARD-targeted rounds 4 and 5. Also if domain has
 * setting @trim_unresponsive defined (or inherited from global configuration),
 * then after staying non-reporting for @trim_unresponsive seconds, membalance
 * will trim it down to @dmem_quota.
 *
 * Membalancectl utility provides command "free-memory" that is used to reclaim
 * requested amount of memory for purposes such as starting new virtual machines.
 * This command invokes procedure sched_freemem(...) inside membalance daemon
 * which in turn executes the routine implementing stage 2 of the algorithm
 * (hard trimming), except it trims the domain to reclaim the amount of memory
 * determined not by HOST_RESERVED_HARD, but requested in "free-memory" command.
 * If domain was shrunk in the recent tick by membalancectl "free-memory" command
 * (or multiple executions of "free-memory" command), the aggregate amount it was
 * shrunk by is used during the next tick time to adjust the amount calculated by
 * DMEM_DECR, in an attempt to ensure a domain does not get shrunk by more than
 * DMEM_DECR total within a single tick, except in dire memory shortage.
 *
 * After all stages of the algorithm calculate scheduled expansions and contractions
 * for managed domains, these contractions and expansions are executed. First
 * all the contractions are executed. Then membalance tries to execute the
 * scheduled expansions. The ability of membalance to execute the expansions is
 * contingent on available free memory, which in turn is contingent on domains
 * ordered to shrink executing their de-ballooning promptly enough. When executing
 * an adjustment cycle, membalance will wait for domains ordered to shrink to
 * release their memory for up to a timeout interval derived from configuration
 * parameters (see @domain_expansion_timeout_xxx). If they fail to release required
 * memory within this time and Xen stays short of free memory, membalance will only
 * expand domains scheduled to expand to the extent available memory allows it,
 * and will leave the adjustment of remaining domains till the next tick.
 *
 ******************************************************************************
 *
 * Consistency check conditions:
 *
 *     RATE_HIGH > RATE_LOW
 *     DMEM_HIGH >= DMEM_QUOTA
 *     DMEM_QUOTA >= DMEM_MIN
 *     HOST_RESERVED_SOFT >= HOST_RESERVED_HARD
 *     GUEST_FREE_THRESHOLD in range 0%...100%
 *
 ******************************************************************************
 *
 * Possible future additions (to consider in the future):
 *
 *     1. As free memory is getting in short supply (e.g. crossing into
 *        HOST_RESERVED_SOFT zone), dynamically scale up DMEM_DECR, making the
 *        algorithm a more agressive stripper of the domains with lower claim to
 *        memory. Likewise, if free memory is ample, dynamically scale up DMEM_INCR
 *        making for a faster expansion of the domains in need of memory.
 *
 *     2. When calculating pressure-resistance function, account for a time
 *        a domain have been in the same category of RATE (<= RATE_LOW,
 *        RATE_LOW ... RATE_HIGH, >= RATE_HIGH). For example, domains that had
 *        low rate for a long time must be squeezable more easily than domains
 *        that had a burst of activity and had been inactive only for a short
 *        time.
 *
 *        Also account for long-term rate history during stage 3 (shrinking
 *        domains to meet HOST_RESERVED_SOFT target) in a more sophisticated
 *        way, such as tracking down average rate over long-time peiod and
 *        taking into account absolute value of rate.
 *
 *     3. When calculating pressure-resistance function, account for domain
 *        size. Among domains with roughly the same RATE, domains with
 *        substantially larger DMEM_SIZE may be more eligible for shrinking
 *        and less eligible for expansion than domains with smaller DMEM_SIZE
 *        and similar RATE.
 *
 *     4. Shrink long waiters: In severe memory shortage, shrink domains with
 *        low % of execution time, i.e. mostly sleeping (cpu exec time < 1% of
 *        total time, over recent long interval). Reduce size by 25%?
 *
 *     5. Proactively shrink domains that for a long time had reported high
 *        percentage of free system memory inside the domain (as reported by
 *        guest OS).
 *
 *     6. Make moving average used to calculate SLOW_RATE tunable on global
 *        and per-domain basis to cover a longer history than currently used
 *        five recent ticks data.
 *
 *     7. Record domain size at which last time saw rate >= RATE_LOW or RATE_HIGH.
 *        When in need of a large block of memory e.g. for membalancectl
 *        "free-memory" operation, may try to trim towards this size either by
 *        the whole amount or a fraction of it.
 *
 *     8. Follow stage 4 (domain rebalancing) by another round of stage 3 (soft
 *        memory constraint). If stage 4 caused an expansion of some domains at
 *        the cost of @host_reserved_soft, follow-up repeat of stage 3 would allow
 *        an immediate compenatory contraction of weak-pressure domains in order to
 *        reclaim memory from them to restore free memory supply back up to
 *        @host_reserved_soft if possible. Currently such a reclamation is delayed
 *        until the next tick.
 *
 ******************************************************************************
 *
 * Xen domain memory is adjusted asynchronously, with total memory currently
 * assigned for a domain (as displayed by "xl list" etc.) being:
 *
 *     libxl_dominfo.current_memkb + libxl_dominfo.outstanding_memkb
 *
 *     xc_domaininfo_t.tot_pages + xc_domaininfo_t.outstanding_pages
 *
 * and advancing towards domain's target memory size. The target value is
 * recorded in xenstore. Current allocation is:
 *
 *     libxl_dominfo.current_memkb (xc_domaininfo_t.tot_pages) =
 *         reflects physically allocated pages
 *
 *     libxl_dominfo.outstanding_memkb (xc_domaininfo_t.outstanding_pages) =
 *         reflects claimed, but physically unallocated pages; the claim
 *         has been staked in global page counters, the arithmetics has been
 *         performed, but no specific page frames have been assigned yet
 *
 * The latter part (libxl_dominfo.outstanding_memkb) is pending memory claim
 * for a domain (when expanding) - an amount of memory claimed, but pages
 * not yet physically allocated to a domain. It is currently used by Xen only
 * while the domain is being set up and then reset to 0.
 *
 * The following formula holds (assuming outstanding_pages is zero):
 *
 *              xcinfo.tot_pages * pagesize_kbs
 *                               =
 *     xen_data_size + roundup(xs_mem_target + xs_mem_videoram, pagesize_kbs)
 *
 * When membalance makes a domain working set adjustment decision, it is made
 * on the basis of paging rate within current domain working set size, i.e.
 * current_memkb.
 *
 * Therefore if membalance adjustment algorithm makes at stage 4 a decision
 * to expand a domain by X, actually expand it only by (X - outstanding_memkb).
 * If however the algorithm makes a decision to shrink a domain by X, do shrink
 * it by X (since this is what has been requested by expanding domains or by
 * the demands of free memory region).
 *
 * When libxl_set_memory_target(...) is invoked, it does not result in an instant
 * synchronous corresponding change in the actual domain size (as reflected by
 * xc_domaininfo_t counters), rather domain size starts changing and will be
 * changing gradually over time until it arrives to the target. Depending on the
 * amount of change and system load, the adjustment can typically take some (small)
 * fraction of a second.
 *
 * In practical terms, when domain is being expanded, xc_domaininfo_t.tot_pages will
 * be gradually going up (with xc_domaininfo_t.outstanding_pages staying zero, as of
 * current Xen). The amount of Xen free memory (as reported by
 * xc_physinfo / get_xen_free_memory) will be gradually going down.
 *
 * Vice versa, when a domain is being shrunk, tot_pages will be gradually going down,
 * while the amount of Xen free memory will be gradually going up.
 *
 * In practical terms, when domain is being expanded, the amount of "claimed" memory
 * is really indicated by target - current_memkb, so that is what we'll use for X
 * in the rule outlined above.
 *
 ******************************************************************************
 *
 * Fine details of domain size calculus:
 *
 * Domain is composed of three parts, as described in file
 * $(XENSRC)/docs/misc/libxl_memory.txt:
 *
 *     G = guest target memory (actual guest sees a part of it, less a small bit)
 *     V = video memory
 *     X = Xen structures memory
 *
 * Command "xl mem-set" sets (G + V).
 * @dmem_min, @dmem_max, @dmem_quota are (G + V)
 * xs_mem_target = G
 * xs_mem_videoram = V
 * Function libxl_set_memory_target(enforce=1) takes argument (G + V).
 * xc_domaininfo_t.(tot_pages + outstanding_pages) = G + V + X
 *
 * To find out X, we take a diff between
 *     xc_domaininfo_t.(tot_pages + outstanding_pages)
 * and
 *     (xs_mem_target + xs_mem_videoram)
 * when processing domain as pending and store it in dom->xen_data_size.
 *
 * dom->memsize, dom->memsize0, dom->memsize_incr and dom->memsize_decr
 * are all (G + V)
 *
 * Note that in PV domains V is -1, which is also compensated by G being
 * larger by 1 than the amount requested by "xl mem-set" command or by
 * "memory" key in domain configuration file so (G + V) still yields
 * the amount requested by the user.
 *
 * To summarize:
 *
 *     G + V:
 *
 *         dmem_min
 *         dmem_quota
 *         dmem_max
 *
 *         memsize
 *         memsize0
 *         memsize_incr
 *         memsize_decr
 *
 *         xl mem-set <id> <size>
 *
 *         libxl_set_memory_target(enforce=1)
 *
 *     G + V + X:
 *
 *         xc_domaininfo_t.(tot_pages + outstanding_pages)
 *
 *     G:
 *
 *         xs_mem_target     (when xs_mem_videoram != -1)
 *
 *     V:
 *
 *         xs_mem_videoram   (when xs_mem_videoram != -1)
 *
 * To make things complicated however,
 *
 * (1) Xen unfortunately does not expose the value of X. There is no Xen
 *     hypervisor call that would allow to query it. Thus in effect Xen
 *     mantains two memory scales, one representing the "visible" size of
 *     a domain indicated by domain size as recorded in Xenstore (or as
 *     represented by xc_domain_get/set_pod_target plus videoram size),
 *     the other is represented by xc_domaininfo_t.tot_pages, and
 *     unfortunately there is no reliable way to translate synchronously
 *     between the two scales, even though domain size can be affected
 *     only by acting on the first scale and relationship of the effect
 *     to free memory is via the second scale.
 *
 *     To make things worse, the "visible" scale represents only a *desired*
 *     domain size and tells nothing about its *actual* size. A domain can have
 *     large xs_mem_target but being unable to expand, therefore the value
 *     of xs_mem_target tells nothing about domain's actual current allocation.
 *     Therefore a reasoning about actual current allocation can be performed
 *     only using xc_domaininfo_t.tot_pages scale, whereas adjustments to the
 *     allocation can be performed only on the "visible" scale -- yet Xen
 *     provides no way to translate between the scales.
 *
 *     We can capture the offset between the two scales, but first there is no
 *     way to do it on a "spot" basis, it is possible at all (and then only in
 *     an unreliable way) after domain has been runnable *and* size-stable for
 *     some time.
 *
 * (2) Furthermore, Xen private-data area size can vary in size.
 *     An observable behavior with Linux domains at least is that it jumps
 *     up by ~ 400K after the very first domain size change.
 *     It can also change due to any other activity that causes memory
 *     allocation to a domain (or deallocation), e.g. Xen ring buffers
 *     allocation/deallocation by split drivers.
 *
 * We handle this handicap in two ways:
 *
 * (1) We periodically re-evaluate X at tick interval when domain size
 *     has been stable (both target and current size) for a while and domain
 *     itself has been runnable for this time, and there was free memory
 *     available to let domain proceed with any pending size expansion.
 *
 * (2) Memory shortages during rebalance with magnitude below 1 MB per
 *     shrinked domain result in log messages with debug rather than warning
 *     severity.
 *
 ******************************************************************************
 *
 * General note:
 *
 * "5-minute rule", or rather more modern editions of it, suggests that it pays
 * off to keep data cached in memory if it is going to be used again in the near
 * furure.
 *
 *     http://en.wikipedia.org/wiki/Five-minute_rule
 *
 *     Jim Gray, Prashant Shenoy, "Rules of Thumb in Data Engineering"
 *     http://research.microsoft.com/pubs/68636/ms_tr_99_100_rules_of_thumb_in_data_engineering.pdf
 *     http://research.microsoft.com/en-us/um/people/gray/talks/RulesOfThumbInDataEngineering_UCSC.ppt
 *
 *     Goetz Graefe, "The Five-Minute Rule Twenty Years Later"
 *     http://dl.acm.org/citation.cfm?id=1363198
 *
 *     Chris Melor, "Flash and the five-minute rule"
 *     http://www.theregister.co.uk/2010/05/19/flash_5_minute_rule/
 *
 * "5-minute rule" compares relative costs of memory vs. that of performing
 * disk/SSD IO and informs how to build a balanced system. In a nutshell, in a
 * balanced system it is economic to retain data in main memory (rather than
 * re-read it from storage media) if it is going to be used within next
 * roughly ~ 2 hours. More exact break-even time period depends on data access
 * pattern (sequential vs. random, and data block size) and on storage medium used.
 *
 * Some break-even points (as of 2007-2010) are approximately:
 *
 *     RAM-SATA   4 KB blocks      1.5 hours
 *     RAM-SATA   1 KB blocks      6 hours
 *     RAM-SSD    4 KB blocks      15 minutes
 *     RAM-SSD    1 KB blocks      45 minutes
 *
 * For larger data segments (16-256 KB) break-even time is notably shorter.
 *
 * Overall, on a system that does not exhibit a compelling shortage of DRAM,
 * it usually pays off to retain data cached in DRAM if it is going to be
 * reused within next ~ 2-6 hours rather than re-read it from SATA drive,
 * or within next 15-45 minutes for SSD-to-DRAM re-reading.
 *
 * The rule thus advises how to build a balanced system (amount of main
 * memory it ought to have) and whether to retain data *if* it is going to
 * be reused. The rule does not provide any direct clue for proactive memory
 * reclamation policy, but still provides a basis for guesses for choosing
 * the latter. As a rule of thumb, on a plentiful memory system, data that
 * may be reused is better retained in DRAM for few hours rather than re-read
 * data from SATA drives, or for about 1 hour when SSD drives are used.
 *
 * However on a system with DRAM memory shortage, where DRAM memory effectively
 * comes at a premium, system administrator may be compelled to choose lower
 * duration for proactive memory reclamation.
 */

void sched_memory(void)
{
	sched_tick++;

	if (doms.managed.empty())
	{
		/*
		 * Quick bail out. Normally not even expected to be invoked
		 * if there are no managed domains.
		 */
		debug_msg(5, "sched_memory: tick %ld, no managed domains",
			  sched_tick);
		return;
	}
	else
	{
		debug_msg(5, "sched_memory: tick %ld, managed domains: %lu%s",
			  sched_tick, (unsigned long) doms.managed.size(),
			  memsched_pause_level ? " (adjustments suspended)" : "");
	}

	stage_collect_data();	    /* stage 1*/

	if (memsched_pause_level != 0)
		return;

	sched_reserved_hard();      /* stage 2 */
	sched_reserved_soft();      /* stage 3 */

	if (debug_level >= 30)
		print_reclaimed();

	sched_rebalance();    	    /* stage 4 */

	do_resize_domains(); 	    /* apply pending size changes */
}


/*
 * Called if membalanced was sleeping for some time
 * because managed domains list was empty
 */
void sched_slept(int64_t ms)
{
	sched_tick += ms / (config.interval * MSEC_PER_SEC);
}

/*
 * Debug printout of reclaimed memory
 */
static void print_reclaimed(void)
{
	domain_info* dom;

	foreach_managed_domain(dom)
	{
		if (dom->memsize0 != dom->memsize)
		{
			notice_msg("  [%ld] -> freemem %ld",
				   dom->domain_id, dom->memsize0 - dom->memsize);
		}
	}
}


/******************************************************************************
*                      domain data collection stage                           *
******************************************************************************/

static void stage_collect_data(void)
{
	const xc_domaininfo_t* xcinfo;
	domain_info* dom;
	bool has_report;
	long freeing = 0;

	/*
	 * CAVEAT:
	 *
	 * Xen does not provide an adequate facility for tracking current memory
	 * allocation and outstanding memory commitments that would be usable by
	 * outside applications. (See the detailed comment in the routine
	 * sched_freemem below).
	 *
	 * Thus any reading of "Xen free memory" taken by us is, strictly speaking,
	 * meaningless since it represents merely a spot reading and there may be
	 * domains, including domains outside of our control, that are expanding
	 * or shrinking, and it is not possible for us to know the extent of their
	 * outstanding lien on free memory.
	 *
	 * There is nothing whatsoever we can do about it in a proper way, and there
	 * is very little we can do to mitigate the consequences.
	 *
	 * We simply have no better option but to assume that spot reading of free
	 * memory does represent "true free memory" (less outstanding commitments),
	 * whereas in fact it may not.
	 */

	/* process xenstore watch events */
	refresh_xs();

	xen_free_slack = get_xen_free_slack();
	xen_free0 = get_xen_free_memory();

	/*
	 * Collect data from the domains: for each domain managed by membalance
	 * read its xenstore "report" key and reset the key, then parse the report
	 */
	id2xcinfo.collect();
	read_domain_reports();

	foreach_managed_domain(dom)
	{
		xcinfo = id2xcinfo.get(dom->domain_id);
		if (!xcinfo)
		{
			/* domain is dead */
			unmanage_domain(dom->domain_id);
			continue;
		}

		dom->begin_sched_tick();

		record_memory_info(dom, xcinfo);
		reset_preshrink(dom);
		dom->reeval_xen_data_size(xen_free0);
	}

	foreach_managed_domain(dom)
	{
		/*
		 * parse domain report message,
		 * if report is invalid, log message and consider as no report
		 */
		has_report = (dom->report_raw != NULL);
		if (has_report && !dom->parse_domain_report())
			has_report = false;

		if (has_report)
		{
			/*
			 * record when we saw last report
			 */
			dom->no_report_time = 0;
			dom->last_report_tick = sched_tick;

			/*
			 * If in-domain guest operating system has plenty of free memory
			 * (reported guest free memory > GUEST_FREE_THRESHOLD percent of total
			 * guest system memory), treat reported data map-in rate as 0,
			 * regardless of the reported rate value.
			 */
			if (dom->freepct > config.guest_free_threshold * 100)
				dom->rate = 0;

			/*
			 * If domain data map-in rate is <= RATE_ZERO, treat reported data map-in
			 * rate as 0, regardless of the reported rate value.
			 */
			if (dom->rate <= dom->rate_zero)
				dom->rate = 0;

			/*
			 * Calculate domain slow_rate and fast_rate
			 * (fast and slow moving averages of rate)
			 */
			dom->calc_rates(sched_tick);

			/*
			 * account for rate history
			 */
			if (dom->slow_rate <= dom->rate_low)
				dom->time_rate_below_low += config.interval;
			else
				dom->time_rate_below_low = 0;

			if (dom->fast_rate < dom->rate_high)
				dom->time_rate_below_high += config.interval;
			else
				dom->time_rate_below_high = 0;
		}
		else if (runnable(dom))
		{
			/*
			 * if domain did not report in for a while,
			 * trim it down to quota
			 */
			dom->no_report_time += config.interval;

			if (dom->trim_unresponsive > 0 &&
			    dom->no_report_time > dom->trim_unresponsive &&
			    memsched_pause_level == 0)
			{
				if (trim_to_quota(dom))
				{
					dom->trimming_to_quota = true;

					freeing += max(0, dom->memsize - dom->dmem_quota);

					dom->memgoal0 =
					dom->memsize0 = dom->memsize =
					dom->dmem_quota;

					continue;
				}
			}
		}

		/*
		 * Disregard domains in the following states:
		 * paused, dying, shutdown, crashed (a variant of shutdown).
		 * Only manage domains in the states: running, blocked.
		 */
		if (!runnable(dom))
			continue;

		/*
		 * Disregard currently non-participating domains (have not
		 * reported data within more than one cycle/tick). If a domain
		 * has not reported data for the current cycle yet, but have
		 * reported for the previous cycle, presume reading to be the
		 * same as for the previous cycle and reuse report data from
		 * the previous cycle.
		 */
		if (sched_tick > dom->last_report_tick + 1)
			continue;

		dom->valid_data = true;

		debug_msg(10, "memsched: collected domain %s\n"
			      "    memsize      = %ld kbs = %ld mb + %ld kb\n"
			      "    memsize_incr = %ld kbs = %ld mb + %ld kb\n"
			      "    memsize_decr = %ld kbs = %ld mb + %ld kb\n"
			      "    rate=%ld, slow_rate=%ld, fast_rate=%ld, freepct=%g%%",
			  dom->printable_name(),
			  dom->memsize, dom->memsize / 1024, dom->memsize % 1024,
			  dom->memsize_incr, dom->memsize_incr / 1024, dom->memsize_incr % 1024,
			  dom->memsize_decr, dom->memsize_decr / 1024, dom->memsize_decr % 1024,
			  dom->rate, dom->slow_rate, dom->fast_rate, dom->freepct);
	}

	/*
	 * Display domains with no valid data for them for the current tick
	 */
	if (debug_level >= 10)
	{
		foreach_managed_domain(dom)
		{
			if (!dom->valid_data)
			{
				notice_msg("memsched: no current data for domain %s",
					   dom->printable_name());
			}
		}
	}

	/*
	 * Read current free memory minus slack
	 */
	host_lien0 = eval_memory_lien();
	xen_free0 = get_xen_free_memory();

	host_free = xen_free0 - xen_free_slack - host_lien0;

	/*
	 * Do not add in @freeing (the memory recovered from the domains being
	 * trimmed down to quote), as it:
	 *
	 *     - may take time to free up
	 *     - may partially be already reflected in the reading of
	 *       get_xen_free_memory() taken after trim_to_quota()
	 *
	 * If Xen provided tracking of lien data, we could use it in lien calculations.
	 */
	// host_free += freeing;

	if (debug_level >= 10)
	{
		bool neg = host_free < 0;
		if (neg)  host_free = -host_free;

		const char* sign = neg ? "(-) " : "";
		notice_msg("memsched: free memory less slack = %s%ld kbs "
			   "= %s%ld mb + %ld kb "
			   "= %s%ld gb + %ld mb + %ld kb",
			   sign, host_free,
			   sign, host_free / 1024, host_free % 1024,
			   sign, host_free / (1024 * 1024), (host_free / 1024) % 1024,
			   host_free % 1024);

		if (neg)  host_free = -host_free;
	}
}

/*
 * domain_info method called at the beginning of scheduling tick
 * (for managed domains only)
 */
void domain_info::begin_sched_tick(void)
{
	/* @report_raw is already refreshed by read_domain_reports(...) */
	trimming_to_quota = false;
	valid_data = false;
	valid_memory_data = false;
	balside = RebalanceSide_NEUTRAL;
	expand_force0 = 0;
}

/*
 * Get current memory size.
 *
 * Total domain size is:
 *     xcinfo->tot_pages (posessed pages)
 * plus
 *     xcinfo->outstanding_pages (claimed but not posessed pages)
 *
 * However do not include xcinfo->outstanding_pages in the
 * calculations of desired domain size adjustment, since
 * measured data map-in rate was sampled based on guest system
 * having only xcinfo->tot_pages in its posession (or rather this
 * is a better estimate), therefore any expansion or contraction
 * should be calculated off this base line.
 *
 * Calculate margins around it corresponding to dmem_decr and dmem_incr.
 */

/* try to increase domain size by up to @dmem_incr */
inline static long eval_incr(const domain_info* dom)
{
	long m;

	/*
	 * base calculations on the actual "core size" of the domain
	 * (without claimed pages) since data map rate is a function
	 * of physically allocated memory, not claimed memory
	 */
	m = dom->memsize0;
	m = (long) (m * (1 + dom->dmem_incr));
	m = roundup(m, memquant_kbs);
	m = max(dom->dmem_min, m);
	m = min(dom->dmem_max, m);

	return m;
}

/* graze up to @dmem_decr off current domain size */
inline static long eval_decr(const domain_info* dom)
{
	long m, m0, decr;

	/*
	 * base calculations on the actual "core size" of the domain
	 * (without claimed pages) since data map rate is a function
	 * of physically allocated memory, not claimed memory
	 */
	m = m0 = dom->memsize0;
	m = (long) (m * (1 - dom->dmem_decr));
	m = roundup(m, memquant_kbs);
	m = max(dom->dmem_min, m);
	m = min(dom->dmem_max, m);

	/*
	 * reduce decrement by the amount of previous trim over the tick
	 */
	if (dom->preshrink && sched_tick - dom->preshrink_tick <= 1)
	{
		decr = m0 - m;
		decr -= dom->preshrink;
		decr = max(decr, 0);

		m = m0 - decr;

		m = roundup(m, memquant_kbs);
		m = max(dom->dmem_min, m);
		m = min(dom->dmem_max, m);
	}

	return m;
}

static void record_memory_info(domain_info* dom, const xc_domaininfo_t* xcinfo)
{
	dom->xcinfo = xcinfo;

	dom->memsize = pagesize_kbs * xcinfo->tot_pages - dom->xen_data_size;
	dom->memsize0 = dom->memsize;

	if (dom->memsize < 0)
		fatal_msg("bug: memsize < 0");

	dom->memgoal0 = roundup(dom->xs_mem_target + dom->xs_mem_videoram, pagesize_kbs);

	dom->memsize_incr = eval_incr(dom);
	dom->memsize_decr = eval_decr(dom);

	dom->valid_memory_data = true;
}

/*
 * Reset a record of domain trimming over the preceding tick.
 *
 * This record is reset on every sched_memory(...) tick after
 * being factored into the calculated value of @memsize_decr.
 */
static void reset_preshrink(domain_info* dom)
{
	dom->preshrink = 0;
	dom->preshrink_tick = 0;
}

/*
 * Parse raw report string -> dom structure.
 *
 * Return:  @true if raw data was present and was successfuly parsed,
 *          @false otherwise
 *
 * May unmanage the domain on hard error.
 */
bool domain_info::parse_domain_report(void)
{
	char* saveptr;
	char* token;
	char* key;
	char* val;
	char* dp;
	map_ss kv;
	const char* cp;

	if (!report_raw)
		return false;

	/* check report structure version */
	if (report_raw[0] != 'A' || report_raw[1] != '\n')
		goto unmanage;

	/* break down report into key-value map */
	for (token = strtok_r(report_raw + 2, "\n", &saveptr);
	     token != NULL;
	     token = strtok_r(NULL, "\n", &saveptr))
	{
		dp = strchr(token, ':');
		if (!dp)
			continue;

		key = token;
		*dp = '\0';
		val = dp + 1;
		while (isblank(*val))
			val++;
		set_kv(kv, key, val);
		*dp = ':';
	}

	/* must include "action: report" */
	cp = k2v(kv, "action");
	if (!(cp && streq(cp, "report")))
		goto unmanage;

	cp = k2v(kv, "kbsec");
	if (!(cp && a2long(cp, &rate)))
		goto unmanage;

	cp = k2v(kv, "freepct");
	if (!(cp && a2double(cp, &freepct)))
		goto unmanage;

	free_ptr(report_raw);

	return true;

	/* unmanage domain */
unmanage:
	debug_msg(1, "domain %s reported malformatted data [%s]",
		  printable_name(),
		  report_raw);
	error_msg("unmanaging domain %s because it submitted malformatted report",
		  printable_name());
	free_ptr(report_raw);
	unmanage_domain(domain_id);

	return false;
}

/*
 * Calculate slow_rate and fast_rate
 * (fast and slow moving averages of rate).
 *
 * Fast_rate (fast moving average of rate) is later used for calculations
 * of domain expansion force/pressure.
 *
 * Slow_rate (slow moving average of rate) is later used for calculations
 * of domain force to resist its contraction.
 */
void domain_info::calc_rates(unsigned long tick)
{
	/*
	 * for slow rate, weight recent samples
	 * and max with last reading
	 */
	static const double slow_weights[] = { 10, 3, 2, 2, 1 };
	static const int nslow_weights = countof(slow_weights);

	/* insert current element at the start, remove excess */
	rate_history.insert(rate_history.begin(), rate_record(tick, rate));
	while (rate_history.size() > nslow_weights)
		rate_history.pop_back();

	/*
	 * for fast rate, use just current sample
	 */
	fast_rate = rate;

	/* slow weight: weight the samples and max with current */
	slow_rate = (long) weight_samples(slow_weights, nslow_weights);
	slow_rate = max(rate, slow_rate);
}

/*
 * Calculate moving average over samples history.
 *
 * Input:
 *     samples data in @rate_history (going backwards)
 *     sample weights in @weights
 *     @nweights size of @weights array
 *
 * Returns:
 *      Weighted samples.
 *
 * Averages up to @nweights (or less if the history is shorter) according to
 * specified weights. If there is a long breach in history (over 1 missing
 * sample), the history is considerd stopped at this point.
 */
double domain_info::weight_samples(const double* weights, int nweights)
{
	double sum_rate = 0;
	double sum_weight = 0;
	unsigned long prev_tick;
	int nel = min(nweights, (int) rate_history.size());

	for (int k = 0;  k < nel;  k++)
	{
		const rate_record& rr = rate_history[k];

		/* use only continuous samples (up to 1 missing sample ok) */
		if (k != 0)
		{
			/* for continuous samples gap will be 0 */
			int gap = prev_tick - rr.tick - 1;
			if (gap < 0)
				fatal_msg("bug: weight_samples bad gap");
			if (gap > 1)
				break;
			k += gap;
		}

		prev_tick = rr.tick;

		if (k >= nel)
			break;

		sum_weight += weights[k];
		sum_rate += rr.rate * weights[k];
	}

	return sum_rate / sum_weight;
}


/******************************************************************************
*                    stage: free memory HARD constraint                       *
******************************************************************************/

static void sched_reserved_hard(void)
{
	if (host_free < config.host_reserved_hard)
	{
		long need = config.host_reserved_hard - host_free;
		long reclaimed = hard_reclaim(need);
		host_free += reclaimed;

		if (reclaimed >= need)
			debug_msg(10, "memsched: reserved_hard: reclaimed %ld KB",
				       reclaimed);
		else
			debug_msg(10, "memsched: reserved_hard: reclaimed %ld KB, still need %ld KB",
				      reclaimed, need - reclaimed);
	}
}

/*
 * Do best to reclaim up to @goal kbs from managed domains.
 *
 * Reclamation is performed by just scheduling domain trimming,
 * hard_reclaim(...) does not perform actual trimming.
 *
 * Return total amount that hard_reclaim(...) was able to recover,
 * which may be less than @goal.
 *
 * Called by:
 *
 *     - sched_memory(...) at those ticks when there is memory shortage
 *       below @host_reserved_hard
 *
 *     - sched_freemem(...) invoked in turn by "membalancectl free-memory"
 *       command when domains need to be shrinked to get the requested
 *       amount of memory
 *
 */
static long hard_reclaim(long goal)
{
	/* memory allocation is in quants */
	goal = roundup(goal, memquant_kbs);

	long req = goal;

	hard_reclaim_round_1(goal);
	hard_reclaim_round_2(goal);
	hard_reclaim_round_3(goal);
	hard_reclaim_round_4(goal);
	hard_reclaim_round_5(goal);

	return req - goal;
}

/*
 * Calculate new size for @dom after decreasing it down
 * by one slice of @dmem_decr
 */
inline static long eval_more_decr(domain_info* dom)
{
	long m = (long) (dom->memsize * (1 - dom->dmem_decr));

	m = roundup(m, memquant_kbs);
	m = max(dom->dmem_min, m);
	m = min(dom->dmem_max, m);

	return m;
}

/*
 * Select domains with RATE <= RATE_LOW and sort them by the time
 * they had rate <= RATE_LOW. Starting from domain that had low rate for
 * longest time and towards domain that had it for shortest time, trim
 * each domain by up to DMEM_DECR (but never below DMEM_MIN) -- until
 * the goal is satisfied or all matching domains have been trimmed down.
 *
 * If domain has been trimmed within the recent tick by sched_freemem(...),
 * charge this trim against any possible current trimming in this round
 * so the total trim does not exceed DMEM_DECR.
 */
static void hard_reclaim_round_1(long& goal)
{
	/* quick bail out if nothing to do */
	if (goal <= 0)
		return;

	domain_info* dom;
	domvector vec;

	/*
	 * Enumerate candidate domains. Leave alone domains in the following
	 * states: paused, dying, shutdown, crashed (a variant of shutdown),
	 * currently being trimmed, or no valid rate data.
	 */
	foreach_managed_domain(dom)
	{
		if (dom->valid_data &&
		    dom->time_rate_below_low != 0 &&
		    runnable(dom) &&
		    !dom->trimming_to_quota)
		{
			vec.push_back(dom);
		}
	}

	/* sort vec by descending time domain rate has been <= RATE_LOW */
	vec.sort_desc_by_time_rate_below_low();

	for (unsigned k = 0;  k < vec.size();  k++)
	{
		dom = vec[k];
		long trim = dom->memsize - dom->memsize_decr;
		if (trim > 0)
		{
			trim = min(trim, goal);
			dom->memsize -= trim;
			goal -= trim;
			if (goal <= 0)   break;
		}
	}
}

/*
 * Select domains with RATE < RATE_HIGH and size > DMEM_QUOTA, excepting domains
 * already trimmed in the previous round, and sort them by the time they had
 * rate < RATE_HIGH. Starting from domain that had rate < RATE_HIGH for longest
 * time and towards domain that had it for shortest time, trim each domain by
 * up to DMEM_DECR (but never below DMEM_QUOTA) -- until the goal is satisfied
 * or all matching domains have been trimmed down.
 */
static void hard_reclaim_round_2(long& goal)
{
	/* quick bail out if nothing to do */
	if (goal <= 0)
		return;

	domain_info* dom;
	domvector vec;

	/*
	 * Enumerate candidate domains. Leave alone domains in the following
	 * states: paused, dying, shutdown, crashed (a variant of shutdown),
	 * or currently being trimmed, or have been trimmed in the previous
	 * round, or no valid rate data.
	 */
	foreach_managed_domain(dom)
	{
		if (dom->valid_data &&
		    dom->time_rate_below_high != 0 &&
		    dom->memsize > dom->dmem_quota &&
		    dom->memsize == dom->memsize0 &&
		    runnable(dom) &&
		    !dom->trimming_to_quota)
		{
			vec.push_back(dom);
		}
	}

	/* sort vec by descending time domain rate has been < RATE_HIGH */
	vec.sort_desc_by_time_rate_below_high();

	for (unsigned k = 0;  k < vec.size();  k++)
	{
		dom = vec[k];
		long trim = dom->memsize - max(dom->memsize_decr, dom->dmem_quota);
		if (trim > 0)
		{
			trim = min(trim, goal);
			dom->memsize -= trim;
			goal -= trim;
			if (goal <= 0)   break;
		}
	}
}

/*
 * Select domains with RATE < RATE_HIGH and size > DMEM_QUOTA, regardless of
 * whether they were already trimmed in previous rounds, and sort them by the
 * time they had rate < RATE_HIGH. Starting from domain that had rate < RATE_HIGH
 * for longest time and towards domain that had it for shortest time, and regarless
 * of any previous trimming amount, trim each domain [additionally] by up to
 * DMEM_DECR (but never below DMEM_QUOTA) on top of any previous trimming --
 * until the goal is satisfied or all matching domains have been trimmed down.
 */
static void hard_reclaim_round_3(long& goal)
{
	/* quick bail out if nothing to do */
	if (goal <= 0)
		return;

	domain_info* dom;
	domvector vec;

	/*
	 * Enumerate candidate domains. Leave alone domains in the following
	 * states: paused, dying, shutdown, crashed (a variant of shutdown),
	 * or currently being trimmed, or no valid rate data.
	 */
	foreach_managed_domain(dom)
	{
		if (dom->valid_data &&
		    dom->time_rate_below_high != 0 &&
		    dom->memsize > dom->dmem_quota &&
		    runnable(dom) &&
		    !dom->trimming_to_quota)
		{
			vec.push_back(dom);
		}
	}

	/* sort vec by descending time domain rate has been < RATE_HIGH */
	vec.sort_desc_by_time_rate_below_high();

	/*
	 * graze down by up to @dmem_decr from current allocation,
	 * but not below quota
	 */
	for (unsigned k = 0;  k < vec.size();  k++)
	{
		dom = vec[k];
		long m = eval_more_decr(dom);
		long trim = dom->memsize - max(m, dom->dmem_quota);
		if (trim > 0)
		{
			trim = min(trim, goal);
			dom->memsize -= trim;
			goal -= trim;
			if (goal <= 0)   break;
		}
	}
}

/*
 * Calculate pressure-resistance function for every domain above DMEM_QUOTA
 * (the advisory value of RATE we use in this calculation is made not perfectly
 * valid by previous contractions, but this is the best we can do at this point).
 * Sort the list by the value of the function. Starting from lowest-pressure
 * domain towards highest-pressure domains, trim each domain by up to DMEM_DECR
 * (but not brining it below DMEM_QUOTA) until the deficit is satisfied.
 *
 * If was unable to satisfy the deficit within one pass, repeat passes until
 * either the deficit is satisfied or all domains are brought down to
 * DMEM_QUOTA.
 *
 * For domains that have claimed but unallocated pages, can trim all of those
 * pages and take the domain below DMEM_QUOTA all way down up to DMEM_MIN.
 *
 * Note that even domains with no valid rate data partake in this round,
 * with pressure-resistance force heuristically ascribed for them.
 */
static void hard_reclaim_round_4(long& goal)
{
	/* quick bail out if nothing to do */
	if (goal <= 0)
		return;

	domain_info* dom;
	domvector vec;
	long m, trim;

	/*
	 * Enumerate candidate domains. Leave alone domains in the following
	 * states: paused, dying, shutdown, crashed (a variant of shutdown),
	 * or currently being trimmed.
	 */
	foreach_managed_domain(dom)
	{
		if (dom->memsize > dom->dmem_quota &&
		    runnable(dom) &&
		    !dom->trimming_to_quota)
		{
			/*
			 * treat domains with no recent valid data rate,
			 * as having rate = 0
			 */
			if (!dom->valid_data)
			{
				dom->rate = 0;
				dom->slow_rate = 0;
				dom->fast_rate = 0;
			}
			vec.push_back(dom);
		}
	}

	/*
	 * Keep trimming domains one slice at a time until we reach the goal
	 * or all domains are squeezed down to @dmem_quota
	 */
	while (goal > 0 && vec.size())
	{
		/* calculate pressure-resistance function for all elements in vec */
		eval_resist_force(vec);

		/* sort vec by ascending pressure-resistance function */
		vec.sort_asc_by_resist_force();

		/*
		 * graze down by up to @dmem_decr from current allocation,
		 * but not below quota; also trim claimed but physically
		 * unallocated pages
		 */
		for (unsigned k = 0;  k < vec.size();  k++)
		{
			dom = vec[k];

			/*
			 * If Xen provided a way to track down memory commitments,
			 * we could try here to trim down domain pages that were
			 * claimed, but have not been physically allocated yet.
			 * Unfortunately Xen does not provide such a facility (see
			 * comment marked CAVEAT in routine sched_freemem below).
			 */

			/* try to trim down physically allocated part */
			m = eval_more_decr(dom);
			trim = dom->memsize - max(m, dom->dmem_quota);
			if (trim > 0)
			{
				trim = min(trim, goal);
				dom->memsize -= trim;
				goal -= trim;
				if (goal <= 0)   break;
			}

			if (dom->memsize <= dom->dmem_quota)
			{
				/* remove element from vec */
				remove_at(vec, k--);
			}
		}
	}
}

/*
 * If free memory deficit is still not satisfied at this point, try to satisfy
 * it by gradually trimming domains from DMEM_QUOTA towards DMEM_MIN.
 *
 * Recalculate pressure-resistance function for each domain using the last
 * sampled RATE value for this domain (this is suboptimal, but the best we
 * can do at this point). Starting from the lowest-pressure domain towards
 * highest-pressure domain, trim each domain by up to DMEM_DECR (but not
 * brining it below DMEM_MIN) until the deficit is satisfied.
 *
 * If was unable to satisfy the deficit within one pass, repeat passes until
 * either the deficit is satisfied or all domains are brought down to DMEM_MIN.
 *
 * Note that even domains with no valid rate data partake in this round,
 * with pressure-resistance force heuristically ascribed for them.
 */
static void hard_reclaim_round_5(long& goal)
{
	/* quick bail out if nothing to do */
	if (goal <= 0)
		return;

	domain_info* dom;
	domvector vec;
	long m, trim;

	/*
	 * Enumerate candidate domains. Leave alone domains in the following
	 * states: paused, dying, shutdown, crashed (a variant of shutdown),
	 * or currently being trimmed.
	 */
	foreach_managed_domain(dom)
	{
		if (dom->memsize > dom->dmem_min &&
		    runnable(dom))
		{
			/*
			 * treat domains with no recent valid data rate,
			 * as having rate = 0, except for young domains
			 */
			if (!dom->valid_data)
			{
				/* Dom0 is never young */
				if (dom->domain_id != 0 &&
				    dom->startup_time >= 0 &&
				    xen_domain_uptime(dom->domain_id) <= dom->startup_time)
				{
					dom->rate =
					dom->slow_rate =
					dom->fast_rate = dom->rate_high + 1;
				}
				else
				{
					dom->rate = 0;
					dom->slow_rate = 0;
					dom->fast_rate = 0;
				}
			}
			vec.push_back(dom);
		}
	}

	/*
	 * Keep trimming domains one slice at a time until we reach the goal
	 * or all domains are squeezed down to @dmem_min
	 */
	while (goal > 0 && vec.size())
	{
		/* calculate pressure-resistance function for all elements in vec */
		eval_resist_force(vec);

		/* sort vec by ascending pressure-resistance function */
		vec.sort_asc_by_resist_force();

		/*
		 * graze down by up to @dmem_decr from current allocation,
		 * but not below @dmem_min
		 */
		for (unsigned k = 0;  k < vec.size();  k++)
		{
			dom = vec[k];
			m = eval_more_decr(dom);
			trim = dom->memsize - max(m, dom->dmem_min);
			if (trim > 0)
			{
				trim = min(trim, goal);
				dom->memsize -= trim;
				goal -= trim;
				if (goal <= 0)   break;
			}

			if (dom->memsize <= dom->dmem_min)
			{
				/* remove element from vec */
				remove_at(vec, k--);
			}
		}
	}
}


/******************************************************************************
*                    stage: free memory SOFT constraint                       *
******************************************************************************/

static void sched_reserved_soft(void)
{
	if (host_free < config.host_reserved_soft)
	{
		long need = config.host_reserved_soft - host_free;
		long reclaimed = soft_reclaim(need);
		host_free += reclaimed;

		if (reclaimed >= need)
			debug_msg(10, "memsched: reserved_soft: reclaimed %ld KB",
				       reclaimed);
		else
			debug_msg(10, "memsched: reserved_soft: reclaimed %ld KB, still need %ld KB",
				      reclaimed, need - reclaimed);
	}

}

/*
 * Do best to reclaim up to @goal kbs from managed domains.
 *
 * Reclamation is performed by just scheduling domain trimming,
 * soft_reclaim(...) does not perform actual trimming.
 *
 * Return total amount that soft_reclaim(...) was able to recover,
 * which may be less than @goal.
 */
static long soft_reclaim(long goal)
{
	/* memory allocation is in quants */
	goal = roundup(goal, memquant_kbs);

	long req = goal;

	soft_reclaim_round_1(goal);
	soft_reclaim_round_2(goal);
	soft_reclaim_round_3(goal);

	return req - goal;
}

/*
 * Select domains with RATE <= RATE_LOW and size > DMEM_QUOTA. Sort the list
 * by the time domain RATE was <= RATE_LOW. Starting from the domains that
 * had low rate for longest time, and towards domains that had it for shortest
 * time, trim each by up to DMEM_DECR (but not below DMEM_QUOTA) until the
 * deficit is satisfied.
 *
 * If domain has already been previously trimmed in current algorithm tick,
 * it can only be trimmed at this stage to the extent that the total trim
 * does not exceed DMEM_DECR.
 *
 * If domain has been trimmed within the recent tick by membalancectl
 * "free-memory" command, charge this trim against any possible current
 * trimming in this round.
 */
static void soft_reclaim_round_1(long& goal)
{
	/* quick bail out if nothing to do */
	if (goal <= 0)
		return;

	domain_info* dom;
	domvector vec;
	long trim;

	/*
	 * Enumerate candidate domains. Leave alone domains in the following
	 * states: paused, dying, shutdown, crashed (a variant of shutdown),
	 * or currently being trimmed, or no valid rate data, or protected
	 * from shrinking due to been just recently expanded.
	 */
	foreach_managed_domain(dom)
	{
		if (dom->valid_data &&
		    dom->time_rate_below_low != 0 &&
		    dom->memsize > dom->dmem_quota &&
		    dom->memsize > dom->dmem_decr &&
		    runnable(dom) &&
		    !dom->trimming_to_quota &&
		    !is_shrink_soft_protected(dom))
		{
			vec.push_back(dom);
		}
	}

	/* sort vec by descending time domain rate has been <= RATE_LOW */
	vec.sort_desc_by_time_rate_below_low();

	for (unsigned k = 0;  k < vec.size();  k++)
	{
		dom = vec[k];
		trim = dom->memsize - max(dom->memsize_decr, dom->dmem_quota);
		if (trim > 0)
		{
			trim = min(trim, goal);
			dom->memsize -= trim;
			goal -= trim;
			if (goal <= 0)   break;
		}
	}
}

/*
 * Select domains with RATE <= RATE_LOW and any size(including <= DMEM_QUOTA).
 * Sort the list by the time domain RATE was <= RATE_LOW. Starting from the
 * domains that had low rate for longest time, and towards domains that had
 * it for shortest time, trim each by up to DMEM_DECR (but not below DMEM_MIN)
 * until the deficit is satisfied.
 *
 * If domain has already been previously trimmed in current algorithm tick,
 * it can only be trimmed at this stage to the extent that the total trim
 * does not exceed DMEM_DECR.
 *
 * If domain has been trimmed within the recent tick by membalancectl
 * "free-memory" command, charge this trim against any possible current
 * trimming in this round.
 */
static void soft_reclaim_round_2(long& goal)
{
	/* quick bail out if nothing to do */
	if (goal <= 0)
		return;

	domain_info* dom;
	domvector vec;
	long trim;

	/*
	 * Enumerate candidate domains. Leave alone domains in the following
	 * states: paused, dying, shutdown, crashed (a variant of shutdown),
	 * or currently being trimmed, or no valid rate data, or protected
	 * from shrinking due to been just recently expanded.
	 */
	foreach_managed_domain(dom)
	{
		if (dom->valid_data &&
		    dom->time_rate_below_low != 0 &&
		    dom->memsize > dom->dmem_decr &&
		    runnable(dom) &&
		    !dom->trimming_to_quota &&
		    !is_shrink_soft_protected(dom))
		{
			vec.push_back(dom);
		}
	}

	/* sort vec by descending time domain rate has been <= RATE_LOW */
	vec.sort_desc_by_time_rate_below_low();

	for (unsigned k = 0;  k < vec.size();  k++)
	{
		dom = vec[k];
		trim = dom->memsize - max(dom->memsize_decr, dom->dmem_min);
		if (trim > 0)
		{
			trim = min(trim, goal);
			dom->memsize -= trim;
			goal -= trim;
			if (goal <= 0)   break;
		}
	}
}

/*
 * Select domains with RATE < RATE_HIGH and size > DMEM_QUOTA. Sort the list
 * by the time domain RATE was < RATE_HIGH. Starting from the domains that
 * had rate < RATE_HIGH for longest time, and towards domains that had it
 * for shortest time, trim each by up to DMEM_DECR (but not below DMEM_QUOTA)
 * until the deficit is satisfied.
 *
 * If domain has already been previously trimmed in current algorithm tick,
 * it can only be trimmed at this stage to the extent that the total trim
 * does not exceed DMEM_DECR.
 *
 * If domain has been trimmed within the recent tick by membalancectl
 * "free-memory" command, charge this trim against any possible current
 * trimming in this round.
 */
static void soft_reclaim_round_3(long& goal)
{
	/* quick bail out if nothing to do */
	if (goal <= 0)
		return;

	domain_info* dom;
	domvector vec;
	long trim;

	/*
	 * Enumerate candidate domains. Leave alone domains in the following
	 * states: paused, dying, shutdown, crashed (a variant of shutdown),
	 * or currently being trimmed, or no valid rate data, or protected
	 * from shrinking due to been just recently expanded.
	 */
	foreach_managed_domain(dom)
	{
		if (dom->valid_data &&
		    dom->time_rate_below_high != 0 &&
		    dom->memsize > dom->dmem_quota &&
		    dom->memsize > dom->dmem_decr &&
		    runnable(dom) &&
		    !dom->trimming_to_quota &&
		    !is_shrink_soft_protected(dom))
		{
			vec.push_back(dom);
		}
	}

	/* sort vec by descending time domain rate has been <= RATE_LOW */
	vec.sort_desc_by_time_rate_below_high();

	for (unsigned k = 0;  k < vec.size();  k++)
	{
		dom = vec[k];
		trim = dom->memsize - max(dom->memsize_decr, dom->dmem_quota);
		if (trim > 0)
		{
			trim = min(trim, goal);
			dom->memsize -= trim;
			goal -= trim;
			if (goal <= 0)   break;
		}
	}
}


/******************************************************************************
*                  stage: satisfy domain expansion demands 	              *
******************************************************************************/

static void sched_rebalance(void)
{
	domvector vec_expand;	/* expansion candidates */
	domvector vec_shrink;   /* shrinking candidates */

	eval_force_context resist_force_context;
	eval_force_context expand_force_context;

	domain_info* dom;
	unsigned k, ndoms;
	long need, m;

	/*
	 * Enumerate candidate domains for rebalancing.
	 *
	 * Leave alone domains in the following states: paused, dying,
	 * shutdown, crashed (a variant of shutdown), or currently being
	 * trimmed, or no valid rate data.
	 */
	foreach_managed_domain(dom)
	{
		if (dom->valid_data &&
		    runnable(dom) &&
		    !dom->trimming_to_quota)
		{
			vec_expand.push_back(dom);
		}
	}

	/* no domains to rebalance? */
	ndoms = vec_expand.size();
	if (ndoms == 0)
		return;

	vec_shrink = vec_expand;

	/*
	 * Calculate expansion and resistance-to-contraction forces
	 */
	expand_force_context.eval_expand_force_context(vec_expand, 0, ndoms - 1);
	resist_force_context.eval_resist_force_context(vec_shrink, 0, ndoms - 1);
	for (k = 0;  k < ndoms;  k++)
	{
		eval_expand_force(expand_force_context, vec_expand[k]);
		eval_resist_force(resist_force_context, vec_shrink[k]);
	}

	/*
	 * To reduce further calculations, weed out domains that do not want
	 * to be expanded (have zero pressure) or cannot be expanded (already
	 * at their maximum size).
	 *
	 * Also record expansion force at the start of the rebalancing to
	 * esablish a precedence order at the resizing execution phase.
	 */
	for (k = 0;  k < vec_expand.size();  k++)
	{
		dom = vec_expand[k];
		dom->expand_force0 = dom->expand_force;
		if (dom->expand_force <= eps || dom->memsize >= dom->memsize_incr)
			remove_at(vec_expand, k--);
	}

	/*
	 * Similarly eliminate domains that cannot be shrunk any further
	 * in the current tick, either because they reached their maximim
	 * soft-shrinking size per tick, or are protected from shrinking
	 * due to been just recently expanded.
	 */
	for (k = 0;  k < vec_shrink.size();  k++)
	{
		dom = vec_shrink[k];
		if (dom->memsize <= dom->memsize_decr ||
		    is_shrink_soft_protected(dom))
		{
			remove_at(vec_shrink, k--);
		}
	}

	/*
	 * Sort vectors
	 */
	vec_expand.sort_desc_by_expand_force();
	vec_shrink.sort_asc_by_resist_force();

	/*
	 * Process domains wishing to expand
	 */
	while (vec_expand.size() != 0)
	{
		/* Get element with the highest expansion pressure */
		dom = vec_expand[0];

		/*
		 * Is this domain in the process of being shrinked in a rebalancing
		 * process? If so, we reached right-side @vec_expand area of low-pressure
		 * domains. This domain is not an eligible candidate for an expansion,
		 * as most likely all domains past it.
		 */
		if (dom->balside == RebalanceSide_SHRINKING)
		{
			remove_at(vec_expand, 0);
			continue;
		}
		dom->balside = RebalanceSide_EXPANDING;

		/*
		 * We want to expand the domain all way up to @memsize_incr,
		 * however in the process it may cross the thresholds of @dmem_min
		 * and @dmem_quota and change its size category and hence expansion
		 * pressure. Once this happens, it may become not the most
		 * demanding domain anymore at that point, and expansion priority
		 * may go to another domain in @vec_expand.
		 *
		 * Therefore try to grow the domain in chunks sized up to the
		 * next category-changing threshold, so after reaching the
		 * threshold we can re-evaluate domain expansion force and
		 * possibly reposition the domain in @vec_expand.
		 *
		 * Notice that eval_incr(...) previously ensured that memsize_incr
		 * is in range [dmem_min ... dmem_max].
		 */
		if (dom->memsize < dom->dmem_min)
			m = min(dom->dmem_min, dom->memsize_incr);
		else if (dom->memsize < dom->dmem_quota)
			m = min(dom->dmem_quota, dom->memsize_incr);
		else
			m = dom->memsize_incr;

		/* In this round, try to expand @dom by @need */
		need = m - dom->memsize;

		/* record current size category */
		category_t c_size = size_expand_category(dom, dom->memsize);

		/*
		 * Try to satisfy domain's demand at the cost of free space
		 */
		if (!expand_into_freemem(dom, need))
		{
			/*
			 * Try to satisfy domain's demand at the cost of
			 * shrinking other domains
			 */
			m = dom->memsize;
			rebalance_domains(dom, need, resist_force_context, vec_shrink);

			/* if could not grow it at all, we are done with rebalancing */
			if (dom->memsize == m)
				break;
		}

		/* is domain expansion need for the current tick fully satisfied? */
		if (dom->memsize >= dom->memsize_incr)
		{
			remove_at(vec_expand, 0);
			continue;
		}

		/*
		 * if domain changed size category:
		 *   - recalculate domain expansion force
		 *   - move domain to correct position in @vec_expand
		 */
		if (size_expand_category(dom, dom->memsize) != c_size)
		{
			eval_expand_force(expand_force_context, dom);
			remove_at(vec_expand, 0);
			insert_into_vec_expand(vec_expand, dom);
		}
		else if (dom->memsize == m)
		{
			fatal_msg("bug: sched_rebalance: size category did not change");
		}
	}
}

/*
 * Try to expand @dom by up to @need at the cost of free memory.
 *
 * If returns @true, memory has been added to the domain drawing on
 * free memory area.
 *
 * If returns @false, no free memory was allocated and caller must
 * try to allocate memory by trimming other domains (those in @vec_shrink).
 */
static bool expand_into_freemem(domain_info* dom, long need)
{
	long chunk = free_allocate(dom->expand_force, need);

	if (chunk == 0)
		return false;

	dom->memsize += chunk;

	debug_msg(30, "  freemem -> [%ld] %ld at force %g, leaves free %ld",
		      dom->domain_id, chunk, dom->expand_force, host_free);

	return true;
}

/*
 * Allocate up to @need (KBs) of free memory at specified @expand_force
 * of a domain trying to expand. Return the allocated amount that can
 * be anywhere between 0 and @need. If returned value is less than @need,
 * then this is the maximum that can be allocated at a given @expand_force.
 *
 * Allocation is always multiple of @memquant_kbs.
 */
static long free_allocate(double expand_force, long need)
{
	long avail, reserve, allocated;

	if (need < 0)
		fatal_msg("bug: free_allocate: need < 0");

	reserve = (expand_force > resist_force_free_soft) ? config.host_reserved_hard
				                          : config.host_reserved_soft;

	avail = host_free - reserve;
	if (avail <= 0)
		return 0;

	allocated = min(avail, need);
	allocated = rounddown(allocated, memquant_kbs);
	host_free -= allocated;

	return allocated;
}

/*
 * Try to expand @dom by up to @need at the cost of shrinking
 * other domains listed in @vec_shrink listed there in the order
 * of ascending shrink-resistance function.
 */
static void rebalance_domains(
	domain_info* dom,
	long need,
	eval_force_context& resist_force_context,
	domvector& vec_shrink)
{
	domain_info* victim;
	long m, chunk;

	while (need > 0 && vec_shrink.size() != 0)
	{
		/* potential victim doman */
		victim = vec_shrink[0];

		/*
		 * Weed out domains ineligible to be victims:
		 *     - currently expanding domain (@dom)
		 *     - domain that was already expanded during the current tick
		 *     - domain is at its minimum decrement and cannot be shrunk any further
		 */
		if (victim->balside == RebalanceSide_EXPANDING ||
		    victim->memsize <= victim->memsize_decr)
		{
			remove_at(vec_shrink, 0);
			continue;
		}

		/*
		 * If requestor's expansion force is not stronger than
		 * the weakest victim's contraction-resistance force, quit
		 */
		if (dom->expand_force <= victim->resist_force)
			return;

		/*
		 * How much we can shave off this victim till its next
		 * size threshold?
		 */
		if (victim->memsize > victim->dmem_quota)
			m = max(victim->memsize_decr, victim->dmem_quota);
		else
			m = victim->memsize_decr;

		chunk = victim->memsize - m;

		/* record current size category */
		category_t c_size = size_resist_category(victim, victim->memsize);

		/*
		 * Transfer memory from @victim to @dom
		 */
		chunk = min(chunk, need);
		victim->memsize -= chunk;
		dom->memsize += chunk;
		need -= chunk;
		victim->balside = RebalanceSide_SHRINKING;
		debug_msg(30, "  [%ld] -> [%ld]  %ld",
			      victim->domain_id, dom->domain_id, chunk);

		/*
		 * Is victim totally spent as a supplier of memory?
		 */
		if (victim->memsize <= victim->memsize_decr)
		{
			remove_at(vec_shrink, 0);
		}
		else if (c_size != size_resist_category(victim, victim->memsize))
		{
			/*
			 * victim domain changed size category:
			 *   - recalculate domain resistance force
			 *   - move domain to correct position in @vec_shrink
			 */
			eval_resist_force(resist_force_context, victim);
			remove_at(vec_shrink, 0);
			insert_into_vec_shrink(vec_shrink, victim);
		}
	}
}

/*
 * Insert domain into @vec pre-sorted by descending expansion force.
 *
 * Try to insert it before other domains with the same expansion
 * force, to minimize the number of domain expansions (better to
 * expand one domain than two). Thus consider ">=" as effective ">".
 */
static void insert_into_vec_expand(domvector& vec, domain_info* dom)
{
	int start = 0;
	int end = vec.size() - 1;
	int mid;

	while (start <= end)
	{
		mid = (start + end) / 2;

		if (dom->expand_force >= vec[mid]->expand_force)
		{
			end = mid - 1;
		}
		else
		{
			start = mid + 1;
		}
	}

	insert_at(vec, start, dom);
}

/*
 * Insert domain into @vec pre-sorted by ascending resistance force.
 *
 * Try to insert it before other domains with the same resistance
 * force, to minimize the number of domain contractions (better to
 * shrink one domain than two). Thus consider "<=" as effective "<".
 */
static void insert_into_vec_shrink(domvector& vec, domain_info* dom)
{
	int start = 0;
	int end = vec.size() - 1;
	int mid;

	while (start <= end)
	{
		mid = (start + end) / 2;

		if (dom->resist_force <= vec[mid]->resist_force)
		{
			end = mid - 1;
		}
		else
		{
			start = mid + 1;
		}
	}

	insert_at(vec, start, dom);
}


/******************************************************************************
*                         memory pressure force calculus                      *
******************************************************************************/

/* helper */
inline static double eval_force(long rate, double base, long rmax)
{
	if (rmax != 0)
		base += (double) rate / rmax;
	return base;
}


/******************************************************************************************
*                                resist force calculations                                *
******************************************************************************************/

void eval_force_context::eval_resist_force_context(const domvector& vec, int vk1, int vk2)
{
	pre_eval();

	/* find out actual rmax for each category */
	for (int k = vk1;  k <= vk2;  k++)
	{
		domain_info* dom = vec[k];
		if (dom->valid_data)
			rmax = max(rmax, dom->slow_rate);
	}

	/* base[c_rate][c_size] */
	base[C_MID][C_MID] = 60;
	base[C_MID][C_HIGH] = 30;
	base[C_HIGH][C_MID] = 100;
	base[C_HIGH][C_HIGH] = 50;

	post_eval();
}

static void eval_resist_force(eval_force_context& ctx, domain_info* dom)
{
	/* handle domains with no valid data in a special way */
	if (!dom->valid_data)
	{
		switch (size_resist_category(dom, dom->memsize))
		{
		case C_LOW:    	dom->resist_force = 500;  break;
		case C_MID:    	dom->resist_force = 62;	  break;
		case C_HIGH:   	dom->resist_force = 32;   break;
		}
		return;
	}

	category_t c_rate = rate_category(dom, dom->slow_rate);
	category_t c_size = size_resist_category(dom, dom->memsize);

	switch (c_size)
	{
	case C_LOW:
		dom->resist_force = 500;
		break;

	case C_MID:
		switch (c_rate)
		{
		case C_LOW:
			dom->resist_force = 40;
			break;
		case C_MID:
		case C_HIGH:
			dom->resist_force = eval_force(dom->slow_rate,
						       ctx.base[c_rate][c_size],
						       ctx.rmax);
			break;
		}
		break;

	case C_HIGH:
		switch (c_rate)
		{
		case C_LOW:
			dom->resist_force = 0;
			break;
		case C_MID:
		case C_HIGH:
			dom->resist_force = eval_force(dom->slow_rate,
						       ctx.base[c_rate][c_size],
						       ctx.rmax);
			break;
		}
		break;
	}
}

/*
 * Evaluate dom->resist_force for domain set
 */
static void eval_resist_force(const domvector& vec)
{
	int size = (int) vec.size();
	if (size)
		eval_resist_force(vec, 0, size - 1);
}

/*
 * Evaluate dom->resist_force for domain set vec[kv1...kv2].
 */
static void eval_resist_force(const domvector& vec, int vk1, int vk2)
{
	eval_force_context ctx;
	ctx.eval_resist_force_context(vec, vk1, vk2);

	for (int k = vk1;  k <= vk2;  k++)
	{
		domain_info* dom = vec[k];
		eval_resist_force(ctx, dom);
	}
}


/******************************************************************************************
*                                expand force calculations                                *
******************************************************************************************/

void eval_force_context::eval_expand_force_context(const domvector& vec, int vk1, int vk2)
{
	pre_eval();

	/* find out actual rmax for each category */
	for (int k = vk1;  k <= vk2;  k++)
	{
		domain_info* dom = vec[k];

		if (!dom->valid_data)
			fatal_msg("bug: eval_expand_force: domain with no valid_data");

		rmax = max(rmax, dom->fast_rate);
	}

	/* base[c_rate][c_size] */
	base[C_MID][C_MID] = 60;
	base[C_MID][C_HIGH] = 30;
	base[C_HIGH][C_MID] = 100;
	base[C_HIGH][C_HIGH] = 50;

	post_eval();
}

static void eval_expand_force(eval_force_context& ctx, domain_info* dom)
{
	category_t c_rate = rate_category(dom, dom->fast_rate);
	category_t c_size = size_expand_category(dom, dom->memsize);

	switch (c_rate)
	{
	case C_LOW:
		dom->expand_force = 0;
		break;

	case C_MID:
		switch (c_size)
		{
		case C_LOW:
			dom->expand_force = 200;
			break;
		case C_MID:
		case C_HIGH:
			dom->expand_force = eval_force(dom->fast_rate,
						       ctx.base[c_rate][c_size],
						       ctx.rmax);
			break;
		}
		break;

	case C_HIGH:
		switch (c_size)
		{
		case C_LOW:
			dom->expand_force = 300;
			break;
		case C_MID:
		case C_HIGH:
			dom->expand_force = eval_force(dom->fast_rate,
						       ctx.base[c_rate][c_size],
						       ctx.rmax);
			break;
		}
		break;
	}
}

/*
 * Evaluate dom->expand_force for domain set
 */
static void eval_expand_force(const domvector& vec)
{
	int size = (int) vec.size();
	if (size)
		eval_expand_force(vec, 0, size - 1);
}

/*
 * Evaluate dom->expand_force for domain set vec[kv1...kv2].
 */
static void eval_expand_force(const domvector& vec, int vk1, int vk2)
{
	eval_force_context ctx;
	ctx.eval_expand_force_context(vec, vk1, vk2);

	for (int k = vk1;  k <= vk2;  k++)
	{
		domain_info* dom = vec[k];
		eval_expand_force(ctx, dom);
	}
}


/******************************************************************************
*                        apply pending domain size changes                    *
******************************************************************************/

/*
 * Apply pending domain size changes (contractions and expansions) that have
 * been scheduled by previous stages of the algorithm
 */
static void do_resize_domains(void)
{
	domain_info* dom;
	domvector vec_up, vec_down;
	long delta;
	unsigned k;

	foreach_managed_domain(dom)
	{
		if (!runnable(dom))
			continue;

		/*
		 * Catch wrong decisions
		 */
		if (dom->memsize < dom->dmem_min &&
		    host_free > config.host_reserved_hard)
		{
			error_msg("bug: tried to size domain %s to less than DMEM_MIN",
				  dom->printable_name());
			delta = min(dom->dmem_min - dom->memsize,
				    host_free - config.host_reserved_hard);
			dom->memsize += delta;
			host_free -= delta;
		}

		if (dom->memsize > dom->dmem_max &&
		    !(dom->memsize0 > dom->dmem_max && dom->memsize <= dom->memsize0))
		{
			error_msg("bug: tried to size domain %s to more than DMEM_MAX",
				  dom->printable_name());
			host_free += dom->memsize - dom->dmem_max;
			dom->memsize = dom->dmem_max;
		}

		if (dom->memsize > dom->memsize0)
		{
			/* domains to expand */
			vec_up.push_back(dom);

			/* already overshooting? */
			if (dom->memgoal0 > dom->memsize)
				regoal(dom, dom->memsize);
		}
		else if (dom->memsize < dom->memsize0)
		{
			/* domains to shrink */
			vec_down.push_back(dom);

			/* already undershooting? */
			if (dom->memgoal0 < dom->memsize)
				regoal(dom, dom->memsize);
		}
		else /* memsize == memsize0 */
		{
			/*
			 * We disable regoal in this case (note "if false" below).
			 *
			 * The reason is due to the cause outlined in section
			 * "Fine details of domain size calculus" above.
			 *
			 * Xen used two scales of domain sizing, and those scales
			 * are disconnected. There is no way to make a reliable
			 * reasoning about sizes on one scale based on data on
			 * another scale.
			 *
			 * More specifically, if as a result of guest activity
			 * (such as allocation/deallocation of ring buffer pages
			 * by split drivers) the size of Xen internal part grows
			 * up, this need to be reflected in xen_data_size.
			 * If it is not reflected, then "target" scale will be
			 * off compared to actual allocation. However Xen does
			 * not provide a way to query the size of Xen internal
			 * per-domain allocation, therefore we can infer it only
			 * after a period of stability in memsize0 and memgoal0
			 * (see domain_info::reeval_xen_data_size), and such a
			 * period cannot be ensured if we regoal here.
			 *
			 * Here is what happens if we regoal here: after a period
			 * of domain activity its Xen hidden part size goes up.
			 * But since memsize0/memgoal0 were unstable during this
			 * period, we could not learn that the hidden part size
			 * went up. If we come here with stale value of xen_data_size,
			 * the value of memsize0 will be also off (by the amount of
			 * change in Xen hidden part size). If we try to drive new
			 * memory allocation to memgoal0, it will actually mean
			 * increasing domain size compared to its current value.
			 * This will also make memsize0/memgoal0 unstable for the
			 * current cycle, so xen_data_size will not get updated
			 * again, and stale value will persist. Therefore the
			 * situation will get repeated at the next cycle again.
			 * And then again. And again. And again.
			 *
			 * What we will have then: at each tick, the size of the
			 * domain will go up by the amount xen_data_size is off
			 * compared to the actual value (changed by Xen, but
			 * unknow to us), and since it is going up and up, and
			 * never stabilizes, reeval_xen_data_size(...) will be
			 * perpetually unable to capture an updated value of
			 * xen_data_size.
			 *
			 * This will continue until domain activity breaks us
			 * out of "memsize == memsize0" condition (but with the
			 * value of xen_data_size still off), or until the domain
			 * reaches dmem_max.
			 *
			 * The only way to resolve this is to expose the size of
			 * Xen hidden area in Xen domain allocation query API,
			 * thus providing a connection between the two scales
			 * (POD target size and actual allocation) which are now
			 * disconnected.
			 *
			 * Until then, the best we can do is not to regoal here
			 * and thus hope reeval_xen_data_size(...) will eventually
			 * be able to re-capture an updated value if xen_data_size.
			 */

			/* Deviating? */
			if (dom->memgoal0 != dom->memsize0 && false)
				regoal(dom, dom->memsize0);
		}
	}

	/*
	 * sort vec from largest negative to smallest negative change in
	 * actual memory allocation; start shrinking domains that have mosty
	 * to give fist
	 */
	vec_down.sort_asc_by_memalloc_change();

	/*
	 * Expansions will be executed in the order of domain expansion
	 * pressure force
	 */
	vec_up.sort_desc_by_expand_force0();

	if (debug_level >= 30)
		print_plan(vec_down, vec_up);

	/*
	 * Execute shrinking
	 */
	for (k = 0;  k < vec_down.size();  k++)
	{
		dom = vec_down[k];
		log_resize(dom, "shrink");
		do_shrink_domain(dom, dom->memsize);
	}

	/*
	 * We are about to execute planned domain expansions. Unfortunately as
	 * of now (v4.4) Xen does not provide an adequate facility for tracking
	 * current memory allocations and liens, including outstanding memory
	 * commitments. (See the comment in sched_freemem below).
	 *
	 * If Xen did, we could have started domain expansions proceeding with
	 * them peacemeal as free memory is becoming available being released by
	 * domains being contracted.
	 *
	 * However since Xen does not track memory liens, if we start domain
	 * expansions before fully completing domain contractions, multiple
	 * expansions will proceed in parallel to each other and in parallel with
	 * onging contractions, with us not being able to tell where we stand in
	 * terms of outstanding memory liens.
	 *
	 * Therefore we engage here in a choreography on the shifting sands
	 * intended to substitute missing Xen lien tracking facility with an
	 * attainable imperfect approximation of lien tracking.
	 *
	 * At any given time, extra amount of memory that can be granted to
	 * domains wishing to expand is defined as constrained by two formulas:
	 *
	 *     xen_free0 - host_reserved_xxx - free_slack - lien0 +
	 *     released_by(vec_down) - allocated_by(vec_up)
	 *
	 *     xen_free - host_reserved_xxx - free_slack - lien0
	 *
	 * Where:
	 *
	 *     xen_free0 = an amount of free memory read at the beginning of
	 *                 rebalancing cycle
	 *
	 *     xen_free  = "live" amount of free memory
	 *
	 *     lien0     = outstanding liens calculated at the beginning of
	 *                 rebalancing cycle
	 *
	 *     reserved_xxx = xxx is "soft" or "hard" depending on the expansion
	 *      	   force of domain currently being processed; we approximate
	 *      	   the value of force with expand_force0
	 *
	 *     released_by(vec_down) = amount of memory released by domains in
	 *      	   @vec_down since the start of shrinking
	 *
	 *     allocated_by(vec_up) = amount of memory reallocated to domains in
	 *      	   @vec_up since the start of expansion
	 *
	 */

	/*
	 * Execute expansions
	 */
	struct timespec ts0 = getnow();
	long allocated = 0;
	domid2xcinfo xinfo;
	bool read_xinfo = true;
	int nomem_cycles = 0;    /* sleep cycles when we saw no memory released */
	bool warn = false;

	struct __prev
	{
		domain_info* dom;
		long goal;
		long alloc;
	}
	prev = { NULL, 0, 0 };

	while (vec_up.size() != 0)
	{
		if (read_xinfo)
		{
			/*
			 * Read domain data on the very first pass or after sleep
			 */
			xinfo.collect();
			read_xinfo = false;
		}

		dom = vec_up[0];

		const xc_domaininfo_t* xcinfo = xinfo.get(dom->domain_id);
		if (!xcinfo)
		{
			/* domain is gone */
			remove_at(vec_up, 0);
			continue;
		}

		if (prev.dom != dom)
		{
			/* starting to expand yet another domain */
			prev.dom = dom;
			prev.goal = dom->memgoal0;
			prev.alloc = dom->memsize0;
		}

		long curr_size = pagesize_kbs * xcinfo->tot_pages - dom->xen_data_size;
		long goal = eval_allocate(dom, curr_size, prev.alloc, vec_down, xinfo, allocated);

		if (goal > prev.goal)
		{
			do_expand_domain(dom, goal);
			allocated += goal - prev.alloc;
			prev.alloc = goal;
			prev.goal = goal;
			dom->last_expand_tick = sched_tick;
			nomem_cycles = 0;
		}

		if (goal == dom->memsize)
		{
			/* achieved expansion goal for this domain */
			log_resize(dom, "expand");
			remove_at(vec_up, 0);
			continue;
		}

		/*
		 * Comes here if there is not enough memory to fully satisfy
		 * the expansion of @dom. If timeout allows us to wait for free
		 * memory to become available (coming from domains that keep
		 * shrinking), sleep and retry. Otherwise quit.
		 */
		if (nomem_cycles >= config.domain_expansion_timeout_abort ||
		    vec_down.size() == 0)
		{
			warn = true;
			break;
		}
		int domain_expansion_timeout_ms =
			min(config.domain_expansion_timeout_max * MSEC_PER_SEC,
			    (int) (config.domain_expansion_timeout_frac *
			           config.interval * MSEC_PER_SEC));
		int64_t ms = timespec_diff_ms(ts0, getnow()) + domain_expansion_timeout_ms;
		if (ms <= 0 || testmode)
			break;
		ms = min(100, ms);
		usleep(ms * USEC_PER_MSEC);
		read_xinfo = true;
		nomem_cycles++;
	}

	/*
	 * Domains can be left unexpanded or partially expanded for the following
	 * basic causes:
	 *
	 * 1) An interference with automatic memory adjustment by membalance,
	 *    such as e.g. manual changes of domain sizes by system administrator
	 *    executed concurrently with membalance without pausing membalance
	 *    automatic memory adjustment, or very sluggish response of some domains
	 *    to previous membalance adjustment directives.
	 *
	 * 2) Very large amount of memory being reallocated from a domain to other
	 *    domains (for 5-second config.interval it would typically be in the
	 *    order of 3 GB or more). The domain that releases memory just won't be
	 *    able to release enough memory for such a reallocation within our
	 *    timeout interval. We leave their expansions till the next tick.
	 *    (Meanwhile domains that had been ordered to shrink will keep/continue
	 *    releasing memory, which we will be able to use on the next tick.)
	 *
	 * 3) In case of a huge memory reallocation, domain whose adjustment was
	 *    initiated at tick N can still be in the process of resizing at tick
	 *    (N + 1). We seem to take care of it not to issue a false warning
	 *    message.
	 *
	 * 4) xen_data_size needs to be readjusted for some domains, specifically
	 *    domains being shrunk. See "Fine details of domain size calculus" above
	 *    for details.
	 *
	 *    The magnitude of required adjustment is typically in the order of few
	 *    hunderd KB, therefore we refrain from issuing a warning message if total
	 *    memory shortage is below 1 MB per shrinked domain and issue just a debug
	 *    rather than warning message instead in this case.
	 */
	if (vec_up.size())
	{
		dom = vec_up[0];
		bool partial = (prev.dom == dom && prev.goal != dom->memgoal0);

		if (mem_shortage(vec_up, partial, prev.goal) <= 1024 * (long) vec_down.size())
			warn = false;

		log_unexpanded(vec_up, warn, partial, prev.goal);
	}
}

/*
 * Called on domains with movement already in progress and with current goal
 * overshooting the re-calculated goal.
 */
static void regoal(domain_info* dom, long size)
{
	do_resize_domain(dom, size);
	dom->memgoal0 = size;
	debug_msg(10, "regoal domain %s", dom->printable_name());
}

/*
 * Calculate how much memory can be allocated to expand @dom.
 * Return new goal size for @dom.
 *
 * @curr_size is @dom actual current memory allocation reported by Xen.
 * @vec_down lists domain being concurrently shrinked.
 *
 * @xinfo is "current" memory allocation data for domains in @vec_down.
 * Read it at the start of the expansion process, re-read again after some
 * time passes, such as after a memory wait sleep.
 *
 * @allocated is an aggregate amount of memory that had already been allocated
 * for expansion of the domains during current rebalancing tick.
 */
static long eval_allocate(domain_info* dom, long curr_size, long prev_alloc,
			  const domvector& vec_down,
			  const domid2xcinfo& xinfo,
			  long allocated)
{
	long m1, m2, m;

	/*
	 * Accounting from the tick start point (on top of memsize0)
	 *
	 * We always refer here to host_reserved_hard regardless of the domain
	 * expansion pressure level (rather than to host_reserved_soft
	 * for low pressure levels) since it can be the transfer of memory
	 * from one domain to another, with overall free memory residual
	 * legitimately being under host_reserved_soft, in case domains
	 * with high memory demand are present in the mix.
	 */
	m1 = xen_free0 - config.host_reserved_hard - xen_free_slack - host_lien0 +
	     mem_released_by(vec_down, xinfo) - allocated;
	m1 = max(m1, 0);
	m1 = rounddown(m1, pagesize_kbs);
	m1 += prev_alloc;

	/*
	 * Accounting from the current time point (on top of curr_size)
	 */
	m2 = get_xen_free_memory() - config.host_reserved_hard - xen_free_slack - host_lien0;
	m2 = max(m2, 0);
	m2 = rounddown(m2, pagesize_kbs);
	m2 += curr_size;

	m = min(m1, m2);
	m = max(m, dom->memsize0);   /* expanding, do not shrink (unless memsize0 > memsize) */
	m = min(m, dom->memsize);    /* do not expand beyond goal */

	debug_msg(20, "eval_allocate: id=%ld  curr=%ld  goal=%ld  memsize0=%ld  "
		      "memsize=%ld  m1=%ld  m2=%ld  memgoal0=%ld",
		      dom->domain_id, curr_size, m, dom->memsize0,
		      dom->memsize, m1, m2, dom->memgoal0);

	debug_msg(21, "               released=%ld  allocated=%ld",
		      mem_released_by(vec_down, xinfo), allocated);

	return m;
}

/*
 * Calculate the amount of memory that had been released by the domains
 * in @vec_down since they started shrinking. Use @xinfo for data about domain
 * current sizing.
 *
 * Might return negative value if some of the domains somehow started
 * expanding again.
 */
static long mem_released_by(const domvector& vec_down, const domid2xcinfo& xinfo)
{
	domain_info* dom;
	const xc_domaininfo_t* xcinfo;
	long oldsize, newsize, dsum = 0;

	/*
	 * Process existing domains that have released pages
	 */
	for (unsigned k = 0;  k < vec_down.size();  k++)
	{
		dom = vec_down[k];
		xcinfo = xinfo.get(dom->domain_id);

		if (xcinfo)
		{
			oldsize = dom->memsize0 + dom->xen_data_size;
			newsize = pagesize_kbs * xcinfo->tot_pages;
			dsum += oldsize - newsize;
		}
	}

	/*
	 * Process managed domains that are gone.
	 * Use conservative estimate of released space.
	 */
	foreach_managed_domain(dom)
	{
		xcinfo = xinfo.get(dom->domain_id);
		if (!xcinfo)
		{
			oldsize = min(dom->memsize0, dom->memsize) + dom->xen_data_size;
			dsum += oldsize;
		}
	}

	return dsum;
}

/*
 * Log debug message about domain resizing
 */
static void log_resize(domain_info* dom, const char* action)
{
	long delta = dom->memsize - dom->memsize0;
	long amt = delta > 0 ? delta : -delta;

	debug_msg(10, "%s domain %s (%ld kb -> %ld kb) by %s %ld kbs = %ld mb + %ld kb",
		      action,
		      dom->printable_name(),
		      dom->memsize0, dom->memsize,
		      delta > 0 ? "[+]" : "[-]",
		      amt, amt / 1024, amt % 1024);
}

/*
 * Log messages about domains that were left unexpanded or only partially expanded.
 *
 * @warn when @true causes summary message being emitted as a warning,
 * otherwise as a debug message.
 *
 * @partial says if domain at @vec_up[0] was partially expanded.
 *
 * @prev_goal is the last memory size goal instruction issued to the domain.
 * Only matters if @partial is true.
 */
static void log_unexpanded(domvector& vec_up, bool warn, bool partial, long prev_goal)
{
	domain_info* dom;
	int nshort = 0;
	long shortage = mem_shortage(vec_up, partial, prev_goal);

	/*
	 * Print a message about a partially expanded domain
	 */
	if (partial)
	{
		dom = vec_up[0];
		long amt = dom->memsize - prev_goal;
		debug_msg(10, "domain %s was expanded partially, "
			      "short by %ld kbs = %ld mb + %ld kb",
			      dom->printable_name(),
			      amt, amt / 1024, amt % 1024);
		remove_at(vec_up, 0);
		nshort++;
	}

	/*
	 * Log expansions that will not be attempted
	 */
	while (vec_up.size() != 0)
	{
		dom = vec_up[0];
		log_resize(dom, "will not expand");
		remove_at(vec_up, 0);
		nshort++;
	}

	/*
	 * Log summary message
	 */
	if (nshort != 0 && warn)
	{
		warning_msg("was unable to %sexpand %d domain%s in the current tick, "
			    "memory shortage = %ld kbs",
			    partial ? "fully " : "",
			    nshort, plural(nshort),
			    shortage);
	}
	else if (nshort != 0 && !warn)
	{
		debug_msg(10, "was unable to %sexpand %d domain%s in the current tick "
			      "[ok, still reclaiming memory]",
			      partial ? "fully " : "",
			      nshort, plural(nshort));
	}
}

/*
 * Calculate memory shortage for this tick's expansion.
 * Called at the end of tick if expansion fails to fully complete.
 * Calculate by how much we are short of the goal.      						 .
 *      												 .
 * @partial says if domain at @vec_up[0] was partially expanded.
 *
 * @prev_goal is the last memory size goal instruction issued to the domain.
 * Only matters if @partial is true.
 */
static long mem_shortage(domvector& vec_up, bool partial, long prev_goal)
{
	long shortage = 0;
	domain_info* dom;
	unsigned k = 0;

	if (partial)
	{
		dom = vec_up[k++];
		shortage += max(0, dom->memsize - prev_goal);
	}

	for (;  k < vec_up.size();  k++)
	{
		dom = vec_up[k];
		shortage += max(0, dom->memsize - dom->memsize0);
	}

	return shortage;
}

/*
 * Collect memory allocation data for managed domains
 */
void collect_domain_memory_info(void)
{
	const xc_domaininfo_t* xcinfo;
	domain_info* dom;

	/* process xenstore watch events */
	refresh_xs();

	id2xcinfo.collect();

	foreach_managed_domain(dom)
	{
		xcinfo = id2xcinfo.get(dom->domain_id);
		if (!xcinfo)
		{
			/* domain is dead */
			unmanage_domain(dom->domain_id);
			continue;
		}

		record_memory_info(dom, xcinfo);
	}
}

/* helper routines for out-of-module calls */
bool is_runnable(const xc_domaininfo_t* xcinfo)
{
	return runnable(xcinfo);
}

bool is_runnable(domain_info* dom)
{
	return runnable(dom);
}


/*
 * Debgging printout
 */
static void print_plan(const domvector& vec_down, const domvector& vec_up)
{
	domain_info* dom;
	unsigned k;

	/*
	 * Display global data
	 */
	notice_msg(" ");
	notice_msg("Rebalancing plan:");
	notice_msg(" ");
	notice_msg("     xen_free_memory = %ld", get_xen_free_memory());
	notice_msg("           xen_free0 = %ld", xen_free0);
	notice_msg("      xen_free_slack = %ld", xen_free_slack);
	notice_msg("          host_lien0 = %ld", host_lien0);
	notice_msg("  host_reserved_soft = %ld", config.host_reserved_soft);
	notice_msg("  host_reserved_hard = %ld", config.host_reserved_hard);
	notice_msg(" ");

	/*
	 * Display domains to be shrunk
	 */
	if (vec_down.size() == 0)
		notice_msg("  Shrink domains: (none)");
	else
		notice_msg("  Shrink domains:\n");

	for (k = 0;  k < vec_down.size();  k++)
	{
		dom = vec_down[k];
		notice_msg("    [%ld]  %ld -> %ld  (yields: %ld)",
			   dom->domain_id,
			   dom->memsize0, dom->memsize,
			   dom->memsize0 - dom->memsize);
	}

	notice_msg(" ");

	/*
	 * Display domains to be expanded
	 */
	if (vec_up.size() == 0)
		notice_msg("  Expand domains: (none)");
	else
		notice_msg("  Expand domains:\n");

	for (k = 0;  k < vec_up.size();  k++)
	{
		dom = vec_up[k];
		notice_msg("    [%ld]  %ld -> %ld  (takes: %ld)  force=%g",
			   dom->domain_id,
			   dom->memsize0, dom->memsize,
			   dom->memsize - dom->memsize0,
			   dom->expand_force0);
	}

	notice_msg(" ");
}


/******************************************************************************
*                       free up requested amount of memory                    *
******************************************************************************/

inline static long calc_avail(long mfree, long lien, bool above_slack, bool draw_reserved_hard)
{
	if (above_slack)
		mfree -= xen_free_slack;
	if (!draw_reserved_hard)
		mfree -= config.host_reserved_hard;
	mfree -= lien;
	mfree = max(mfree, 0);
	return rounddown(mfree, memquant_kbs);
}


/*
 * Try to bring host free memory up to the requested amount (@req kbs),
 * by shrinking domains if necessary.
 *
 * Invoked by membalancectl via RPC.
 *
 * @above_slack argument is @true if the @amt mount is on top of Xen free
 * memory slack, otherwise false.
 *
 * If @must is true and target goal is not achievable, membalance won't
 * even try shrinking the domains.
 *
 * If Xen memory is in the state of a flux, e.g. domains are continuing to
 * expand or contract, allow membalance daemon to wait up to @timeout
 * seconds for the flux to stabilize if deemed necessary.
 *
 * Return the amount of free host memory attained (with and without slack).
 * The amounts are in KBs.
 *
 * If request is successful, the returned amount is >= than requested.
 *
 * If request is unsuccessful, and return status is 'N' or 'A',
 * returned size values represent maximum attainable memory.
 *
 * Return status:
 *
 *    P => Error: request is rejected because automatic adjustment
 *         is not paused.
 *         Other fields in the response are not meaninful.
 *
 *    N => Error, adjustment has not attempted since the goal is not
 *         achievable and "must" was reqested.
 *         Other fields show max. achievable free memory.
 *
 *    A => Adjustment has been attempted (if necessary).
 *         Other fields show max. achieved free memory.
 *         It could be less or more than was requested.
 */
int sched_freemem(u_quad_t u_reqamt,
		  bool above_slack,
		  bool draw_reserved_hard,
		  bool must,
		  int timeout,
		  u_quad_t* p_freemem_with_slack,
		  u_quad_t* p_freemem_less_slack)
{
	long req = (long) u_reqamt;
	long xen_free_memory;
	long max_xen_free_memory, xen_free_target, prev_xen_free_memory;
	long avail, avail_with_slack, avail_less_slack;
	long max_avail, max_avail_with_slack, max_avail_less_slack;
	long lien, freeable, reclaim, reclaimed, cond_free_slack;
	domain_info* dom;

	/*
	 * Automatic memory adjustment must be paused,
	 * or the request does not make sense
	 */
	if (memsched_pause_level == 0)
	{
		*p_freemem_with_slack = 0;
		*p_freemem_less_slack = 0;
		return 'P';
	}

	/*
	 * CAVEAT:
	 *
	 * Unfortunately Xen does not provide a way for an outside application
	 * to really know Xen memory allocation and commitments.
	 *
	 * A proper memory allocation/commitments tracking facility ought to
	 * provide the following data:
	 *
	 *   1) Currently allocated G+V+X size for every domain.
	 *
	 *   2) Separately, X part of current allocation.
	 *
	 *   3) Target G+V+X size for every domain (distinct from item#1 for
	 *      domains in the process of expansion or contraction).
	 *
	 *   4) Separately, X part of target allocation if different from (2).
	 *
	 *   5) Summary (1) and (3) for all the domains.
	 *
	 *   6) Liens data should be lockable (perhaps for lien increase only).
	 *
	 * Unfortunately as of current version (4.4) Xen does not provide this
	 * data.
	 *
	 * There are "target" and "videoram" sizes recorded in Xenstore, but they
	 * constitute only "G+V" part of domain size which does not include the
	 * "X" part -- the size of a domain's Xen internal data area which is
	 * not stored or published anywhere. Therefore capturing "target" and
	 * "videoram" values for all the domains
	 *
	 *     (overhead issues aside -- but this can be worked around by
	 *      reading watch-updates from Xenstore /local/domain key and
	 *      its subkeys, rather than reading Xenstore keys every time
	 *      the values are needed)
	 *
	 * does not provide us means to find "true free size" because the "X"
	 * part cannot be accounted for. Speaking in terms of Xen interface
	 * structures, knowing "target" + "videoram" does not let us reason
	 * about xc_domaininfo_t.tot_pages and vice versa. In effect, Xen/XL
	 * created two disjoint spaces of memory sizing with no conversion
	 * between them possible for an outside application.
	 *
	 * Domains may be in the process of shrinking or expansion (including
	 * domains not managed by membalance) and their current size can be
	 * distinct from their allocation targets and in the process of moving
	 * towards the targets. Thus capturing just current size or free memory
	 * does not provide us "true free size" until such movement is completed,
	 * since Xen does not let us know the outstanding commitments.
	 *
	 * Thus the only way left for us to find or rather pray to approximate
	 * "true free size" is to wait for domain resizings in progress to
	 * complete by observing free memory size to stabilize over some time.
	 *
	 * This is very unreliable because:
	 *
	 *    1) A domain in the process of expansion or contraction may temporary
	 *       stall, either because it is not alloted enough CPU time, or because
	 *       some intra-guest activity temporarily preempted the balloon driver.
	 *
	 *    2) A domain can be paused.
	 *
	 *    3) Multiple domains can be expanding and shrinking simultaneously
	 *       compensating each others group impact on free memory and creating an
	 *       illusion that a stabilization had been achieved.
	 *
	 * Once these conditions clear, domain will resume its expansion or
	 * contraction, or multi-domain expansion and contraction will get out of mutual
	 * balance, and the assumption of having acquired "true free memory size" based
	 * on an apparent stability of free memory amount on the host will prove wrong.
	 *
	 * To reiterate, relying on "stable free memory size" obsveration is damned
	 * unreliable, but unfortunately Xen does not provide us any better way.
	 */

	/*
	 * Collect memory allocation data
	 */
	timeout = timeout * MSEC_PER_SEC - config.domain_freemem_timeout;
	timeout = max(0, timeout);
	if (testmode)  timeout = 0;
	xen_free_memory = xen_wait_free_memory_stable(timeout);
	xen_free_slack = get_xen_free_slack();
	collect_domain_memory_info();
	lien = eval_memory_lien();

	/*
	 * Calculate absolute maximum of memory attainable after
	 * shrinking all managed runnable domains down to dmem_min
	 */
	freeable = 0;
	foreach_managed_domain(dom)
	{
		if (dom->memsize > dom->dmem_min && runnable(dom))
			freeable += dom->memsize - dom->dmem_min;
	}
	max_xen_free_memory = xen_free_memory + freeable;

	/*
	 * Calculate absolute maximum of memory that might be made available
	 */
	max_avail = calc_avail(max_xen_free_memory, lien, above_slack, draw_reserved_hard);
	max_avail_with_slack = calc_avail(max_xen_free_memory, lien, false, draw_reserved_hard);
	max_avail_less_slack = calc_avail(max_xen_free_memory, lien, true, draw_reserved_hard);

	/*
	 * check if @req is not negative or zero
	 */
	if (req <= 0)
	{
		*p_freemem_with_slack = max_avail_with_slack;
		*p_freemem_less_slack = max_avail_less_slack;
		return 'A';
	}

	/*
	 * check if @req is sane and not prone to easy overflow,
	 * to avoid arithmetic overflow in subsequent calculations
	 */
	if (req >= LONG_MAX / 2)
	{
		*p_freemem_with_slack = 0;
		*p_freemem_less_slack = 0;
		return 'A';
	}

	/*
	 * Allocation will be in quants
	 */
	req = roundup(req, memquant_kbs);

	/*
	 * Check if sufficient amount of free memory is attainable
	 * at all by shrinking down managed domains.
	 */
	if (must && req > max_avail)
	{
		*p_freemem_with_slack = max_avail_with_slack;
		*p_freemem_less_slack = max_avail_less_slack;
		return 'N';
	}

	/*
	 * check if @req is already available without shrinking
	 * down managed domains and drawing on draw_reserved_hard
	 */
	avail = calc_avail(xen_free_memory, lien, above_slack, false);
	avail_with_slack = calc_avail(xen_free_memory, lien, false, false);
	avail_less_slack = calc_avail(xen_free_memory, lien, true, false);
	if (req <= avail)
	{
		*p_freemem_with_slack = avail_with_slack;
		*p_freemem_less_slack = avail_less_slack;
		return 'A';
	}

	/*
	 * Perform domain squeeze scheduling
	 */
	cond_free_slack = above_slack ? xen_free_slack : 0;
	reclaim = req + config.host_reserved_hard + cond_free_slack + lien - xen_free_memory;
	reclaim = max(reclaim, 0);
	reclaim = min(reclaim, freeable);
	reclaim = roundup(reclaim, memquant_kbs);
	reclaimed = hard_reclaim(reclaim);

	/*
	 * Bug catching sieve: should never happen
	 */
	if (reclaimed < reclaim)
	{
		error_msg("bug: sched_freemem: reclaimed (%ld) < reclaim (%ld)",
			  reclaimed, reclaim);
		if (must)
		{
			*p_freemem_with_slack = avail_with_slack;
			*p_freemem_less_slack = avail_less_slack;
			return 'N';
		}
	}

	/*
	 * Execute shrinking
	 */
	foreach_managed_domain(dom)
	{
		if (dom->memsize < dom->memgoal0 && runnable(dom))
		{
			log_resize(dom, "shrink");
			do_shrink_domain(dom, dom->memsize);

			/* record shrinkage data */
			if (dom->preshrink_tick != sched_tick)
			{
				dom->preshrink_tick = sched_tick;
				dom->preshrink = 0;
			}
			dom->preshrink += max(0, dom->memsize0 - dom->memsize);
		}
	}

	/*
	 * Wait for a limited time for shrinking to complete to the target.
	 * Result may be less than a target if some domains fail to shrink
	 * promptly enough.
	 */
	prev_xen_free_memory = xen_free_memory;
	xen_free_target = xen_free_memory + min(reclaimed, reclaim);
	xen_free_memory = xen_wait_free_memory(xen_free_target,
					       config.domain_freemem_timeout);

	if (xen_free_memory < xen_free_target)
	{
		warning_msg("membalancectl free-memory was unable to reclaim "
			    "enough memory: reclaimed only %ld instead of %ld kbs",
			    xen_free_memory - prev_xen_free_memory,
			    xen_free_target - prev_xen_free_memory);
	}

	/*
	 * Outstandling lien might have changed during xen_wait_free_memory(...)
	 */
	collect_domain_memory_info();
	lien = eval_memory_lien();

	/*
	 * Report how much we got
	 */
	*p_freemem_with_slack = calc_avail(xen_free_memory, lien, false, draw_reserved_hard);
	*p_freemem_less_slack = calc_avail(xen_free_memory, lien, true, draw_reserved_hard);

	return 'A';
}


/******************************************************************************
*                             memory lien calculation                         *
******************************************************************************/

/*
 * Evaluate the amount of outstanding lien on Xen free memory.
 *
 * Currently (see CAVEAT notes above) the only lien source we are able to
 * (at least partially meaningfully) account for is managed domains in the
 * paused state with expansion pending.
 */
static long eval_memory_lien(void)
{
	long delta, lien = 0;
	domain_info* dom;

	foreach_managed_domain(dom)
	{
		if (dom->xcinfo->flags & XEN_DOMINF_paused)
		{
			delta = dom->memgoal0 + dom->xen_data_size - dom->memsize0;
			lien += max(0, delta);
		}

	}

	return lien;
}

