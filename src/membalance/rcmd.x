/*
 *  MEMBALANCE daemon
 *
 *  rcmd.x - define RPC interface for management commands between
 *           MEMBALANCED and MEMBALANCECTL
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

struct rcmd_status
{
	int status;
	string message<>;
};

typedef struct rcmd_kv_listentry *rcmd_kv_listentry_ptr;
struct rcmd_kv_listentry
{
	rcmd_kv_listentry_ptr next;
	string key<>;
	string value<>;
};

typedef struct rcmd_str_listentry *rcmd_str_listentry_ptr;
struct rcmd_str_listentry
{
	rcmd_str_listentry_ptr next;
	string value<>;
};

struct rcmd_domain_settings_res
{
	int status;
	string message<>;
	struct rcmd_kv_listentry *kvs;
};

struct rcmd_freemem_res
{
	/*
	 * status:
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
	int status;
	unsigned hyper freemem_with_slack;      /* total free memory */
	unsigned hyper freemem_less_slack;      /* free memory - slack */
};

program RCMD_MEMBALANCED {
	version RCMD_MEMBALANCED_V1 {
		/*
		 * Ping
		 */
		void
		rcmd_null(void) = 0;

		/*
		 * Pause domain memory adjustment.
		 * Returns new pause level.
		 */
		unsigned int /*new_pause_level*/
		rcmd_pause(void) = 1;

		/*
		 * Resume domain memory adjustment.
		 * If @force is @false, decrement pause level by 1.
		 * If @force is @true, reset pause level to 0.
		 * Returns new pause level.
		 */
		unsigned int /*new_pause_level*/
		rcmd_resume(bool /*force*/) = 2;

		/*
		 * Try to bring host free memory up to the requested amount,
		 * by shrinking domains if necessary. 
		 *
		 * @above_slack is @true if the amount is on top of Xen free
		 * memory slack, otherwise @false. 
		 *
		 * If @use_reserved_hard is true, membalance will draw on
		 * host_reserved_hard if necessary.
		 *
		 * If @must is true and target result is not achievable,
		 * membalance won't even try shrinking domains.
		 *
		 * Return the amount of free host memory attained (with and
		 * without slack) in case of success or attainable in case
		 * of failure. The amounts are in KBs.
		 *
		 * If Xen memory is in the state of a flux, e.g. domains
		 * are continuing to expand or contract, allow membalance daemon
		 * to wait up to @timeout seconds for the flux to stabilize
		 * if deemed necessary.
		 */
		rcmd_freemem_res
		rcmd_freemem(unsigned hyper /*amt_kbs*/, 
			     bool /*above_slack*/,
			     bool /*use_reserved_hard*/,
			     bool /*must*/,
			     int  /*timeout*/) = 3;

		/*
		 * Rescan domain and try to make it managed.
		 * When returns, the domain may still be in a pending state.
		 * If @domain_id is -1, rescan all unmanaged domains.
		 */
		rcmd_status
		rcmd_manage_domain(unsigned hyper /*domain_id*/) = 4;

		/*
		 * Show membalance status (settings, domains etc.).
		 * The argument is a verbosity level.
		 */
		string /*human-readable*/
		rcmd_show_status(int /*verbosity*/) = 5;

		/*
		 * Dump debugging state to log file
		 */
		void
		rcmd_debug_dump(void) = 6;

		/*
		 * Dump debugging state to a result string
		 */
		string /*human-readable*/
		rcmd_debug_dump_to_string(void) = 7;

		/*
		 * Set logging level.
		 * If parameter is -1, do not change logging level.
		 * Returns previous logging level (effective before the call).
		 */
		int /*previous-logging-level*/
		rcmd_set_debug_level(int /*debug_level*/) = 8;

		/*
		 * Direct logging to membalanced log (parameter = 1)
		 * or to syslog (parameter = 0).
		 * If parameter is -1, do not change logging sink.
		 * Returns previous sink identifier (effective before the call).
		 */
		int /*previous-sink-id*/
		rcmd_set_logging_sink(int /*sink_id*/) = 9;

		/*
		 * Show domain settings
		 */
		rcmd_domain_settings_res
		rcmd_get_domain_settings(unsigned hyper /*domain_id*/) = 10;

		/*
		 * Change domain settings
		 */
		rcmd_domain_settings_res /*updated-settings*/
		rcmd_set_domain_settings(unsigned hyper /*domain_id*/,
		                         rcmd_kv_listentry_ptr /*properties*/) = 11;

		/*
		 * Execute development-time test
		 */
		rcmd_status
		rcmd_test(rcmd_str_listentry_ptr /*argv*/) = 12;
	} = 1;
} = 0x40000001;

