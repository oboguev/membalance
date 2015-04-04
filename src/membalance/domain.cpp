/*
 *  MEMBALANCE daemon
 *
 *  domain.cpp - Xen domain descriptor handling
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

#include "membalanced.h"

/******************************************************************************
*                          forward declarations                               *
******************************************************************************/

static void show_domains(FILE* fp, const char* title, const domid2info& xdoms, char kind);
static const char* fetch_key(map_ss* kv, XLU_Config* xlu_config, const char* key);
static void inval(const char* config_source, const char* key);
static void show_status_header(FILE* fp);
static void show_status_global(FILE* fp);
static void show_status_memory(FILE* fp, long xen_free_memory);
static void show_managed_domains(FILE* fp);
static void show_pending_domains(FILE* fp);
static void show_unmanaged_domains(FILE* fp);
static void populate_sorted_by_id(std::vector<long>& vec, const domid2info& dmap);


/******************************************************************************
*                          basic domain routines                              *
******************************************************************************/

domain_info::domain_info(long domain_id)
{
	this->domain_id = domain_id;
	qid = NULL;

	pending_cycle = 0;
	pending_skipped = 0;

	vm_name = NULL;
	vm_uuid = NULL;
	xs_mem_max = -1;
	xs_mem_target = -1;
	xs_mem_videoram = XS_MEM_VIDEORAM_UNSET;
	xs_start_time = NULL;

	config_file_status = TriMaybe;
	ctrl_modes_allowed = 0;
	ctrl_mode = 0;
	xc_memory = -1;
	xc_maxmem = -1;
	xc_dmem_max = -1;
	xc_dmem_quota = -1;
	xc_dmem_min = -1;
	xc_dmem_incr = -1;
	xc_dmem_decr = -1;
	xc_rate_high = -1;
	xc_rate_low = -1;
	xc_rate_zero = -1;
	xc_guest_free_threshold = -1;
	xc_startup_time = -1;
	xc_trim_unresponsive = -1;
	xc_trim_unmanaged = TriMaybe;

	dmem_max = -1;
	dmem_quota = -1;
	dmem_min = -1;
	dmem_incr = -1;
	dmem_decr = -1;
	rate_high = -1;
	rate_low = -1;
	rate_zero = -1;
	guest_free_threshold = -1;
	startup_time = -1;
	trim_unresponsive = -1;
	trim_unmanaged = true;
	xen_data_size = 0;

	report_raw = NULL;
}

domain_info::~domain_info()
{
	free_ptr(qid);
	free_ptr(vm_name);
	free_ptr(vm_uuid);
	free_ptr(xs_start_time);
	free_ptr(report_raw);
}

void domain_info::on_enter_pending(void)
{
	ts_noticed = getnow();
	pending_cycle = 0;
	pending_skipped = 0;
	resolved_config_seq = 0;
	xen_data_size_phase = 0;
}

void domain_info::on_enter_managed(void)
{
	/*
	 * caution: do not use @sched_tick here as it may be adjusted by leaps
	 * by sched_slept(...) between here and the call to sched_memory(...)
	 */
	last_report_tick = 0;
	no_report_time = 0;
	time_rate_below_low = 0;
	time_rate_below_high = 0;
	valid_data = false;
	valid_memory_data = false;
	last_expand_tick = 0;
	preshrink = 0;
	preshrink_tick = 0;
	xds_phase = 0;
	xds_totsize0 = -1;
	xds_memgoal0 = -1;
}

void domain_info::set_qid(const char* qid)
{
	free_ptr(this->qid);
	this->qid = xstrdup(qid);
}

/*
 * Format domain printable name from known pieces of data
 */
const char* domain_info::printable_name(void)
{
	std::string& s = printable_name_buf;
	char buf[64];

	sprintf(buf, "%ld", domain_id);
	s = buf;

	if (vm_name && vm_uuid)
	{
		s += " (name: ";
		s += vm_name;
		s += ", uuid: ";
		s += vm_uuid;
		s += ")";
	}
	else if (vm_name)
	{
		s += " (name: ";
		s += vm_name;
		s += ")";
	}
	else if (vm_uuid)
	{
		s += " (uuid: ";
		s += vm_uuid;
		s += ")";
	}

	return s.c_str();
}


/******************************************************************************
*                        domain lifecycle routines                            *
******************************************************************************/

/*
 *
 *                   +------------------------+
 *                  /                         |
 *                 /                          v
 *   [New] --> [Pending] ---> [Managed] --> [Dead]
 *              ^  \             |            ^
 *              |   \            v            |
 *              |    +-----> [Unmanaged] -----+
 *      	|	         |
 *      	|	         |
 *      	+----------------+
 *
 */

bool is_domain_known(long domain_id)
{
	return contains(doms.managed, domain_id) ||
	       contains(doms.unmanaged, domain_id) ||
	       contains(doms.pending, domain_id);
}

domain_info* transition_new_pending(long domain_id)
{
	debug_msg(5, "domain %ld transition: new -> pending", domain_id);
	domain_info* dom = new domain_info(domain_id);
	doms.pending[domain_id] = dom;
	dom->on_enter_pending();
	return dom;
}

void transition_dead(long domain_id)
{
	if (contains(doms.managed, domain_id))
	{
		transition_managed_dead(domain_id);
	}
	else if (contains(doms.unmanaged, domain_id))
	{
		transition_unmanaged_dead(domain_id);
	}
	else if (contains(doms.pending, domain_id))
	{
		transition_pending_dead(domain_id);
	}
	else
	{
		qid_dead(domain_id);
	}
}

void unmanage_domain(long domain_id)
{
	if (contains(doms.managed, domain_id))
	{
		transition_managed_unmanaged(domain_id);
	}
	else if (contains(doms.pending, domain_id))
	{
		transition_pending_unmanaged(domain_id);
	}
}

void transition_managed_dead(long domain_id)
{
	debug_msg(5, "domain %ld transition: managed -> dead", domain_id);
	delete doms.managed[domain_id];
	doms.managed.erase(domain_id);
	qid_dead(domain_id);
	/*
	 * Academically speaking, we ought to remove dead domain from the ACL
	 * on the @membalance_interval_path key, however there is no harm in it
	 * being left there for a while, so save some CPU cycles and do not
	 * remove it. This domain will be removed from the ACL anyway next time
	 * any domain transitions pending -> managed.
	 */
	// update_membalance_interval_protection();
}

void transition_unmanaged_dead(long domain_id)
{
	debug_msg(5, "domain %ld transition: unmanaged -> dead", domain_id);
	// delete doms.unmanaged[domain_id];  // always "delete NULL" so no-op
	doms.unmanaged.erase(domain_id);
	qid_dead(domain_id);
}

void transition_pending_dead(long domain_id)
{
	debug_msg(5, "domain %ld transition: pending -> dead", domain_id);
	delete doms.pending[domain_id];
	doms.pending.erase(domain_id);
	qid_dead(domain_id);
}

void transition_pending_managed(long domain_id)
{
	debug_msg(5, "domain %ld transition: pending -> managed", domain_id);
	domain_info* dom = doms.pending[domain_id];
	notice_msg("starting to manage domain %s", dom->printable_name());
	doms.managed[domain_id] = dom;
	doms.pending.erase(domain_id);
	dom->on_enter_managed();
	on_add_managed();
}

void transition_pending_unmanaged(long domain_id)
{
	debug_msg(5, "domain %ld transition: pending -> unmanaged", domain_id);
	if (domain_id != 0 || config.dom0_mode)
	{
		notice_msg("will not manage domain %s",
			   doms.pending[domain_id]->printable_name());
	}
	delete doms.pending[domain_id];
	doms.pending.erase(domain_id);
	doms.unmanaged[domain_id] = NULL;
}

void transition_managed_unmanaged(long domain_id)
{
	debug_msg(5, "domain %ld transition: managed -> unmanaged", domain_id);
	domain_info* dom = doms.managed[domain_id];
	notice_msg("stopping to manage domain %s", dom->printable_name());
	if (dom->trim_unmanaged)
		trim_to_quota(dom);
	delete dom;
	doms.managed.erase(domain_id);
	doms.unmanaged[domain_id] = NULL;
	test_debugger();
}

void transition_unmanaged_pending(long domain_id)
{
	debug_msg(5, "domain %ld transition: unmanaged -> pending", domain_id);
	// delete doms.unmanaged[domain_id];  // always "delete NULL" so no-op
	doms.unmanaged.erase(domain_id);
	domain_info* dom = new domain_info(domain_id);
	doms.pending[domain_id] = dom;
	dom->on_enter_pending();
}

/******************************************************************************
*                         reexamine domain status                             *
******************************************************************************/

/*
 * daemon config parameters affecting domain_info::resolve_settings(...)
 * etc. may have changed, making some currently unmanaged domains manageable
 * and vice versa
 *
 * rescan both managed and unmanaged domains
 */
void rescan_domains_on_config_change(const membalance_config& old_config)
{
	/*
	 * if Dom0 management-enable status has changed, handle it
	 */
	if (config.dom0_mode)
	{
		if (contains(doms.unmanaged, 0))
			transition_unmanaged_pending(0);
	}
	else
	{
		if (contains(doms.managed, 0))
			transition_managed_unmanaged(0);
		else if (contains(doms.pending, 0))
			transition_pending_unmanaged(0);
	}

	domid_set mids, umids;
	keyset(mids, doms.managed);
	keyset(umids, doms.unmanaged);

	/*
	 * update current settings for managed domains (merging the defaults
	 * from new global config) and also unmanage those previously managed
	 * domains, whose aggregate settings went invalid or incomplete
	 * as a result of global config change
	 */
	for (domid_set::const_iterator it = mids.begin(); it != mids.end(); ++it)
	{
		long domain_id = *it;
		domain_info* dom = doms.managed[domain_id];
		if (!dom->resolve_settings())
			transition_managed_unmanaged(domain_id);
	}

	/*
	 * try to re-examine previously unmanaged domains, in case they were
	 * not manageable due to failing resolve_settings(...), but now new
	 * global settings may possibly make then manageable
	 */
	if (domain_info::resolve_settings_affected(old_config))
	{
		for (domid_set::const_iterator it = umids.begin(); it != umids.end(); ++it)
		{
			long domain_id = *it;
			if (domain_id != 0 || config.dom0_mode)
				transition_unmanaged_pending(domain_id);
		}
	}
}

/*
 * System administrator manually requested to manage the domain (or all currently
 * unmanaged domains) via membalancectl command "manage-domain <domid>".
 * If @domain_id is -1, rescan all unmanaged domains, otherwise specified domain.
 */
int rescan_domain(long domain_id, char** message)
{
	if (domain_id == -1)
	{
		/* rescan all currently unmanaged domains */
		int res = 'M';
		domid_set ids;
		keyset(ids, doms.unmanaged);

		for (domid_set::const_iterator it = ids.begin();  it != ids.end();  ++it)
		{
			domain_id = *it;
			if (domain_id == 0 && !config.dom0_mode)
				continue;
			transition_unmanaged_pending(domain_id);
			res = 'P';
		}

		*message = (res == 'M') ? xprintf("There is currently no unmanaged domains")
					: xprintf("Unmanaged domains are being rescanned");

		return res;
	}
	else if (domain_id == 0 && !config.dom0_mode)
	{
		*message = xprintf("Dom0 management is disabled");
		return 'X';
	}
	else if (contains(doms.managed, domain_id))
	{
		*message = xprintf("Domain %ld is already managed", domain_id);
		return 'M';
	}
	else if (contains(doms.unmanaged, domain_id))
	{
		transition_unmanaged_pending(domain_id);
		*message = xprintf("Unmanaged domain %ld is being rescanned", domain_id);
		return 'P';
	}
	else if (contains(doms.pending, domain_id))
	{
		*message = xprintf("Domain %ld is already being rescanned", domain_id);
		return 'P';
	}
	else
	{
		*message = xprintf("Domain %ld does not exist", domain_id);
		return 'X';
	}
}


/******************************************************************************
*                          pending domain routines                            *
******************************************************************************/

/*
 * Called at startup to enumerate all known local domains
 * and add them as pending
 */
void enumerate_local_domains_as_pending(void)
{
	/* enumerate all local domain ids */
	domid_set ids;
	enumerate_local_domains(ids);

	/* add them to pending */
	for (domid_set::const_iterator it = ids.begin(); it != ids.end(); ++it)
	{
		long domain_id = *it;
		if (!is_domain_known(domain_id))
			transition_new_pending(domain_id);
	}
}

/*
 * Called periodically (up to once a second) to process all pending domains
 * and classify them as either managed or unmanaged, or if classification is
 * still impossible, keep the domain as pending till next invocation.
 */
void process_pending_domains(void)
{
	/* back out right away if nothing to do */
	if (doms.pending.empty())
		return;

	domid_set new_managed_ids;
	domid_set ids;
	keyset(ids, doms.pending);

	/*
	 * Iterate all pending domain and try to resolve them as either
	 * managed, unmanaged, dead or still pending
	 */
	for (domid_set::const_iterator it = ids.begin(); it != ids.end(); ++it)
	{
		long domain_id = *it;
		domain_info* info = doms.pending[domain_id];

		bool isdead;
		tribool status = info->process_pending(&isdead);

		if (isdead)
		{
			/* domain has been destroyed */
			transition_pending_dead(domain_id);
			continue;
		}

		switch (status)
		{
		case TriTrue:
			/* make managed */
			transition_pending_managed(domain_id);
			new_managed_ids.insert(domain_id);
			break;

		case TriFalse:
			/* make unmanaged */
			transition_pending_unmanaged(domain_id);
			break;

		case TriMaybe:
			/* if was unable to resolve this domain status for too long,
			   issue a message and declare it unmanaged */
			if (timespec_diff_ms(getnow(), info->ts_noticed) >
			    config.domain_pending_timeout * MSEC_PER_SEC)
			{
				info->pending_timeout_message();
				transition_pending_unmanaged(domain_id);
			}
			/* otherwise try again on next cycle */
			break;
		}
	}

	/*
	 * For all domains transitioned pending->managed, create domain-specific
	 * membalance structure (keys) in xenstore and assign them approriate access
	 * rights for Dom0 and target domain.
	 */
	for (domid_set::const_iterator it = new_managed_ids.begin(); it != new_managed_ids.end(); ++it)
	{
		long domain_id = *it;
		if (!init_membalance_report(domain_id))
			transition_managed_unmanaged(domain_id);
	}

	/*
	 * If managed list changed, update protection on the key (Dom0=rw, all managed=r)
	 * and write /membalance/interval to send a message to new domain.
	 */
	if (!new_managed_ids.empty())
	{
		update_membalance_interval_and_protection();
	}
}

/*
 * Issue a message that membalance has been unable to collect data
 * for the domain and is listing is as unmanaged.
 */
void domain_info::pending_timeout_message(void)
{
	std::string dname = printable_name();
	std::string ms;
	const char* cp;

	error_msg("failed to collect data for domain %s within %d seconds, "
		  "treating it as unmanaged by membalance",
		  dname.c_str(),
		  config.domain_pending_timeout);

	if (!vm_name)		     ms += ", name";
	if (!vm_uuid)		     ms += ", uuid";
	if (xs_mem_max == -1)        ms += ", xs mem max";
	if (xs_mem_target == -1)     ms += ", xs mem target";

	if (xs_mem_videoram == XS_MEM_VIDEORAM_UNSET)   ms += ", xs mem videoram";
	if (!xs_start_time && domain_id != 0)  		ms += ", xs start time";

	if (ms.length() == 0)
	{
		if (xen_data_size_phase != config.xen_private_data_size_samples)
			ms += ", xen private data size";
	}

	if (ms.length() == 0)
	{
		if (config_file_status != TriTrue)
			ms += ", xen domain config file";
	}

	cp = ms.c_str();
	if (*cp)
		cp += 2;
	else
		cp = "nothing";

	error_msg("data missing for domain %ld: %s", domain_id, cp);
}

/*
 * Rertrieve Xen configuration file for the domain and parse membalance related
 * settings in it.
 *
 * Store the result status in config_file_status:
 *   If successful:        @true
 *   If permanent errror:  @false
 *   To retry again:       @maybe
 */
void domain_info::retrieve_config_file_settings(void)
{
	/* already processed? */
	if (config_file_status != TriMaybe)
		return;

	uint8_t *config_data = NULL;
	int config_len = 0;
	int rc;

	XLU_Config* xlu_config = NULL;
	char config_source[256];
	char* parser_msg_buffer = NULL;
	size_t parser_msg_buffer_size = 0;
	FILE* parser_msg_fp = NULL;
	int e;
	map_ss kv;

	/*
	 * When a VM is created, libxl_userdata_store stores the whole XL CFG file
	 * content in Xen temporary storage area. Fetch it.
	 */
        rc = libxl_userdata_retrieve(xl_ctx, domain_id, "xl", &config_data, &config_len);
	if (rc)
		goto cleanup;

	/*
	 * Parse fetched config file
	 */

	/* format config source name for parser error messages */
	snprintf(config_source, countof(config_source),
		 "config for domain %s", printable_name());

	/*
	 * A glitch in Xen can sometimes result in empty config file data being
	 * returned due to a transient condition. This is most likely to be
	 * observable when the very first DomU is being started, and on the very
	 * first pass through process_pending(...). Second pass will typically
	 * retrive completed config file data.
	 */
	if (config_len == 0)
	{
		debug_msg(5, "xen (libxl) returned empty %s, will retry reading",
			  config_source);
		goto cleanup;
	}

	/* create in-memory file for parser messages */
	parser_msg_fp = open_memstream(&parser_msg_buffer, &parser_msg_buffer_size);
	if (!parser_msg_fp)
	{
		error_perror("unable to create in-memory file");
		goto cleanup;
	}

	/* init the parser */
	xlu_config = xlu_cfg_init(parser_msg_fp, config_source);
	if (!xlu_config)
	{
		error_msg("unable to initialize Xen config parser");
		goto cleanup;
	}

	/* parse config */
	e = xlu_cfg_readdata(xlu_config, (const char*) config_data, config_len);
	if (e)
	{
		error_msg("unable to parse %s: %s", config_source, strerror(e));
		goto unmanage;
	}

	/*
	 * Fetch membalance keys from domain config data
 	 */
	fetch_key(&kv, xlu_config, "membalance_mode");
	fetch_key(&kv, xlu_config, "membalance_dmem_max");
	fetch_key(&kv, xlu_config, "membalance_dmem_min");
	fetch_key(&kv, xlu_config, "membalance_dmem_quota");
	fetch_key(&kv, xlu_config, "membalance_dmem_incr");
	fetch_key(&kv, xlu_config, "membalance_dmem_decr");
	fetch_key(&kv, xlu_config, "membalance_rate_high");
	fetch_key(&kv, xlu_config, "membalance_rate_low");
	fetch_key(&kv, xlu_config, "membalance_rate_zero");
	fetch_key(&kv, xlu_config, "membalance_guest_free_threshold");
	fetch_key(&kv, xlu_config, "membalance_startup_time");
	fetch_key(&kv, xlu_config, "membalance_trim_unresponsive");
	fetch_key(&kv, xlu_config, "membalance_trim_unmanaged");

	if (!parse_config_file_settings(xlu_config, config_source, kv))
		goto unmanage;

	/*
	 * Config file settings retrieved.
	 * They still may be incomplete or incoherent.
	 * resolve_settings(...) is responsible for validation.
	 */
	config_file_status = TriTrue;
	goto cleanup;

unmanage:
	config_file_status = TriFalse;

cleanup:

	if (xlu_config)
		xlu_cfg_destroy(xlu_config);

	if (parser_msg_fp)
		fclose(parser_msg_fp);

	free_ptr(parser_msg_buffer);
	free_ptr(config_data);
}

/*
 * Parse domain config file settings.
 * On success, return @true.
 * On error, message and return @false.
 */
bool domain_info::parse_config_file_settings(XLU_Config* xlu_config,
					     const char* config_source,
					     const map_ss& kv)
{
	const char* cp;
	const char* key;
	bool done = false;
	long lv;

	/*
	 * Handle "membalance_mode" key
	 */
	key = "membalance_mode";
	if ((cp = k2v(kv, key)) != NULL)
	{
		/* if membalance_mode is invalid, bail out */
		CHECK(parse_control_mode(config_source, key, cp, &ctrl_modes_allowed));

		/* if membalance_mode is "off", bail out */
		CHECK(ctrl_modes_allowed != 0);
	}

	/* if no membalance keys, quit */
	CHECK(kv.size() != 0);

	/* if there are membalance keys, but control mode is not specified */
	if (cp == NULL)
	{
		error_msg("unable to parse %s: missing \"membalance_mode\" key",
			  config_source);
		goto cleanup;
	}

	/*
	 * if only one mode is enabled, set it as the current from the very
	 * start, otherwise leave the mode to be dynamically determined when
	 * domain sends thevery first message to membalanced
	 */
	if (ctrl_modes_allowed == CTRL_MODE_AUTO ||
	    ctrl_modes_allowed == CTRL_MODE_DIRECT)
	{
		ctrl_mode = ctrl_modes_allowed;
	}
	else
	{
		ctrl_mode = 0;
	}

	/*
	 * Handle "memory" and "maxmem" keys
	 */
	if (!xlu_cfg_get_long(xlu_config, "memory", &lv, 0))
	{
		if (lv > LONG_MAX / 1024 || lv < 0)
		{
			inval(config_source, "memory");
			goto cleanup;
		}
		xc_memory = lv * 1024;
		xc_maxmem = lv * 1024;
	}
	else
	{
		error_msg("unable to parse %s: missing \"memory\" key", config_source);
		goto cleanup;
	}


	if (!xlu_cfg_get_long(xlu_config, "maxmem", &lv, 0))
	{
		if (lv > LONG_MAX / 1024 || lv < 0)
		{
			inval(config_source, "maxmem");
			goto cleanup;
		}
		xc_maxmem = lv * 1024;
	}

	/*
	 * Handle the rest of "membalance_xxx" keys
	 */
	key = "membalance_dmem_max";
	if ((cp = k2v(kv, key)) != NULL)
		CHECK(parse_kb(config_source, key, cp, "mb", &xc_dmem_max));

	key = "membalance_dmem_min";
	if ((cp = k2v(kv, key)) != NULL)
		CHECK(parse_kb(config_source, key, cp, "mb", &xc_dmem_min));

	key = "membalance_dmem_quota";
	if ((cp = k2v(kv, key)) != NULL)
		CHECK(parse_kb(config_source, key, cp, "mb", &xc_dmem_quota));

	key = "membalance_rate_high";
	if ((cp = k2v(kv, key)) != NULL)
		CHECK(parse_kb_sec(config_source, key, cp, "kb/s", &xc_rate_high));

	key = "membalance_rate_low";
	if ((cp = k2v(kv, key)) != NULL)
		CHECK(parse_kb_sec(config_source, key, cp, "kb/s", &xc_rate_low));

	key = "membalance_rate_zero";
	if ((cp = k2v(kv, key)) != NULL)
		CHECK(parse_kb_sec(config_source, key, cp, "kb/s", &xc_rate_zero));

	key = "membalance_dmem_incr";
	if ((cp = k2v(kv, key)) != NULL)
		CHECK(parse_pct(config_source, key, cp, &xc_dmem_incr,
				config.min_dmem_incr, config.max_dmem_incr));

	key = "membalance_dmem_decr";
	if ((cp = k2v(kv, key)) != NULL)
		CHECK(parse_pct(config_source, key, cp, &xc_dmem_decr,
				config.min_dmem_decr, config.max_dmem_decr));

	key = "membalance_guest_free_threshold";
	if ((cp = k2v(kv, key)) != NULL)
		CHECK(parse_pct(config_source, key, cp, &xc_guest_free_threshold, 0.0, 1.0));

	key = "membalance_startup_time";
	if ((cp = k2v(kv, key)) != NULL)
		CHECK(parse_sec(config_source, key, cp, &xc_startup_time));

	key = "membalance_trim_unresponsive";
	if ((cp = k2v(kv, key)) != NULL)
		CHECK(parse_sec(config_source, key, cp, &xc_trim_unresponsive));

	key = "membalance_trim_unmanaged";
	if ((cp = k2v(kv, key)) != NULL)
		CHECK(parse_bool(config_source, key, cp, &xc_trim_unmanaged));

	done = true;

cleanup:

	return done;
}

/*
 * Insert @config[key], if present, into @kv[key]
 */
static const char* fetch_key(map_ss* kv, XLU_Config* xlu_config, const char* key)
{
	const char* cp;
	if (0 == xlu_cfg_get_string(xlu_config, key, &cp, 0))
	{
		std::string ckey(key);
		(*kv)[ckey] = std::string(cp);
		return kv->find(ckey)->second.c_str();
	}
	else
	{
		return NULL;
	}
}

/*
 * Resolve domain settings by merging domain configuration file
 * and global defaults. Validate the resolved set of settings.
 *
 * Resolution chain to determine active domain settings is as follows:
 *
 *     1. xenstore settings (when applicable)
 *     2. domain config file
 *     3. membalance.conf settings (when applicable)
 *     4. hardwired defaults (when applicable)
 *
 * Returns:
 *     @true   if settings are complete, valid and consistent
 *     @false  if settings are incomplete, invalid or incoherent,
 *             an error message is also issued in this case
 */
bool domain_info::resolve_settings(void)
{
	/*
	 * IMPORTANT: This routine should be kept in sync with
	 *            resolve_settings_affected(...) below.
	 */

	bool valid = true;
	std::string msg;
	char buf[256];
	bool automode = 0 != (ctrl_modes_allowed & CTRL_MODE_AUTO);

	/* dmem_min */
	if (xc_dmem_min >= 0)
		dmem_min = xc_dmem_min;
	else if (xc_memory >= 0)
		dmem_min = xc_memory;
	else
		{ undefined_setting("dmem_min"); valid = false; }

	/* dmem_max */
	if (xc_dmem_max >= 0)
		dmem_max = xc_dmem_max;
	else if (xs_mem_max >= 0)
		dmem_max = xs_mem_max;
	else if (xc_memory >= 0)
		dmem_max = xc_memory;
	else
		{ undefined_setting("dmem_max"); valid = false; }

	if (automode)
	{
		/* dmem_quota */
		if (xc_dmem_quota >= 0)
			dmem_quota = xc_dmem_quota;
		else if (xc_memory >= 0)
			dmem_quota = xc_memory;
		else
			{ undefined_setting("dmem_quota"); valid = false; }

		/* dmem_incr */
		if (xc_dmem_incr >= 0)
			dmem_incr = xc_dmem_incr;
		else if (config.isval_dmem_incr())
			dmem_incr = config.dmem_incr;
		else
			{ undefined_setting("dmem_incr"); valid = false; }

		/* dmem_decr */
		if (xc_dmem_decr >= 0)
			dmem_decr = xc_dmem_decr;
		else if (config.isval_dmem_decr())
			dmem_decr = config.dmem_decr;
		else
			{ undefined_setting("dmem_decr"); valid = false; }

		/* rate_high */
		if (xc_rate_high >= 0)
			rate_high = xc_rate_high;
		else if (config.isval_rate_high())
			rate_high = config.rate_high;
		else
			{ undefined_setting("rate_high"); valid = false; }

		/* rate_low */
		if (xc_rate_low >= 0)
			rate_low = xc_rate_low;
		else if (config.isval_rate_low())
			rate_low = config.rate_low;
		else
			{ undefined_setting("rate_low"); valid = false; }

		/* rate_zero */
		if (xc_rate_zero >= 0)
			rate_zero = xc_rate_zero;
		else if (config.isval_rate_zero())
			rate_zero = config.rate_zero;
		else
			{ undefined_setting("rate_zero"); valid = false; }

		/* guest_free_threshold */
		if (xc_guest_free_threshold >= 0)
			guest_free_threshold = xc_guest_free_threshold;
		else if (config.isval_guest_free_threshold())
			guest_free_threshold = config.guest_free_threshold;
		else
			{ undefined_setting("guest_free_threshold"); valid = false; }

		/* startup_time */
		if (xc_startup_time >= 0)
			startup_time = xc_startup_time;
		else if (config.isval_startup_time())
			startup_time = config.startup_time;

		/* trim_unresponsive */
		if (xc_trim_unresponsive >= 0)
			trim_unresponsive = xc_trim_unresponsive;
		else if (config.isval_trim_unresponsive())
			trim_unresponsive = config.trim_unresponsive;

		/* trim_unmanaged */
		if (xc_trim_unmanaged != TriMaybe)
			trim_unmanaged = (bool) xc_trim_unmanaged;
		else if (config.isval_trim_unmanaged())
			trim_unmanaged = config.trim_unmanaged;
	}

	CHECK(valid);

	/*
	 * round up memory sizes to Xen allocation quant size
	 */
	if (dmem_max >= 0)
		dmem_max = roundup(dmem_max, memquant_kbs);
	if (dmem_min >= 0)
		dmem_min = roundup(dmem_min, memquant_kbs);
	if (dmem_quota >= 0)
		dmem_quota = roundup(dmem_quota, memquant_kbs);

	if (automode && !(rate_low < rate_high))
	{
		if (msg.length() != 0)
			msg += ", ";
		sprintf(buf, "rate_low (%ld) < rate_high (%ld)", rate_low, rate_high);
		msg += buf;
		valid = false;
	}

	if (automode && !(dmem_min <= dmem_quota))
	{
		if (msg.length() != 0)
			msg += ", ";
		sprintf(buf, "dmem_min (%ld) <= dmem_quota (%ld)", dmem_min, dmem_quota);
		msg += buf;
		valid = false;
	}

	if (automode && !(dmem_quota <= dmem_max))
	{
		if (msg.length() != 0)
			msg += ", ";
		sprintf(buf, "dmem_quota (%ld) <= dmem_max (%ld)", dmem_quota, dmem_max);
		msg += buf;
		valid = false;
	}

	if (!automode && !(dmem_min <= dmem_max))
	{
		if (msg.length() != 0)
			msg += ", ";
		sprintf(buf, "dmem_min (%ld) <= dmem_max (%ld)", dmem_min, dmem_max);
		msg += buf;
		valid = false;
	}

	if (!(dmem_max <= xs_mem_max))
	{
		if (msg.length() != 0)
			msg += ", ";
		sprintf(buf, "dmem_max (rounded up to page size: %ld) <= maxmem (%ld)",
			dmem_max, xs_mem_max);
		msg += buf;
		valid = false;
	}

	if (xs_mem_videoram > 0 && (xs_mem_videoram % memquant_kbs) != 0)
	{
		if (msg.length() != 0)
			msg += ", ";
		sprintf(buf, "videoram (%ld) is multiple of page size (%ldK)",
			     xs_mem_videoram, memquant_kbs);
		msg += buf;
		valid = false;
	}

	if (!valid)
	{
		error_msg("parameter values are incoherent for domain %s, unfulfilled: %s",
			  printable_name(), msg.c_str());
		goto cleanup;
	}

	if (dmem_min == dmem_max)
	{
		warning_msg("domain %s cannot be managed by membalance because "
			    "dmem_min == dmem_max (%ld)",
			    printable_name(), dmem_min);
		goto cleanup;
	}

	return true;

cleanup:

	/*
	 * IMPORTANT: This routine should be kept in sync with
	 *            resolve_settings_affected(...) below.
	 */

	return false;
}

/*
 * Called when dmain configuration has been reloaded.
 *
 * Configuration change may potentially affect those domains that previously
 * were not manageable under old settings due to failing resolve_settings(...),
 * but may be manageable under new settings. Return @true new config differs
 * from @old config in a way that justifies trying to re-examine unmanaged
 * domains. Return @false if the change may not affect the outcome of
 * resolve_settings(...).
 *
 * This function control only whether unmanaged domains will be re-examined.
 * Its return value does not have an effect on managed domains, which will
 * have resolve_settings(...) called for them anyway.
 */
bool domain_info::resolve_settings_affected(const membalance_config& old)
{
	/* undefined -> defined */
	if ((!old.isval_dmem_incr() && config.isval_dmem_incr()) ||
	    (!old.isval_dmem_decr() && config.isval_dmem_decr()) ||
	    (!old.isval_rate_high() && config.isval_rate_high()) ||
	    (!old.isval_rate_low() && config.isval_rate_low()) ||
	    (!old.isval_rate_zero() && config.isval_rate_zero()) ||
	    (!old.isval_guest_free_threshold() && config.isval_guest_free_threshold()))
	{
		return true;
	}

	if (old.isval_rate_high() && config.isval_rate_high() &&
	    old.rate_high != config.rate_high)
	{
		return true;
	}

	if (old.isval_rate_low() && config.isval_rate_low() &&
	    old.rate_low != config.rate_low)
	{
		return true;
	}

	return false;
}

void domain_info::undefined_setting(const char* key)
{
	error_msg("parameter %s undefined for domain %s", key, printable_name());
}

/*
 * Print message
 */
static void inval(const char* config_source, const char* key)
{
	error_msg("invalid value for \"%s\" in %s", key, config_source);
}


/******************************************************************************
*                    handle xenstore events for a domian                      *
******************************************************************************/

/*
 * Handle xenstore watch events for domain identified by @domain_id.
 * @subpath is a key under /local/domain/<domid>/subpath.
 * If @subpath is an empty string, this is the top-level domid key itself.
 */
void handle_xs_watch_event(long domain_id, const char* subpath)
{
	domain_info* dom;
	tribool rc;
	bool watched = false;
	bool resolve = false;

	/*
	 * Ignore messages for Dom0 if managing it is disabled
	 */
	if (domain_id == 0 && !config.dom0_mode)
		return;

	/*
	 * Check if domain is being destroyed or created
	 */
	if (*subpath == '\0')
	{
		switch (domain_alive(domain_id))
		{
		case TriTrue:
			/* process creation of new domains */
			if (!is_domain_known(domain_id))
				transition_new_pending(domain_id);
			break;

		case TriFalse:
			/* domain has been destroyed */
			transition_dead(domain_id);
			break;

		case TriMaybe:
			break;
		}

		return;
	}

	/*
	 * Update data for managed or pending domains
	 */
	if (contains(doms.managed, domain_id))
	{
		dom = doms.managed[domain_id];
	}
	else if (contains(doms.pending, domain_id))
	{
		dom = doms.pending[domain_id];
	}
	else if (contains(doms.unmanaged, domain_id))
	{
		/*
		 * Initiate rescanning for unmanaged domains if the change is
		 * to one of the keys that potentially could have been blocking
		 * successful resolve_settings(...) for this domain.
		 */
		if (streq(subpath, "memory/static-max") ||
		    streq(subpath, "memory/videoram"))
		{
			if (domain_alive(domain_id) == TriFalse)
				transition_unmanaged_dead(domain_id);
			else
				transition_unmanaged_pending(domain_id);
		}
		return;
	}
	else
	{
		return;
	}

	if (streq(subpath, "memory/static-max"))
	{
		rc = read_value_from_xs(dom, subpath, &dom->xs_mem_max, 0);
		resolve = watched = true;
	}
	else if (streq(subpath, "memory/target"))
	{
		rc = read_value_from_xs(dom, subpath, &dom->xs_mem_target, 0);
		watched = true;
	}
	else if (streq(subpath, "memory/videoram"))
	{
		rc = read_value_from_xs(dom, subpath, &dom->xs_mem_videoram, -1);
		resolve = watched = true;
	}
	else if (streq(subpath, "name"))
	{
		rc = read_value_from_xs(dom, subpath, &dom->vm_name);
		watched = true;
	}

	/*
	 * If subkey has been deleted or in case or bad read error transition
	 * domain to unmanaged. A subkey is likely to be deleted in the process
	 * of domain destruction, in which case domain will soon be transitioned
	 * to dead (when deletion notification for domain root key is received).
	 */
	if (watched && rc != TriTrue)
	{
		if (contains(doms.managed, domain_id))
			transition_managed_unmanaged(domain_id);
		else if (contains(doms.pending, domain_id))
			transition_pending_unmanaged(domain_id);
	}

	/*
	 * If settings have changed for managed domain, and these settings
	 * are of the kind that affect resolve_settings(), execute resolution.
	 * If it fails (because settings are inconsistent), transition the
	 * domain to unmanaged.
	 */
	if (resolve && rc == TriTrue && contains(doms.managed, domain_id))
	{
		if (!dom->resolve_settings())
			transition_managed_unmanaged(domain_id);
	}
}


/******************************************************************************
*                              display info                                   *
******************************************************************************/

/*
 * This routine is called by RPC server to report the settings of a specified
 * domain to the management client
 */
int get_domain_settings(long domain_id, char** message, map_ss& kv)
{
	domain_info* dom = NULL;
	int res = 'X';
	char buf[64];
	char* p;

	/*
	 * check domain state
	 */
	if (contains(doms.managed, domain_id))
	{
		res = 'M';
		set_kv(kv, "state", "managed");
		dom = doms.managed[domain_id];
	}
	else if (contains(doms.unmanaged, domain_id))
	{
		res = 'U';
		set_kv(kv, "state", "unmanaged");
		goto cleanup;
	}
	else if (contains(doms.pending, domain_id))
	{
		res = 'P';
		set_kv(kv, "state", "pending");
		goto cleanup;
	}
	else
	{
		*message = xprintf("Domain %ld does not exist", domain_id);
		res = 'X';
		goto cleanup;
	}

	/*
	 * for managed domains, report their settings
	 */

	setfmt(kv, "domain_id", "%ld", dom->domain_id);

	if (dom->vm_name)
		set_kv(kv, "vm_name", dom->vm_name);

	if (dom->vm_uuid)
		set_kv(kv, "vm_uuid", dom->vm_uuid);

	p = buf;
	if (dom->ctrl_modes_allowed & CTRL_MODE_AUTO)    *p++ = 'A';
	if (dom->ctrl_modes_allowed & CTRL_MODE_DIRECT)  *p++ = 'D';
	*p = '\0';
	setfmt(kv, "ctrl_modes_allowed", "%s", buf);

	switch (dom->ctrl_mode)
	{
	case CTRL_MODE_AUTO:
		setfmt(kv, "ctrl_mode", "%s", "A");
		break;
	case CTRL_MODE_DIRECT:
		setfmt(kv, "ctrl_mode", "%s", "D");
		break;
	default:
		setfmt(kv, "ctrl_mode", "%s","-");
		break;
	}

	if (dom->xs_mem_max >= 0)
		setfmt(kv, "xs_mem_max", "%ld", dom->xs_mem_max);

	if (dom->xs_mem_target >= 0)
		setfmt(kv, "xs_mem_target", "%ld", dom->xs_mem_target);

	if (dom->xs_mem_videoram != XS_MEM_VIDEORAM_UNSET)
		setfmt(kv, "xs_mem_videoram", "%ld", dom->xs_mem_videoram);

	if (dom->dmem_max >= 0)
		setfmt(kv, "dmem_max", "%ld", dom->dmem_max);

	if (dom->dmem_quota >= 0)
		setfmt(kv, "dmem_quota", "%ld", dom->dmem_quota);

	if (dom->dmem_min >= 0)
		setfmt(kv, "dmem_min", "%ld", dom->dmem_min);

	if (dom->dmem_incr >= 0)
		setfmt(kv, "dmem_incr", "%g", dom->dmem_incr * 100.0);

	if (dom->dmem_decr >= 0)
		setfmt(kv, "dmem_decr", "%g", dom->dmem_decr * 100.0);

	if (dom->rate_high >= 0)
		setfmt(kv, "rate_high", "%ld", dom->rate_high);

	if (dom->rate_low >= 0)
		setfmt(kv, "rate_low", "%ld", dom->rate_low);

	if (dom->rate_zero >= 0)
		setfmt(kv, "rate_zero", "%ld", dom->rate_zero);

	if (dom->guest_free_threshold >= 0)
		setfmt(kv, "guest_free_threshold", "%g", dom->guest_free_threshold * 100.0);

	if (dom->startup_time >= 0)
		setfmt(kv, "startup_time", "%d", dom->startup_time);

	if (dom->trim_unresponsive >= 0)
		setfmt(kv, "trim_unresponsive", "%d", dom->trim_unresponsive);

	setfmt(kv, "trim_unmanaged", "%d", (int) dom->trim_unmanaged);

cleanup:

	/* XDR cannot marshal null string pointers */
	if (*message == NULL)
		*message = xstrdup("");

	return res;
}


/******************************************************************************
*                               debugging                                     *
******************************************************************************/

/*
 * Dump debugging information for all domains
 */
void show_domains(FILE* fp)
{
	show_domains(fp, "Managed domains", doms.managed, 'm');
	show_domains(fp, "Unmanaged domains", doms.unmanaged, 'u');
	show_domains(fp, "Pending domains", doms.pending, 'p');
}

/*
 * Dump debugging information for a domain set
 */
static void show_domains(FILE* fp, const char* title, const domid2info& xdoms, char kind)
{
	const char* offset = "    ";
	const char* offset2 = "        ";

	if (xdoms.size() == 0)
	{
		fprintf(fp, "%s: none\n\n", title);
		return;
	}

	fprintf(fp, "%s: (%lu total)\n\n", title, xdoms.size());

	for (domid2info::const_iterator it = xdoms.begin(); it != xdoms.end(); ++it)
	{
		long domain_id = it->first;
		domain_info* dom = it->second;

		/* id, name, uuid */
		fprintf(fp, "%sid: %ld", offset, domain_id);
		if (!dom)
		{
			/* membalanced does not keep data for unmanaged domains */
			fprintf(fp, "\n");
			continue;
		}

		if (dom->vm_name || dom->vm_uuid)
		{
			fprintf(fp, " (");
			if (dom->vm_name)
				fprintf(fp, "name: %s%s",
					dom->vm_name,
					dom->vm_uuid ? ", " : "");
			if (dom->vm_uuid)
				fprintf(fp, "uuid: %s", dom->vm_uuid);
			fprintf(fp, ")");
		}
		fprintf(fp, "\n");

		/*
		 * print active settings
		 */
		/* active (dmem_min, dmem_quota, dmem_max) */
		fprintf(fp, "%sactive:  dmem_min: %ld, dmem_quota: %ld, dmem_max: %ld\n",
			offset2, dom->dmem_min, dom->dmem_quota, dom->dmem_max);

		/* active (dmem_incr, dmem_decr, rate_high, rate_low) */
		fprintf(fp, "%s         dmem_incr: %g%%, dmem_decr: %g%%, rate_high: %ld, rate_low: %ld\n",
			offset2,
			dom->dmem_incr >= 0 ? dom->dmem_incr * 100.0 : -1,
			dom->dmem_decr >= 0 ? dom->dmem_decr * 100.0 : -1,
			dom->rate_high, dom->rate_low);

		/* active (rate_zero, guest_free_threshold) */
		fprintf(fp, "%s         rate_zero: %ld, guest_free_threshold: %g%%\n",
			offset2,
			dom->rate_zero,
			dom->guest_free_threshold >= 0 ? dom->guest_free_threshold * 100.0 : -1);

		/* active (startup_time, trim_unresponsive, trim_unmanaged) */
		fprintf(fp, "%s         startup_time: %d, trim_unresponsive: %d, trim_unmanaged: %d\n",
			offset2,
			dom->startup_time,
			dom->trim_unresponsive,
			dom->trim_unmanaged);

		/*
		 * print domain config file data
		 */
		if (dom->config_file_status == TriTrue)
		{
			/* dom cfg (memory, maxmem, dmem_max, dmem_quota, dmem_min) */
			fprintf(fp, "%sdom cfg: memory: %ld, maxmem: %ld\n",
				offset2, dom->xc_memory, dom->xc_maxmem);

			/* dom cfg (memory, maxmem, dmem_max, dmem_quota, dmem_min) */
			fprintf(fp, "%s         dmem_min: %ld, dmem_quota: %ld, dmem_max: %ld\n",
				offset2, dom->xc_dmem_min, dom->xc_dmem_quota, dom->xc_dmem_max);

			/* dom cfg (dmem_incr, dmem_decr, rate_high, rate_low) */
			fprintf(fp, "%s         dmem_incr: %g%%, dmem_decr: %g%%, "
				    "rate_high: %ld, rate_low: %ld\n",
				offset2,
				dom->xc_dmem_incr >= 0 ? dom->xc_dmem_incr * 100.0 : -1,
				dom->xc_dmem_decr >= 0 ? dom->xc_dmem_decr * 100.0 : -1,
				dom->xc_rate_high,
				dom->xc_rate_low);

			/* dom cfg (rate_zero, guest_free_threshold) */
			fprintf(fp, "%s         rate_zero: %ld, guest_free_threshold: %g%%\n",
				offset2,
				dom->xc_rate_zero,
				dom->xc_guest_free_threshold >= 0 ? dom->xc_guest_free_threshold * 100.0 : -1);

			/* dom cfg (startup_time, trim_unresponsive, trim_unmanaged) */
			fprintf(fp, "%s         startup_time: %d, trim_unresponsive: %d, trim_unmanaged: %d\n",
				offset2,
				dom->startup_time,
				dom->xc_trim_unresponsive,
				dom->xc_trim_unmanaged);
		}
		else if (dom->config_file_status == TriFalse)
		{
			fprintf(fp, "%sdom cfg: cannot fetch\n", offset2);
		}
		else if (dom->config_file_status == TriMaybe)
		{
			fprintf(fp, "%sdom cfg: still retrieving\n", offset2);
		}
		else
		{
			fprintf(fp, "%sdom cfg: weird\n", offset2);
		}

		/*
		 * print xenstore data: memory (max, target, videoram)
		 */
		fprintf(fp, "%sxenstore data: memory (max: %ld, target: %ld, videoram: %ld)\n",
			offset2, dom->xs_mem_max, dom->xs_mem_target, dom->xs_mem_videoram);

		fprintf(fp, "\n");
	}

	fprintf(fp, "\n");
}


/******************************************************************************
*                   server for membalancectl "list" command                   *
******************************************************************************/

/*
 * Show domain and memory status.
 * Invoked by membalancedctl via RPC.
 */
char* show_status(int verbosity)
{
	char* buffer = NULL;
	size_t buffer_size = 0;
	FILE* fp = NULL;
	long xen_free_memory;

	/* create in-memory file */
	fp = open_memstream(&buffer, &buffer_size);
	if (!fp)
	{
		error_perror("unable to create in-memory file");
		return xstrdup("Error: out of memory");
	}

	/* collect memory usage data */
	collect_domain_memory_info();
	xen_free_memory = get_xen_free_memory();
	xen_free_slack = get_xen_free_slack();

	/* display elements */
	show_status_header(fp);
	show_status_global(fp);
	show_status_memory(fp, xen_free_memory);
	show_managed_domains(fp);
	show_pending_domains(fp);
	show_unmanaged_domains(fp);

	/* shutdown memory file and extract its content */
	fflush(fp);
	fclose(fp);

	return buffer ? buffer : xstrdup("Error: out of memory");
}

/* display header */
static void show_status_header(FILE* fp)
{
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);

	char tms[64];
	strftime(tms, countof(tms), "%F %T", &tm);

	fprintf(fp, "Status of %s %s at %s\n", progname, progversion, tms);
	fprintf(fp, "\n");
}

/* display global setting */
static void show_status_global(FILE* fp)
{
	int w1, w2;

	fprintf(fp, "Domain adjustment is ");
	if (memsched_pause_level)
		fprintf(fp, "paused (depth %u)\n", memsched_pause_level);
	else
		fprintf(fp, "enabled\n");
	fprintf(fp, "Memory balancing interval: %d sec\n", config.interval);
	fprintf(fp, "\n");
	w1 = strlen(decode_memsize(config.host_reserved_hard));
	w2 = strlen(decode_memsize(config.host_reserved_soft));
	fprintf(fp, "host_reserved_hard:  %*s (MB.KB)\n",
		    max(w1, w2), decode_memsize(config.host_reserved_hard));
	fprintf(fp, "host_reserved_soft:  %*s (MB.KB)\n",
		    max(w1, w2), decode_memsize(config.host_reserved_soft));
	fprintf(fp, "\n");
}

/* display memory status */
static void show_status_memory(FILE* fp, long xen_free_memory)
{
	long free_kbs, free_hard, free_soft;
	const char* sign_kbs = "";
	const char* sign_hard = "";
	const char* sign_soft = "";
	const char* sign_minus = "(-) ";
	int width, w_kbs, w_soft, w_hard;

	free_kbs = xen_free_memory - xen_free_slack;

	free_hard = free_kbs - config.host_reserved_hard;
	if (free_hard < 0)
	{
		free_hard = -free_hard;
		sign_hard = sign_minus;
	}

	free_soft = free_kbs - config.host_reserved_soft;
	if (free_soft < 0)
	{
		free_soft = -free_soft;
		sign_soft = sign_minus;
	}

	if (free_kbs < 0)
	{
		free_kbs = -free_kbs;
		sign_kbs = sign_minus;
	}

	w_kbs = strlen(sign_kbs) + strlen(decode_memsize(free_kbs));
	w_soft = strlen(sign_soft) + strlen(decode_memsize(free_soft));
	w_hard = strlen(sign_hard) + strlen(decode_memsize(free_hard));

	width = max(w_kbs, max(w_hard, w_soft));
	w_kbs = width - w_kbs;
	w_soft = width - w_soft;
	w_hard = width - w_hard;

	std::string xen_free_slack_str = decode_memsize(xen_free_slack);
	fprintf(fp, "Free memory:   %*s%s%s (MB.KB) + Xen free memory slack of %s (MB.KB)\n",
		    w_kbs, "",
		    sign_kbs, decode_memsize(free_kbs),
		    xen_free_slack_str.c_str());
	fprintf(fp, "Over soft by:  %*s%s%s (MB.KB)\n",
		    w_soft, "",
		    sign_soft, decode_memsize(free_soft));
	fprintf(fp, "Over hard by:  %*s%s%s (MB.KB)\n",
		    w_hard, "",
		    sign_hard, decode_memsize(free_hard));

	fprintf(fp, "\n");
}

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

/*
 * Categories of domain size for expansion force:
 *
 *     C_LOW:    DMEM_SIZE <= DMEM_MIN
 *     C_MID:    RATE = ] DMEM_MIN ... DMEM_QUOTA ]
 *     C_HIGH:	 DMEM_SIZE > DMEM_QUOTA
 *
 */
inline static char size_category_code(domain_info* dom, long size)
{
	if (size > dom->dmem_quota)
		return 'H';
	else if (size <= dom->dmem_min)
		return 'L';
	else
		return ' ';
}

/* display managed domains */
static void show_managed_domains(FILE* fp)
{
	std::vector<long> vec;
	populate_sorted_by_id(vec, doms.managed);

	if (vec.size() == 0)
	{
		fprintf(fp, "Managed domains: none\n\n");
		return;
	}

	fprintf(fp, "Managed domains:\n\n");
	fprintf(fp, "                                                      size         rate       rate  trend\n");
	fprintf(fp, "    ID                    name                      (MB.KB)     (MB.KB/sec)   (MB.KB/sec)\n");
	fprintf(fp, "  ----- ---------------------------------------- ------------- ------------- -------------\n");

	for (unsigned k = 0;  k < vec.size();  k++)
	{
		domain_info* dom = doms.managed[vec[k]];

		fprintf(fp, "  %5ld %-40s ",
			    dom->domain_id,
			    dom->vm_name ? dom->vm_name : "");

		if (dom->valid_memory_data)
		{
			fprintf(fp, "%11s %c ",
				    decode_memsize(dom->memsize),
				    size_category_code(dom, dom->memsize));
		}
		else
		{
			fprintf(fp, "%11s %c ",
				    "", ' ');
		}

		if (dom->valid_data)
		{
			fprintf(fp, "%11s %c ",
				    decode_memsize(dom->rate),
				    rate_category_code(dom, dom->rate));
			fprintf(fp, "%11s %c",
				    decode_memsize(dom->slow_rate),
				    rate_category_code(dom, dom->slow_rate));
		}

		fprintf(fp, "\n");
	}
	fprintf(fp, "\n");
}

/* display pending domains */
static void show_pending_domains(FILE* fp)
{
	std::vector<long> vec;
	populate_sorted_by_id(vec, doms.pending);

	if (vec.size() == 0)
	{
		fprintf(fp, "Pending domains: none\n\n");
		return;
	}

	fprintf(fp, "Pending domains:\n\n");
	fprintf(fp, "    ID                    name\n");
	fprintf(fp, "  ----- ----------------------------------------\n");

	for (unsigned k = 0;  k < vec.size();  k++)
	{
		domain_info* dom = doms.pending[vec[k]];
		fprintf(fp, "  %5ld %-40s\n",
			    dom->domain_id,
			    dom->vm_name ? dom->vm_name : "");
	}

	fprintf(fp, "\n");
}

/* display unmanaged domains */
static void show_unmanaged_domains(FILE* fp)
{
	std::vector<long> vec;
	populate_sorted_by_id(vec, doms.unmanaged);

	if (vec.size() == 0)
	{
		fprintf(fp, "Unmanaged domains: none\n");
		return;
	}

	if (vec.size() == 1 && vec[0] == 0)
	{
		fprintf(fp, "Unmanaged domains: only Dom0\n");
		return;
	}

	fprintf(fp, "Unmanaged domains:\n\n");
	fprintf(fp, "    ID\n");
	fprintf(fp, "  -----\n");

	for (unsigned k = 0;  k < vec.size();  k++)
		fprintf(fp, "  %5ld\n", vec[k]);
}

/*
 * Fill in @vec with a list of domain_id's of domains held in @dmap,
 * returning @vec elements sorted in the order of increaing domain_id.
 */
static void populate_sorted_by_id(std::vector<long>& vec, const domid2info& dmap)
{
	vec.clear();
	for (domid2info::const_iterator it = dmap.begin();  it != dmap.end();   ++it)
		vec.push_back(it->first);
	std::sort(vec.begin(), vec.end());
}

/*
 * Format memory size as MB.KB
 */
const char* decode_memsize(long kbs)
{
	static char buf[64];
	if (kbs < 1024)
		sprintf(buf, "%ld", kbs);
	else
		sprintf(buf, "%ld.%04ld", kbs / 1024, kbs % 1024);
	return buf;
}


/******************************************************************************
*                  recalc the size of domain's Xen pivate data                 *
******************************************************************************/

/*
 * The size of Xen's private data area for a domain can change with time.
 * We try to track the changes and store new value in dom->xen_data_size.
 * See above in "Fine details of domain size calculus".
 */
void domain_info::reeval_xen_data_size(long xen_free)
{
	if (!is_runnable(xcinfo))
		return;

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
	if (xen_free < (long) pagesize_kbs * 100)
		return;

	long totsize0 = pagesize_kbs * xcinfo->tot_pages;

	if (xds_phase == 0 ||
	    xds_totsize0 != totsize0 ||
	    xds_memgoal0 != memgoal0 ||
	    totsize0 < memgoal0)
	{
		xds_totsize0 = totsize0;
		xds_memgoal0 = memgoal0;
		xds_phase = 1;
		return;
	}

	if (++xds_phase >= config.xen_private_data_size_samples)
	{
		xen_data_size = totsize0 - memgoal0;
		--xds_phase;
	}
}
