/*
 *  MEMBALANCE daemon
 *
 *  config_def.h - Define configuration parameters,
 *                 mostly readable from /etc/membalance.conf
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 *
 *  This file can be included multiple times, with various definitions
 *  of CONFIG_ITEM and CONFIG_ITEM_CONST.
 */

/*
 * Data rate sampling interval.
 *
 * Memprobed takes memory pressure reading every @interval seconds
 * and reports it to membalance(d) running in the hypervisor domain.
 * Initial default value is 5 seconds, but is dynamically adjustable
 * by membalanced.
 */
CONFIG_ITEM(interval, int, 5)

/*
 * Interval tolerance.
 *
 * Consider time points closer than @tolerance_ms to targeted sample taking
 * time to be good enough for sample taking.
 */
CONFIG_ITEM_CONST(tolerance_ms, int, 200)

/*
 * Maximum XenStore transaction retry attempts.
 * Used when starting or committing a transaction.
 */
CONFIG_ITEM(max_xs_retries, int, 20)

/*
 * Limit data update interval to a range of 2 ... 30 seconds
 */
CONFIG_ITEM_CONST(min_interval, int, 2)
CONFIG_ITEM_CONST(max_interval, int, 30)

/*
 * When starting up as a daemon and Xen has not fully completed its initialization
 * yet, wait up to @max_xen_init_retries seconds for Xen to initialize before
 * giving up, and log message about waiting after @xen_init_retry_msg seconds.
 */
CONFIG_ITEM(max_xen_init_retries, int, 300)
CONFIG_ITEM(xen_init_retry_msg, int, 15)

/*
 * If domain has been in pending state (with membalance trying to collect
 * data about it) and data collection did not complete within
 * @domain_pending_timeout seconds, declare the domain as unmanaged.
 */
CONFIG_ITEM(domain_pending_timeout, int, 300)

/*
 * Membalance will try to leave alone Xen host system memory in the amount
 * specified by @host_reserved_hard. Membalance will not expand domains if
 * there is less than @host_reserved_hard memory left, nor expand domains
 * by an amount that would leave less than @host_reserved_hard available.
 * In addition, if host system's free memory drops below @host_reserved_hard,
 * membalance will try to shrink managed domains to recover enough memory to
 * bring the amount of host system available memory back to @host_reserved_hard.
 *
 * The value of @host_reserved_hard is in addition to (on top of) Xen free
 * memory slack.
 *
 * Default value for @host_reserved_hard is 0.
 */
CONFIG_ITEM(host_reserved_hard, long, 0)	/* in KBs */

/*
 * Membalance will try to set the amount of host free memory defined by
 * @host_reserved_soft aside for use only by domains in substantial need
 * of memory: either domains below their @dmem_quota and with rate > @rate_low,
 * or domains any size with rate >= @rate_high.
 *
 * The value of @host_reserved_soft is in addition to (on top of) Xen free
 * memory slack.
 *
 * Default value for @host_reserved_soft is:
 *
 *     host_reserved_hard + 10% of (Xen physical memory -
 *      			    Xen free memory slack -
 *      			    Dom0 minimal size).
 */
CONFIG_ITEM(host_reserved_soft, long, 0)	/* in KBs */

/*
 * The values of @rate_high, @rate_low, @dmem_incr and @dmem_decr specified
 * in this file are used as the defaults for domains, unless they are overriden
 * on per-domain basis in individual Xen domain configuration files.
 *
 * These values control automatic expansion and contraction of memory allocation
 * to the domain. Domain has a greater claim if its data map-in rate (i.e.
 * virtual memory hard paging rate plus file system page cache block read-in
 * rate) exceeds @rate_high, the smallest claim if its rate is below @rate_low,
 * and intermediate claim if its rate is in between @rate_low and @rate_high.
 *
 * For instance, at each @interval, if domain data map-in rate exceeds
 * @rate_high and domain memory allocation is over the domain quota as specified
 * by @dmem_quota in the domain configuration, membalance will try to
 * dynamically expand the domain memory allocation by @dmem_incr.
 *
 * If data map-in rate is below @rate_high and domain memory allocation is over
 * its quota, and host free memory is in short supply, membalance will
 * dynamically shrink the domain memory allocation by @dmem_decr.
 *
 * Parameters @dmem_incr and @dmem_decr specify by how much membalance will try
 * to expand or contract a domain in a single @interval cycle, as a percentage
 * of current domain memory size. Normally the amount of domain expansion or
 * trimming within a single @interval cycle is limited to @dmem_incr and
 * @dmem_decr correspondingly, however in the event of dire host memory
 * shortage membalance can trim domains by more than @dmem_decr, even all way
 * down to @dmem_min.
 */
CONFIG_ITEM(rate_high, unsigned long, 200)	/* in KB/s */
CONFIG_ITEM(rate_low, unsigned long, 0)		/* in KB/s */
CONFIG_ITEM(dmem_incr, double, 0.06)		/* fractions (%/100) */
CONFIG_ITEM(dmem_decr, double, 0.04)		/* fractions (%/100) */

/*
 * Sanity limits for dmem_incr and dmem_decr (liberal at max),
 * as fractions (multiply by 100 to get percent)
 */
CONFIG_ITEM_CONST(min_dmem_decr, double, 0.005)	/* 0.5 %*/
CONFIG_ITEM_CONST(max_dmem_decr, double, 0.1)   /* 10% */

CONFIG_ITEM_CONST(min_dmem_incr, double, 0.005) /* 0.5% */
CONFIG_ITEM_CONST(max_dmem_incr, double, 0.3)   /* 30% */

/*
 * If guest data rate is <= @rate_zero, it is considered to be zero
 * regardless of the reported rate value.
 */
CONFIG_ITEM(rate_zero, unsigned long, 30)	/* in KB/s */

/*
 * If guest system has more than @guest_free_threshold percentage of
 * free system memory, its data rate is considered to be zero
 * regardless of the reported rate value.
 */
CONFIG_ITEM(guest_free_threshold, double, 0.15)	/* fractions (%/100) */

/*
 * To minimize the probability of a jitter of reallocating memory back and
 * forth between very similar domains (domain upsize/downsize jitter),
 * a domain that has been expanded not more than @shrink_protection_time
 * ticks back is not considered elgigible for shrinking for the purposes of
 * meeting @host_reserved_soft free memory constraint or domain rebalancing.
 * However this protection is not in effect for the purposes of meeting
 * @host_reserved_hard free memory constraint.
 */
CONFIG_ITEM_CONST(shrink_protection_time, u_int, 1)	/* ticks */

/*
 * Trim domain memory allocation down to @dmem_quota when the domain
 * transitions from managed to unmanaged, and is sized above @dmem_quota
 */
CONFIG_ITEM(trim_unmanaged, bool, true)

/*
 * Trim domain memory allocation down to @dmem_quota if it has not been
 * reporting its data map-in rate data (e.g. memprobed daemon stopped
 * running in the guest) for @trim_unresponsive seconds, while domain
 * has been staying runnable over this time, and domain size is over
 * @dmem_quota
 */
CONFIG_ITEM(trim_unresponsive, int, 200)

/*
 * Estimated threshold on domain guest OS startup time.
 *
 * This value can also be refined on a per-domain basis.
 *
 * If domain uptime is less than @startup_time and domain did not start to
 * report rate data yet, it may be given a benefit of the doubt that it did
 * not have a good-faith chance to start memprobed daemon yet and accorded
 * a somewhat more lenient treatment in certain memory shortage situations
 * compared to older non-reporting domains.
 */
CONFIG_ITEM(startup_time, int, 300)

/*
 * In each @interval cycle, membalance will determine required domain size
 * adjustments and then will first execute scheduled domain contractions,
 * and afterwards scheduled expansions. When free memory is in short supply,
 * the ability of membalance to perform the expansions depends on domains
 * being shrinked complying propmptly enough with contraction directives for
 * them. After performing the contractions, membalance will try to perform
 * expansions for up to @domain_expansion_timeout ms and will give up trying
 * to perform expansions after this during the current @interval cycle, in
 * case domain being shrinked fail to release memory promptly. In this event
 * domains needing to expand will have to wait till next @interval tick.
 *
 * @domain_expansion_timeout is calculated as:
 *
 *    min(interval * domain_expansion_timeout_frac,
 *        domain_expansion_timeout_max)
 *
 * Membalance will abort waiting for memory if it does not see any memory
 * being released over @domain_expansion_timeout_abort consecutive retry
 * cycles (each 100 ms).
 *
 * Do not set @domain_expansion_timeout too high.
 *
 * First, it should definitely be less than @interval, preferably well under
 * @interval. Second, while waiting for free memory, membalance does not
 * respond to signals and RPC control requests.
 */
CONFIG_ITEM_CONST(domain_expansion_timeout_frac, double, 0.3)
CONFIG_ITEM_CONST(domain_expansion_timeout_max, int, 5)
CONFIG_ITEM_CONST(domain_expansion_timeout_abort, int, 4)

/*
 * When "membalancectl free-memory" command causes managed domains to shrink
 * in order to supply the required memory, it will wait for domains shrinking
 * process to complete for up to @domain_freemem_timeout ms and if domains
 * being shrunk fail to release the requested memory within this time, the
 * command will either fail or (depending on its options) return with whatever
 * memory it was able to obtain.
 *
 * Do not set @domain_freemem_timeout too high.
 *
 * First, it should definitely be less than @interval, preferably well under
 * @interval. Second, while waiting for free memory, membalance does not
 * respond to signals and RPC control requests.
 */
CONFIG_ITEM_CONST(domain_freemem_timeout, int, 700)

/*
 * Enable or disable management of Dom0, in either AUTO or DIRECT modes.
 *
 * This is a flag bitmask of CTRL_MODE_AUTO and/or CTRL_MODE_DIRECT,
 * with either of the flags set or unset.
 *
 * Membalance management of Dom0 is currently not implemented, so this option
 * is undocumented and is to be currently left as "disabled" (0), since the
 * "enabled" setting:
 *
 * 1) Currently would not work because Dom0 has no XL config file, so we would
 *    need to reconstruct Dom0 membalance config information using different
 *    means.
 *
 * 2) Would require extra testing, in particular the stability of ballooning
 *    and deballooning of Dom0 from inside of Dom0 itself.
 *
 * 3) Interference with Dom0 "autoballoon" operations needs to be thought
 *    through.
 *
 * Even if these issues were to be addressed, it may be better to leave
 * dom0_mode default as "disabled" (0), to prevent accidental unintended
 * destabilization of the host system in case an administrator sets improper
 * membalance values for Dom0, such as too low DMEM_MIN.
 */
CONFIG_ITEM(dom0_mode, unsigned int, 0)

/*
 * The number of samples to take when determining the size of domain's Xen
 * private data area.
 */
CONFIG_ITEM_CONST(xen_private_data_size_samples, u_int, 3)

