/*
 *  MEMBALANCE daemon
 *
 *  domain_info.h - Membalance descriptor for Xen domain
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

#ifndef __MEMBALANCE_DOMAIN_INFO_H__
#define __MEMBALANCE_DOMAIN_INFO_H__

/*
 * Domain control modes:
 *
 *     AUTO - automaticallu adjust domain memory allocation based on memory
 *            demand/pressure readings reported by the domains and available
 *            host memory amount
 *
 *     DIRECT - adjust domain size according to the instructions from the
 *              domain, target domain size is calculated by the domain
 *
 */
#define CTRL_MODE_AUTO	   (1 << 0)
#define CTRL_MODE_DIRECT   (1 << 1)

class rate_record
{
public:
	u_long tick;
	long rate;

	rate_record(u_long tick, long rate)
	{
		this->tick = tick;
		this->rate = rate;
	}
};

/*
 * Indicate what this domain is doing in the current inter-domain memory
 * rebalancing cycle: expanding, shrinking or staying neutral.
 */
typedef enum __balside
{
	RebalanceSide_NEUTRAL = 0,
	RebalanceSide_EXPANDING,
	RebalanceSide_SHRINKING
}
balside_t;

/*
 * PV domains have xenstore key "memory/videoram" set to -1,
 * so we use another value to designate unknown value
 */
#define XS_MEM_VIDEORAM_UNSET  (-11)

/*
 * Membalance keeps @domain_info object per each Xen domain
 * in managed or pending state
 */
class domain_info
{
public:
	domain_info(long domain_id);
	~domain_info();

public:
	long 	domain_id; 	    	/* xen domain id */
	char*   qid;    		/* id for membalance keys in xenstore */

	/**********************************************************************
	*                 Processing of "pending" state                       *
	**********************************************************************/

	struct timespec ts_noticed;     /* time when entered pending state */
	u_long	pending_cycle;    	/* number of times process_pending(...) was called */
	u_int	pending_skipped;       	/* ... and was skipped lately */
	u_long  resolved_config_seq;    /* config seq# for last resolve_settings() or 0 */

	/**********************************************************************
	*                  Settings read from Xenstore                        *
	**********************************************************************/

	char*	vm_name;		/* xen domain name */
	char*	vm_uuid;		/* xen domain uuid */
	long	xs_mem_max;		/* memory allocations from xen config */
	long	xs_mem_target;     	/* ... */
	long	xs_mem_videoram;   	/* ... */
	char*	xs_start_time;		/* vm start time*/

	/**********************************************************************
	*             Settings read from Xen domain config file               *
	**********************************************************************/

	tribool config_file_status;	/* processed domain config file? */
	u_int	ctrl_modes_allowed;     /* enabled domain control modes,
					   as a bitmask: CTRL_MODE_xxx */
	long	xc_memory;		/* "memory" (in KBs) */
	long	xc_maxmem;		/* "maxmem" (in KBs) */
	long	xc_dmem_max;		/* "membalance_dmem_max" (in KBs) */
	long	xc_dmem_quota;		/* "membalance_dmem_quota" (in KBs) */
	long	xc_dmem_min;		/* "membalance_dmem_min" (in KBs) */
	double	xc_dmem_incr;		/* "membalance_dmem_incr" (in fractions, i.e. %/100) */
	double	xc_dmem_decr;		/* "membalance_dmem_decr" (in fractions, i.e. %/100) */
	long	xc_rate_high;		/* "membalance_rate_high" (in KB/s) */
	long	xc_rate_low;		/* "membalance_rate_low" (in KB/s) */
	long	xc_rate_zero;		/* "membalance_rate_zero" (in KB/s) */
	double	xc_guest_free_threshold;     /* "guest_free_threshold" (in fractions, i.e. %/100) */
	int	xc_startup_time;	/* "membalance_startup_time" in seconds */
	int	xc_trim_unresponsive;	/* "membalance_trim_unresponsive" in seconds */
	tribool	xc_trim_unmanaged;	/* "membalance_trim_unmanaged" yes/no/maybe */

	/**********************************************************************
	*                            Active settings 			      *
	*    based on Xen domain config file settings after being resolved    *
	*      with defaults supplied by membalance global configuration      *
	**********************************************************************/

	u_int	ctrl_mode;         	/* currently active domain control mode:
					   one of CTRL_MODE_xxx or 0 if unknown */
	long	dmem_max;		/* in KBs */
	long	dmem_quota;		/* in KBs */
	long	dmem_min;		/* in KBs */
	double	dmem_incr;		/* in fractions (%/100) */
	double	dmem_decr;		/* in fractions (%/100) */
	long	rate_high;		/* in KB/s */
	long	rate_low;		/* in KB/s */
	long	rate_zero;		/* in KB/s */
	double	guest_free_threshold;	/* in fractions (%/100) */
	int	startup_time;		/* in seconds */
	int	trim_unresponsive;	/* in seconds */
	bool	trim_unmanaged;		/* yes/no */
	long	xen_data_size;		/* xen private data size (KBs),
	                                   see explanation in sched.cpp */
	u_int	xen_data_size_phase;	/* xen_data_size acquisition phase */

	/**********************************************************************
	*        Fields used internally by memory scheduling algorithm,       *
	*         with life scope across invocations of sched_memory()        *
	**********************************************************************/

	u_long 	last_report_tick;	/* last time domain reported (sched tick#) */
	long 	no_report_time;		/* aggregated time the domain did not provide
					   a report (sec) while being runnable */
	std::vector<rate_record>     	/* rate history data */
		rate_history;
	long	time_rate_below_low;    /* time rate was <= @rate_low */
	long	time_rate_below_high;   /* time rate was < @rate_high */
	u_long 	last_expand_tick;	/* last time domain was expanded (sched tick#) */

	/*
	 * If sched_freemem(...) shrinks this domain in between ticks to meet the
	 * needs of "membalancectl free-memory" command, it records the amount of
	 * performed shrinking in @preshrink (and previous tick# in @preshrink_tick),
	 * so during the next tick this shrinkage can be factored into the adjustment
	 * of domain size over the tick time.
	 */
	long	preshrink;      	/* the amount "freemem" already shrunk this dom */
	u_long	preshrink_tick; 	/* shrinkage tick# */

	/**********************************************************************
	*        Fields used internally by memory scheduling algorithm,       *
	*     with life scope only within an invocation of sched_memory()     *
	**********************************************************************/

	const xc_domaininfo_t* xcinfo;	/* xenctrl-level info */
	char*	report_raw;		/* raw domain report string */
	bool	valid_data;		/* has valid rate etc. data for current tick to go on */
	bool	valid_memory_data;	/* has valid memory sizing data for current tick */
	bool	trimming_to_quota;	/* @true if currently trimming to @dmem_quota */
	balside_t balside;		/* expanding, shrinking or staying neutral */
	long 	rate;   		/* latest rate reading */
	double	freepct;		/* latest guest free memory %-age reading */
	long 	fast_rate;		/* fast moving average of rate */
	long 	slow_rate;		/* slow moving average of rate */

	/* memory amount (KBs) targeted by domain (less xen_data_size) */
	long	memgoal0;    		/* ... at the start of the tick */

	/* memory amount (KBs) physically allocated to domain (less xen_data_size) */
	long 	memsize0;       	/* ... at the start of the tick */
	long 	memsize;		/* ... targeted by the scheduler */

	long	memsize_incr;		/* on domain expansion normally increase
					   ... its size up to here */
	long	memsize_decr;		/* on domain contraction normally decrease
					   ... its size up to here */

	double	expand_force;		/* domain pressure force to expand outwards */
	double	resist_force;   	/* domain force to resist contraction */

	double	expand_force0;		/* expansion force at the fisrt slice */

	/**********************************************************************
	*          Fields for tracking the change in xen_data_size            *
	**********************************************************************/

	u_int   xds_phase;		/* ticks memsize0 and memgoal0 were stable */
	long	xds_totsize0;		/* previous tick's memsize0 + xds */
	long	xds_memgoal0;   	/* previous tick's memgoal0 */

	/**********************************************************************
	*                            Methods                                  *
	**********************************************************************/
public:
	/* get human-readable name of this domain */
	const char* printable_name(void);

	/* called when domain is placed on pending domain list */
	void on_enter_pending(void);

	/* called when domain is placed on managed domain list */
	void on_enter_managed(void);

	/* process pending state (try to gather domain data) */
	tribool process_pending(bool* p_is_dead);

	/* issue a message when was unable to collect data for the domain */
	void pending_timeout_message(void);

	/* resolve domain settings vs. global defaults */
	bool resolve_settings(void);

	/*
	 * check if the change old_config -> config affects
	 * resolve_settings(...)
	 */
	static bool resolve_settings_affected(const membalance_config& old_config);

	/* store generated qid */
	void set_qid(const char* qid);

	/* called at the beginning of scheduling tick */
	void begin_sched_tick(void);

	/* parse raw report string */
	bool parse_domain_report(void);

	/* calculate slow and fast moving averages of rate */
	void calc_rates(unsigned long tick);

	/* re-evaluate xen_data_size */
	void reeval_xen_data_size(long xen_free);

protected:
	bool is_xs_data_complete(void);
	char process_pending_read_xs(void);
	char eval_xen_data_size(void);
	void retrieve_config_file_settings(void);
	bool parse_config_file_settings(XLU_Config* config,
					const char* config_source,
					const map_ss& kv);
	void undefined_setting(const char* key);
	double weight_samples(const double* weights, int nweights);

private:
	domain_info();

private:
	std::string printable_name_buf;
};

#endif // __MEMBALANCE_DOMAIN_INFO_H__

