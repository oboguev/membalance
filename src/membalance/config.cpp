/*
 *  MEMBALANCE daemon
 *
 *  config.cpp - manage configuration
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

#include "membalanced.h"


/******************************************************************************
*                               static data                                   *
******************************************************************************/

static const char* membalanced_conf_path = "/etc/membalance.conf";


/******************************************************************************
*                           forward declarations                              *
******************************************************************************/

static bool convert_unit_second(const char* dbname, const char* key,
				int* iv, const char* unit);
static bool convert_unit_second(cfg_database_t cfg, const char* key,
				int* iv, const char* unit);

static bool convert_unit_kb(const char* dbname, const char* key,
			    unsigned long* ulv, const char* unit);
static bool convert_unit_kb(cfg_database_t cfg, const char* key,
			    unsigned long* ulv, const char* unit);

static bool convert_unit_kb(const char* dbname, const char* key,
			    long* lv, const char* unit);
static bool convert_unit_kb(cfg_database_t cfg, const char* key,
			    long* lv, const char* unit);

static bool convert_unit_kb_sec(const char* dbname, const char* key,
			        unsigned long* ulv, const char* unit);
static bool convert_unit_kb_sec(cfg_database_t cfg, const char* key,
			        unsigned long* ulv, const char* unit);

static bool convert_unit_kb_sec(const char* dbname, const char* key,
			        long* lv, const char* unit);
static bool convert_unit_kb_sec(cfg_database_t cfg, const char* key,
			        long* lv, const char* unit);

static void inval(const char* dbname, const char* key);
static void inval(cfg_database_t cfg, const char* key);

static void config_eval_reserved_soft(membalance_config& xconfig, const char* cname);

/******************************************************************************
*                          (re)load configuration                             *
******************************************************************************/

static const char* units_mem[] =
{
	"k", "kb",
	"m", "mb",
	"g", "gb",
	NULL
};

static const char* units_time[] =
{
	"s", "sec", "secs", "second", "seconds",
	"m", "min", "mins", "minute", "minutes",
	"h", "hr", "hrs", "hour", "hours",
	NULL
};

static const char* units_rate[] =
{
	"kb/s", "kb/sec", "kbs",
	"mb/s", "mb/sec", "mbs",
	"gb/s", "gb/sec", "gbs",
	NULL
};

static const char* units_percent[] =
{
	"%",
	NULL
};

/*
 * Called to either initially load (@reload = false)
 * or to reload (@reload = true) membalanced configuration.
 */
void load_configuration(bool reload)
{
	int fd = -1;
	cfg_database_t cfg;
	membalance_config xconfig;
	std::set<std::string> vkeys;	/* lists valid keys */
	const char* unit;
	const char* key;
	const char* sv;
	double dfv;
	bool bv;
	int iv;
	unsigned long ulv;
	long lv;

	/* descriptive name of configuration source (for messages) */
	std::string cname_buf("configuration file ");
	cname_buf += membalanced_conf_path;
	const char* cname = cname_buf.c_str();

	debug_msg(1, reload ? "reloading configuration"
		            : "loading configuration");

	/*
	 * Read and parse configuration file
	 */
	fd = open(membalanced_conf_path, O_RDONLY);
	if (fd < 0)
	{
		if (errno == ENOENT || errno == ENOTDIR)
			warning_msg("unable to read %s", cname);
		else if (reload)
			error_perror("unable to read %s", cname);
		else
			fatal_perror("unable to read %s", cname);
		return;
	}

	cfg = cfg_parse(cname, fd);
	close(fd);
	if (!cfg)
		return;

	/*
	 * Fetch configuration values
	 */
	xconfig.defaults();

	key = "interval";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_int_units(cfg, key, units_time, "sec", &iv, &unit) &&
	    convert_unit_second(cfg, key, &iv, unit))
	{
		if (iv <= 0)
		{
			inval(cname, key);
		}
		else if (iv < config.min_interval)
		{
			warning_msg("value of \"%s\" in %s is too low, setting to %d",
				    key, cname, config.min_interval);
			xconfig.set_interval(config.min_interval);
		}
		else if (iv > config.max_interval)
		{
			warning_msg("value of \"%s\" in %s is too high, clamped down to %d",
				    key, cname, config.max_interval);
			xconfig.set_interval(config.max_interval);
		}
		else
		{
			xconfig.set_interval(iv);
		}
	}

	key = "max_xs_retries";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_int(cfg, key, &iv))
	{
		if (iv < 0)
			inval(cname, key);
		else
			xconfig.set_max_xs_retries(iv);
	}

	key = "max_xen_init_retries";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_int_units(cfg, key, units_time, "sec", &iv, &unit) &&
	    convert_unit_second(cfg, key, &iv, unit))
	{
		if (iv < 0)
			inval(cname, key);
		else
			xconfig.set_max_xen_init_retries(iv);
	}

	key = "xen_init_retry_msg";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_int_units(cfg, key, units_time, "sec", &iv, &unit) &&
	    convert_unit_second(cfg, key, &iv, unit))
	{
		if (iv < 0)
			inval(cname, key);
		else
			xconfig.set_xen_init_retry_msg(iv);
	}

	key = "domain_pending_timeout";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_int_units(cfg, key, units_time, "sec", &iv, &unit) &&
	    convert_unit_second(cfg, key, &iv, unit))
	{
		if (iv < 0)
			inval(cname, key);
		else
			xconfig.set_domain_pending_timeout(iv);
	}

	key = "host_reserved_hard";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_long_units(cfg, key, units_mem, "mb", &lv, &unit) &&
	    convert_unit_kb(cfg, key, &lv, unit))
	{
		if (lv < 0)
			inval(cname, key);
		else
			xconfig.set_host_reserved_hard(lv);
	}

	key = "host_reserved_soft";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_long_units(cfg, key, units_mem, "mb", &lv, &unit) &&
	    convert_unit_kb(cfg, key, &lv, unit))
	{
		if (lv < 0)
			inval(cname, key);
		else
			xconfig.set_host_reserved_soft(lv);
	}

	key = "rate_high";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_ulong_units(cfg, key, units_rate, "kb/s", &ulv, &unit) &&
	    convert_unit_kb_sec(cfg, key, &ulv, unit))
	{
		xconfig.set_rate_high(ulv);
	}

	key = "rate_low";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_ulong_units(cfg, key, units_rate, "kb/s", &ulv, &unit) &&
	    convert_unit_kb_sec(cfg, key, &ulv, unit))
	{
		xconfig.set_rate_low(ulv);
	}

	key = "rate_zero";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_ulong_units(cfg, key, units_rate, "kb/s", &ulv, &unit) &&
	    convert_unit_kb_sec(cfg, key, &ulv, unit))
	{
		xconfig.set_rate_zero(ulv);
	}

	key = "dmem_incr";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_float_units(cfg, key, units_percent, "%", &dfv, &unit))
	{
		dfv /= 100.0;
		if (dfv >= config.min_dmem_incr && dfv <=config.max_dmem_incr)
			xconfig.set_dmem_incr(dfv);
		else
			inval(cname, key);
	}

	key = "dmem_decr";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_float_units(cfg, key, units_percent, "%", &dfv, &unit))
	{
		dfv /= 100.0;
		if (dfv >= config.min_dmem_decr && dfv <= config.max_dmem_decr)
			xconfig.set_dmem_decr(dfv);
		else
			inval(cname, key);
	}

	key = "guest_free_threshold";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_float_units(cfg, key, units_percent, "%", &dfv, &unit))
	{
		dfv /= 100.0;
		if (dfv >= 0 && dfv <= 1.0)
			xconfig.set_guest_free_threshold(dfv);
		else
			inval(cname, key);
	}

	key = "startup_time";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_int_units(cfg, key, units_time, "sec", &iv, &unit) &&
	    convert_unit_second(cfg, key, &iv, unit))
	{
		if (iv < 0)
		{
			inval(cname, key);
		}
		else
		{
			xconfig.set_startup_time(iv);
		}
	}

	key = "trim_unresponsive";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_int_units(cfg, key, units_time, "sec", &iv, &unit) &&
	    convert_unit_second(cfg, key, &iv, unit))
	{
		if (iv < 0)
		{
			inval(cname, key);
		}
		else
		{
			xconfig.set_trim_unresponsive(iv);
		}
	}

	key = "trim_unmanaged";
	vkeys.insert(std::string(key));
	if (TriTrue == cfg_get_bool(cfg, key, &bv))
	{
		xconfig.set_trim_unmanaged(bv);
	}

	key = "dom0_membalance_mode";
	vkeys.insert(std::string(key));
	if (cfg_get_string(cfg, key, &sv))
	{
		unsigned int ctrl_modes_allowed = 0;
		if (parse_control_mode(cfg_dbname(cfg), "dom0_membalance_mode",
				       sv, &ctrl_modes_allowed))
		{
			xconfig.set_dom0_mode(ctrl_modes_allowed);
		}
	}

	/* check for misspelled keys and issue a message */
	cfg_validate_keys(cfg, vkeys);
	cfg_destroy(cfg);

	/* apply defaults */
	xconfig.merge(default_config);

	/* consistency check */
	if (xconfig.isval_rate_low() &&
	    xconfig.isval_rate_high() &&
	    xconfig.rate_low >= xconfig.rate_high)
	{
		warning_msg("%s: rate_low (%lu KB/s) >= rate_high (%lu KB/s)",
			    cname, xconfig.rate_low, xconfig.rate_high);
	}

	if (!xconfig.isset_host_reserved_hard())
		xconfig.set_host_reserved_hard(0);

	config_eval_reserved_soft(xconfig, cname);

	config = xconfig;
}

/*
 * Called after XL completed initialization
 */
void config_eval_after_xl_init(void)
{
	/* descriptive name of configuration source (for messages) */
	std::string cname_buf("configuration file ");
	cname_buf += membalanced_conf_path;
	const char* cname = cname_buf.c_str();

	config_eval_reserved_soft(config, cname);
}

static void config_eval_reserved_soft(membalance_config& xconfig, const char* cname)
{
	bool have_soft = false;

	if (xconfig.isset_host_reserved_soft())
	{
		have_soft = true;
	}
	else if (xl_ctx != NULL)
	{
		long delta = get_xen_physical_memory() -
			     get_xen_free_slack() -
			     get_xen_dom0_minsize();
		delta = max(0, delta);
		delta = (long) (delta * 0.1);
		xconfig.host_reserved_soft = xconfig.host_reserved_hard + delta;
		xconfig.host_reserved_soft = roundup(xconfig.host_reserved_soft, memquant_kbs);
		have_soft = true;
	}

	if (xconfig.isset_host_reserved_hard() &&
	    have_soft &&
	    xconfig.host_reserved_soft < xconfig.host_reserved_hard)
	{
		warning_msg("%s: host_reserved_soft (%ld KB) < host_reserved_hard (%ld KB)",
			    cname, xconfig.host_reserved_soft, xconfig.host_reserved_hard);
		warning_msg("increasing active value of host_reserved_soft to host_reserved_hard (%ld KB)",
			    xconfig.host_reserved_hard);
		xconfig.host_reserved_soft = xconfig.host_reserved_hard;
	}
}

/*
 * Called to reload membalanced configuration
 */
void reload_configuration(void)
{
	membalance_config sv = config;

	load_configuration(true);

	/*
	 * update interval, unless an update is already scheduled anyway
	 */
	if (config.interval != sv.interval &&
	    !update_interval_in_xs)
	{
		update_interval_in_xs = true;
		update_membalance_interval();
	}

	/*
	* daemon config parameters affecting domain_info::resolve_settings(...)
	* etc. may have changed, making some currently unmanaged domains
	* manageable and vice versa
	*
	* rescan both managed and unmanaged domains
	 */
	rescan_domains_on_config_change(sv);
}


/******************************************************************************
*                              unit converters                                *
******************************************************************************/

/*
 * seconds - int
 */
static bool convert_unit_second(cfg_database_t cfg, const char* key,
				int* iv, const char* unit)
{
	return convert_unit_second(cfg_dbname(cfg), key, iv, unit);
}

static bool convert_unit_second(const char* dbname, const char* key,
				int* iv, const char* unit)
{
	if (*iv < 0)
	{
		inval(dbname, key);
		return false;
	}

	if (tolower(*unit) == 's')
	{
		return true;
	}
	else if (tolower(*unit) == 'm')
	{
		long lv = *iv;
		lv *= 60;
		if (lv < INT_MIN || lv > INT_MAX)
		{
			inval(dbname, key);
			return false;
		}
		else
		{
			*iv = (int) lv;
			return true;
		}
	}
	else if (tolower(*unit) == 'h')
	{
		long lv = *iv;
		lv *= 3600;
		if (lv < INT_MIN || lv > INT_MAX)
		{
			inval(dbname, key);
			return false;
		}
		else
		{
			*iv = (int) lv;
			return true;
		}
	}
	else
	{
		return false;
	}
}

/*
 * KB - ulong
 */
static bool convert_unit_kb(cfg_database_t cfg, const char* key,
			    unsigned long* ulv, const char* unit)
{
	return convert_unit_kb(cfg_dbname(cfg), key, ulv, unit);
}

static bool convert_unit_kb(const char* dbname, const char* key,
			    unsigned long* ulv, const char* unit)
{
	if (tolower(*unit) == 'k')
	{
		return true;
	}
	else if (tolower(*unit) == 'm')
	{
		if (*ulv > ULONG_MAX / 1024)
		{
			inval(dbname, key);
			return false;
		}
		else
		{
			*ulv *= 1024;
			return true;
		}
	}
	else if (tolower(*unit) == 'g')
	{
		if (*ulv > ULONG_MAX / (1024 * 1024))
		{
			inval(dbname, key);
			return false;
		}
		else
		{
			*ulv *= 1024 * 1024;
			return true;
		}
	}
	else
	{
		return false;
	}
}

/*
 * KB - long
 */
static bool convert_unit_kb(cfg_database_t cfg, const char* key,
			    long* lv, const char* unit)
{
	return convert_unit_kb(cfg_dbname(cfg), key, lv, unit);
}

static bool convert_unit_kb(const char* dbname, const char* key,
			    long* lv, const char* unit)
{
	if (*lv < 0)
	{
		inval(dbname, key);
		return false;
	}

	if (tolower(*unit) == 'k')
	{
		return true;
	}
	else if (tolower(*unit) == 'm')
	{
		if (*lv > LONG_MAX / 1024)
		{
			inval(dbname, key);
			return false;
		}
		else
		{
			*lv *= 1024;
			return true;
		}
	}
	else if (tolower(*unit) == 'g')
	{
		if (*lv > LONG_MAX / (1024 * 1024))
		{
			inval(dbname, key);
			return false;
		}
		else
		{
			*lv *= 1024 * 1024;
			return true;
		}
	}
	else
	{
		return false;
	}
}

/*
 * KB/sec - ulong
 */
static bool convert_unit_kb_sec(cfg_database_t cfg, const char* key,
			        unsigned long* ulv, const char* unit)
{
	return convert_unit_kb_sec(cfg_dbname(cfg), key, ulv, unit);
}

static bool convert_unit_kb_sec(const char* dbname, const char* key,
			        unsigned long* ulv, const char* unit)
{
	return convert_unit_kb(dbname, key, ulv, unit);
}

/*
 * KB/sec - long
 */
static bool convert_unit_kb_sec(cfg_database_t cfg, const char* key,
			        long* lv, const char* unit)
{
	return convert_unit_kb_sec(cfg_dbname(cfg), key, lv, unit);
}

static bool convert_unit_kb_sec(const char* dbname, const char* key,
			        long* lv, const char* unit)
{
	return convert_unit_kb(dbname, key, lv, unit);
}


/******************************************************************************
*                            parse external strings                           *
******************************************************************************/

/*
 * Parse memory size with specified units as the number of KB's.
 *
 * Parameters:
 *         @dbname = data source descriptive name (for error messaging)
 *         @key = data item key name (for error messaging)
 *         @value = string value of the item
 *         @default_unit = default unit to use, e.g. "kb"
 *         @pvalue = where to return the value
 *
 * Return: @true if successful.
 *         @false on error, and issue a message.
 */
bool parse_kb(const char* dbname, const char* key, const char* svalue,
	      const char* default_unit, long* pvalue)
{
	const char* unit;
	return cfg_get_long_units(dbname, key, svalue, units_mem,
				  default_unit, pvalue, &unit) &&
	       convert_unit_kb(dbname, key, pvalue, unit);
}

/*
 * Parse data rate with specified units as the number of KB/sec.
 *
 * Parameters:
 *         @dbname = data source descriptive name (for error messaging)
 *         @key = data item key name (for error messaging)
 *         @value = string value of the item
 *         @default_unit = default unit to use, e.g. "kb"
 *         @pvalue = where to return the value
 *
 * Return: @true if successful.
 *         @false on error, and issue a message.
 */
bool parse_kb_sec(const char* dbname, const char* key, const char* svalue,
		  const char* default_unit, long* pvalue)
{
	const char* unit;
	return cfg_get_long_units(dbname, key, svalue, units_rate,
				  default_unit, pvalue, &unit) &&
	       convert_unit_kb_sec(dbname, key, pvalue, unit);
}

/*
 * Parse percentage.
 *
 * Parameters:
 *         @dbname = data source descriptive name (for error messaging)
 *         @key = data item key name (for error messaging)
 *         @value = string value of the item
 *         @pvalue = where to return the value (1% => 0.01)
 *
 * Return: @true if successful.
 *         @false on error, and issue a message.
 */
bool parse_pct(const char* dbname, const char* key, const char* svalue, double* pvalue)
{
	const char* unit;
	if (!cfg_get_float_units(dbname, key, svalue, units_percent, "%", pvalue, &unit))
		return false;
	*pvalue /= 100.0;
	return true;
}

/*
 * Additionally verify that value is in range [vmin ... vmax]
 */
bool parse_pct(const char* dbname, const char* key, const char* svalue, double* pvalue,
	       double vmin, double vmax)
{
	if (!parse_pct(dbname, key, svalue, pvalue))
		return false;

	if (!(*pvalue >= vmin && *pvalue <= vmax))
	{
		inval(dbname, key);
		return false;
	}

	return true;
}

/*
 * Parse time (result value: seconds).
 *
 * Parameters:
 *         @dbname = data source descriptive name (for error messaging)
 *         @key = data item key name (for error messaging)
 *         @value = string value of the item
 *         @pvalue = where to return the value
 *
 * Return: @true if successful.
 *         @false on error, and issue a message.
 */
bool parse_sec(const char* dbname, const char* key, const char* svalue, int* pvalue)
{
	const char* unit;

	return cfg_get_int_units(dbname, key, svalue, units_time, "sec", pvalue, &unit) &&
	       convert_unit_second(dbname, key, pvalue, unit);
}

/*
 * Parse boolean value.
 *
 * Parameters:
 *         @dbname = data source descriptive name (for error messaging)
 *         @key = data item key name (for error messaging)
 *         @value = string value of the item
 *         @pvalue = where to return the value
 *
 * Return: @true if successful.
 *         @false on error, and issue a message.
 */
bool parse_bool(const char* dbname, const char* key, const char* svalue,
	        tribool* pvalue)
{
	bool bv;
	bool res = parse_bool(dbname, key, svalue, &bv);
	if (res)
		*pvalue = (tribool) bv;
	return res;
}

bool parse_bool(const char* dbname, const char* key, const char* svalue,
	        bool* pvalue)
{
	return cfg_get_bool(dbname, key, svalue, pvalue);
}

/*
 * Parse domain control mode.
 *
 * Possible values:
 *
 *    off
 *    auto
 *    direct
 *    auto,direct (in various combinations/order)
 *
 * Return: @true if successful.
 *         @false on error, and issue a message.
 *
 * On successful return, *@pvalue is a bitmask of CTRL_MODE_xxx.
 */
bool parse_control_mode(const char* dbname, const char* key, const char* svalue,
			unsigned int* pvalue)
{
	unsigned int rmask = 0;
	bool res = true;
	char* saveptr;
	char* token;
	const char* delim =  ", \t";
	char* xp = xstrdup(svalue);
	int ntokens = 0;
	bool off = false;

	for (token = strtok_r(xp, delim, &saveptr);
	     token != NULL;
	     token = strtok_r(NULL, delim, &saveptr))
	{
		if (!*token)
			continue;

		ntokens++;

		if (streqi(token, "off"))
			off = true;
		else if (streqi(token, "auto"))
			rmask |= CTRL_MODE_AUTO;
		else if (streqi(token, "direct"))
			rmask |= CTRL_MODE_DIRECT;
		else
			res = false;
	}

	free(xp);

	if (ntokens == 0)
		res = false;

	if (off && rmask != 0)
		res = false;

	if (res)
		*pvalue = rmask;
	else
		inval(dbname, key);

	return res;
}


/******************************************************************************
*                              helper routines                                *
******************************************************************************/

/*
 * Issue a message about invalid key value in the configuration file
 */
static void inval(const char* dbname, const char* key)
{
	error_msg("invalid value for \"%s\" in %s", key, dbname);
}

static void inval(cfg_database_t cfg, const char* key)
{
	inval(cfg_dbname(cfg), key);
}

