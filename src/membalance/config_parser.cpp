/*
 * Config file parser
 */

#include "membalanced.h"

/******************************************************************************
*                             local definitions                               *
******************************************************************************/

#ifndef __MAP_SS_DEFINED__
  #define __MAP_SS_DEFINED__
  typedef std::map<std::string, std::string> map_ss;
#endif

class cfg_database
{
public:
	std::string dbname;	/* descriptive name for messages */
	map_ss kv;		/* key->value map */
};


/******************************************************************************
*                           forward declarations                              *
******************************************************************************/

static void parse(cfg_database* cfg, char* content);
static void error_atline(cfg_database* cfg, int nline);
static void inval(cfg_database_t cfg, const char* key);
static void inval(const char* dbname, const char* key);
static bool extract_value_units(
	char dtype, char* pbuf, size_t bufsize,
	const char* dbname, const char* key, const char* svalue,
	const char** units, const char* default_unit, const char** used_unit);


/******************************************************************************
*                             parser routines                                 *
******************************************************************************/

/*
 * Get descriptive name of the configuration source
 */
const char* cfg_dbname(cfg_database_t vcfg)
{
	cfg_database* cfg = (cfg_database*) vcfg;
	return cfg->dbname.c_str();
}

/*
 * Parse configuration.
 * Issue a message if syntax error is detected.
 *
 * Return parsed database handle.
 * On hard error returns NULL.
 *
 * When parsing the file:
 *     - tabs are converted to spaces
 *     - leading and trailing blanks are stripped
 *     - multiple blanks inside values are collapsed
 */
cfg_database_t cfg_parse(const char* dbname, int fd)
{
	struct stat st;
	char* bp = NULL;
	ssize_t sz;
	cfg_database_t cfg = NULL;

	CHECK(fstat(fd, &st) == 0);

	bp = (char*) malloc(st.st_size + 2);
	if (bp == NULL)
	{
		errno = ENOMEM;
		goto cleanup;
	}

	if (st.st_size != 0)
	{
		CHECK(lseek(fd, 0, SEEK_SET) != (off_t) -1);
		sz = read(fd, bp, st.st_size);
		CHECK(sz > 0);
		if (sz != st.st_size)
		{
			error_msg("unable to read %s: incomplete read", dbname);
			goto out;
		}
		strcpy(bp + st.st_size, "\n");
	}

	cfg = cfg_parse(dbname, bp, st.st_size + 2);

out:

	free_ptr(bp);
	return cfg;

cleanup:
	error_perror("unable to read %s", dbname);
	goto out;
}

/*
 * Parse configuration.
 * Issue a message if syntax error is detected.
 *
 * Return parsed database handle.
 * On hard error returns NULL.
 *
 * When parsing the file:
 *     - tabs are converted to spaces
 *     - leading and trailing blanks are stripped
 *     - multiple blanks inside values are collapsed
 */
cfg_database_t cfg_parse(const char* dbname, char* content, size_t content_size)
{
	bool free_content = false;
	char* p;
	int len;

	cfg_database* cfg = new cfg_database;
	cfg->dbname = dbname;

	/*
	 * make sure content is terminated by "\n\0"
	 */
	if (content_size >= 2 &&
	    content[content_size - 2] == '\n' &&
	    content[content_size - 1] == '\0')
	{
		/* already terminated */
	}
	else
	{
		p = (char*) malloc(content_size + 2);
		if (p == NULL)
		{
			errno = ENOMEM;
			error_perror("unable to parse %s", dbname);
			return NULL;
		}

		memcpy(p, content, content_size);
		content = p;
		strcpy(content + content_size, "\n");
		free_content = true;
	}

	/* make sure last line is nl-terminated */
	len = strlen(content);
	if (len == 0)
		goto cleanup;
	if (content[len - 1] != '\n')
		strcpy(content + len, "\n");

	parse(cfg, content);

cleanup:

	if (free_content)
		free(content);

	return (cfg_database_t) cfg;
}

/*
 * Parse @content.
 * Guaranted to be terminated by nl-nul.
 */
static void parse(cfg_database* cfg, char* content)
{
	char* nextline = content;
	char* line;
	char* p;
	std::string key;
	std::string val;
	int nline = 0;
	char* xp;
	const char* cp;
	char buf[256];
	bool space;

	for (;;)
	{
		/* move to next line */
		line = nextline;
		nline++;
		if (!*line)
			break;
		key.clear();
		val.clear();

		/* find the end of this line and start of next line */
		nextline = strchr(line, '\n') + 1;

		/*
		 * parse current line
		 */
		p = line;

		/* skip leading spaces */
		while (isblank(*p))
			p++;

		/* comment? */
		if (*p == '#') continue;

		/* consume key */
		while (!isblank(*p) && *p != '\n' && *p != '=' && *p != '#')
			key += *p++;
		while (isblank(*p))
			p++;

		if (*p != '=')
		{
			/* do not message if blank line */
			if (key.length() != 0)
				error_atline(cfg, nline);
			continue;
		}

		/* skip '=' and blanks after */
		p++;
		while (isblank(*p))
			p++;

		/* copy value */
		while (*p != '\n' && *p != '#')
			val += *p++;

		/* duplicate it into a writable buffer */
		cp = val.c_str();
		if (strlen(cp) >= countof(buf))
			xp = xstrdup(val.c_str());
		else
			strcpy(xp = buf, cp);

		/* tabs -> space */
		for (p = xp; *p;  p++)
			if (*p == '\t')  *p = ' ';

		/* strip trailing blanks */
		for (p = xp + strlen(xp) - 1;  p >= xp;  p--)
		{
			if (*p != ' ')
				break;
			*p = '\0';
		}

		/* record purified value back to @val, collapse multiple spaces */
		val.clear();
		for (p = xp, space = false; *p; p++)
		{
			if (*p == ' ')
			{
				if (!space)
				{
					val += *p;
					space = true;
				}
			}
			else
			{
				val += *p;
				space = false;
			}
		}

		/* deallocate xp if was allocated */
		if (xp != buf)
			free(xp);

		/* insert (key, val) into cfg */
		if (cfg->kv.find(key) != cfg->kv.end())
		{
			warning_msg("duplicate values for \"%s\" in %s",
				    key.c_str(), cfg->dbname.c_str());
		}
		cfg->kv[key] = val;
	}
}

static void error_atline(cfg_database* cfg, int nline)
{
	error_msg("error in %s at line %d", cfg->dbname.c_str(), nline);
}

/*
 * Destroy created configuration database.
 */
void cfg_destroy(cfg_database_t vcfg)
{
	cfg_database* cfg = (cfg_database*) vcfg;
	delete cfg;
}

/*
 * Get key value as string.
 * Return @true if value is present, false if not.
 */
bool cfg_get_string(cfg_database_t vcfg, const char* key, const char** value)
{
	cfg_database* cfg = (cfg_database*) vcfg;
	map_ss::const_iterator it = cfg->kv.find(key);

	if (it != cfg->kv.end())
	{
		*value = it->second.c_str();
		return true;
	}
	else
	{
		*value = NULL;
		return false;
	}
}

/*
 * Get key value as specified type.
 * Return:
 *     @true if key value is present and valid
 *     @false if key value is present and not valid (messge is issued)
 *     @maybe if key value is not present
 */
tribool cfg_get_bool(cfg_database_t cfg, const char* key, bool* value)
{
	const char* sv;
	if (!cfg_get_string(cfg, key, &sv))
		return TriMaybe;

	return (tribool) cfg_get_bool(cfg_dbname(cfg), key, sv, value);
}

tribool cfg_get_int(cfg_database_t cfg, const char* key, int* value)
{
	long v;
	tribool res = cfg_get_long(cfg, key, &v);

	if (res != TriTrue)
		return res;

	if (v > INT_MAX || v < INT_MIN)
	{
		inval(cfg, key);
		return TriFalse;
	}

	*value = (int) v;
	return TriTrue;
}

tribool cfg_get_uint(cfg_database_t cfg, const char* key, unsigned int* value)
{
	unsigned long v;
	tribool res = cfg_get_ulong(cfg, key, &v);

	if (res != TriTrue)
		return res;

	if (v > UINT_MAX)
	{
		inval(cfg, key);
		return TriFalse;
	}

	*value = (unsigned int) v;
	return TriTrue;
}

tribool cfg_get_long(cfg_database_t cfg, const char* key, long* value)
{
	const char* sv;
	if (!cfg_get_string(cfg, key, &sv))
		return TriMaybe;

	if (!a2long(sv, value))
	{
		inval(cfg, key);
		return TriFalse;
	}

	return TriTrue;
}

tribool cfg_get_ulong(cfg_database_t cfg, const char* key, unsigned long* value)
{
	const char* sv;
	if (!cfg_get_string(cfg, key, &sv))
		return TriMaybe;

	if (!a2ulong(sv, value))
	{
		inval(cfg, key);
		return TriFalse;
	}

	return TriTrue;
}


/*
 * Get key value as specified type.
 * Return:
 *     @true if key value is valid
 *     @false if key value is not valid (messge is issued)
 */
bool cfg_get_bool(const char* cname, const char* key, const char* sv, bool* value)
{
	if (streq(sv, "1") || streqi(sv, "yes") || streqi(sv, "y") ||
	    streqi(sv, "true") || streqi(sv, "on") ||
	    streqi(sv, "enable") || streqi(sv, "enabled"))
	{
		*value = true;
		return true;
	}
	else if (streq(sv, "0") || streqi(sv, "no") || streqi(sv, "n") ||
		 streqi(sv, "false") || streqi(sv, "off") ||
		 streqi(sv, "disable") || streqi(sv, "disabled"))
	{
		*value = false;
		return true;
	}
	else
	{
		inval(cname, key);
		return false;
	}
}


/*
 * Unit-based routines (cfg version)
 */

tribool cfg_get_long_units(cfg_database_t cfg, const char* key,
			const char** units, const char* default_unit,
			long* value, const char** used_unit)
{
	const char* sv;
	if (!cfg_get_string(cfg, key, &sv))
		return TriMaybe;

	bool res = cfg_get_long_units(cfg_dbname(cfg), key, sv,
				      units, default_unit,
				      value, used_unit);

	return res ? TriTrue : TriFalse;
}

tribool cfg_get_ulong_units(cfg_database_t cfg, const char* key,
			const char** units, const char* default_unit,
			unsigned long* value, const char** used_unit)
{
	const char* sv;
	if (!cfg_get_string(cfg, key, &sv))
		return TriMaybe;

	bool res = cfg_get_ulong_units(cfg_dbname(cfg), key, sv,
				       units, default_unit,
				       value, used_unit);

	return res ? TriTrue : TriFalse;
}

tribool cfg_get_int_units(cfg_database_t cfg, const char* key,
			const char** units, const char* default_unit,
			int* value, const char** used_unit)
{
	const char* sv;
	if (!cfg_get_string(cfg, key, &sv))
		return TriMaybe;

	bool res = cfg_get_int_units(cfg_dbname(cfg), key, sv,
				     units, default_unit,
				     value, used_unit);

	return res ? TriTrue : TriFalse;
}

tribool cfg_get_uint_units(cfg_database_t cfg, const char* key,
			const char** units, const char* default_unit,
			unsigned int* value, const char** used_unit)
{
	const char* sv;
	if (!cfg_get_string(cfg, key, &sv))
		return TriMaybe;

	bool res = cfg_get_uint_units(cfg_dbname(cfg), key, sv,
				      units, default_unit,
				      value, used_unit);

	return res ? TriTrue : TriFalse;
}

tribool cfg_get_float_units(cfg_database_t cfg, const char* key,
			const char** units, const char* default_unit,
			double* value, const char** used_unit)
{
	const char* sv;
	if (!cfg_get_string(cfg, key, &sv))
		return TriMaybe;

	bool res = cfg_get_float_units(cfg_dbname(cfg), key, sv,
				      units, default_unit,
				      value, used_unit);

	return res ? TriTrue : TriFalse;
}

/*
 * Unit-based routines (string value version)
 */

bool cfg_get_long_units(const char* dbname, const char* key,
			const char* svalue,
			const char** units, const char* default_unit,
			long* value, const char** used_unit)
{
	char buf[64];

	if (!extract_value_units('d', buf, countof(buf),
				 dbname, key, svalue,
				 units, default_unit, used_unit))
	{
		return false;
	}

	if (!a2long(buf, value))
	{
		inval(dbname, key);
		return false;
	}

	return true;
}

bool cfg_get_ulong_units(const char* dbname, const char* key,
			const char* svalue,
			const char** units, const char* default_unit,
			unsigned long* value, const char** used_unit)
{
	char buf[64];

	if (!extract_value_units('d', buf, countof(buf),
				 dbname, key, svalue,
				 units, default_unit, used_unit))
	{
		return false;
	}

	if (!a2ulong(buf, value))
	{
		inval(dbname, key);
		return false;
	}

	return true;
}

bool cfg_get_int_units(const char* dbname, const char* key,
			const char* svalue,
			const char** units, const char* default_unit,
			int* value, const char** used_unit)
{
	long v;
	bool res = cfg_get_long_units(dbname, key, svalue,
				      units, default_unit,
				      &v, used_unit);
	if (res)
	{
		if (v > INT_MAX || v < INT_MIN)
		{
			inval(dbname, key);
			res = false;
		}
		else
		{
			*value = (int) v;
		}
	}

	return res;
}

bool cfg_get_uint_units(const char* dbname, const char* key,
			const char* svalue,
			const char** units, const char* default_unit,
			unsigned int* value, const char** used_unit)
{
	unsigned long v;
	bool res = cfg_get_ulong_units(dbname, key, svalue,
				       units, default_unit,
				       &v, used_unit);

	if (res)
	{
		if (v > UINT_MAX)
		{
			inval(dbname, key);
			res = false;
		}
		else
		{
			*value = (unsigned int) v;
		}
	}

	return res;
}

bool cfg_get_float_units(const char* dbname, const char* key,
			const char* svalue,
			const char** units, const char* default_unit,
			double* value, const char** used_unit)
{
	char buf[64];

	if (!extract_value_units('f', buf, countof(buf),
				 dbname, key, svalue,
				 units, default_unit, used_unit))
	{
		return false;
	}

	if (!a2double(buf, value))
	{
		inval(dbname, key);
		return false;
	}

	return true;
}

static bool extract_value_units(
	char dtype, char* pbuf, size_t bufsize,
	const char* dbname, const char* key, const char* svalue,
	const char** units, const char* default_unit, const char** used_unit)
{

	char* pbuf0 = pbuf;

	/*
	 * Copy value to buffer.
	 */
	if (dtype == 'd')
	{
		/* copy out integer number */
		while (isdigit(*svalue))
		{
			CHECK(pbuf - pbuf0 < (ssize_t) bufsize - 1);
			*pbuf++ = *svalue++;
		}
		CHECK(pbuf != pbuf0);
		*pbuf = '\0';
	}
	else if (dtype == 'f')
	{
		int nwhole = 0, nfrac = 0;

		/* copy out the whole part */
		while (isdigit(*svalue))
		{
			CHECK(pbuf - pbuf0 < (ssize_t) bufsize - 1);
			*pbuf++ = *svalue++;
			nwhole++;
		}
		CHECK(nwhole != 0);

		/* copy out the fractional part */
		if (*svalue == '.')
		{
			CHECK(pbuf - pbuf0 < (ssize_t) bufsize - 1);
			*pbuf++ = *svalue++;

			while (isdigit(*svalue))
			{
				CHECK(pbuf - pbuf0 < (ssize_t) bufsize - 1);
				*pbuf++ = *svalue++;
				nfrac++;
			}
			CHECK(nfrac != 0);
		}

		*pbuf = '\0';
	}
	else
	{
		goto cleanup;
	}

	/* skip blanks between the value and unit specifier */
	while (isblank(*svalue))
		svalue++;

	/* missing unit? */
	if (*svalue == '\0' && default_unit)
		svalue = default_unit;

	/* locate unit */
	for (; *units; units++)
	{
		if (streqi(svalue, *units))
		{
			*used_unit = *units;
			return true;
		}
	}

cleanup:
	inval(dbname, key);
	return false;
}

/*
 * Validate for the absence of unknown (e.g. mis-spelled) keys.
 * Return:
 *     @true if no unrecognized keys
 *     @false if there are unrecognized keys (message issued)
 */
bool cfg_validate_keys(cfg_database_t cfg, const char** keys)
{
	std::set<std::string> vkeys;
	std::string s;

	while (*keys)
	{
		s = *keys++;
		vkeys.insert(s);
	}

	return cfg_validate_keys(cfg, vkeys);
}

bool cfg_validate_keys(cfg_database_t vcfg, const string_set& keys)
{
	cfg_database* cfg = (cfg_database*) vcfg;
	bool res = true;

	for (map_ss::const_iterator it = cfg->kv.begin();  it != cfg->kv.end();   ++it)
	{
		if (!contains(keys, it->first))
		{
			error_msg("invalid option key \"%s\" in %s",
				  it->first.c_str(), cfg_dbname(vcfg));
			res = false;
		}
	}

	return res;
}


/******************************************************************************
*                               Utility routines                              *
******************************************************************************/

static void inval(const char* dbname, const char* key)
{
	error_msg("invalid value for \"%s\" in %s", key, dbname);
}

static void inval(cfg_database_t cfg, const char* key)
{
	inval(cfg_dbname(cfg), key);
}

/*
 * Convert string @cp -> unsigned long *@pval.
 * Return @true if conversion is successful.
 * Return @false if not.
 */
bool a2ulong(const char* cp, unsigned long* pval)
{
	int sv_errno = errno;
	bool res = true;
	char* ep;

	errno = 0;
	*pval = strtoul(cp, &ep, 10);

	/*
	 * some implementations of strtouls are buggy and accept
	 * numbers with minus sign in front of them
	 */
	if (errno || ep == cp || *ep || !isdigit(*cp))
		res = false;

	errno = sv_errno;

	return res;
}

/*
 * Convert string @cp -> long *@pval.
 * Return @true if conversion is successful.
 * Return @false if not.
 */
bool a2long(const char* cp, long* pval)
{
	int sv_errno = errno;
	bool res = true;
	char* ep;

	errno = 0;
	*pval = strtol(cp, &ep, 10);

	if (errno || ep == cp || *ep)
		res = false;

	errno = sv_errno;

	return res;
}

/*
 * Convert string @cp -> int *@pval.
 * Return @true if conversion is successful.
 * Return @false if not.
 */
bool a2int(const char* cp, int* pval)
{
	long lv;

	if (!a2long(cp, &lv) || lv < INT_MIN || lv > INT_MAX)
		return false;

	*pval = (int) lv;

	return true;
}

/*
 * Convert string @cp -> double *@pval.
 * Return @true if conversion is successful.
 * Return @false if not.
 */
bool a2double(const char* cp, double* pval)
{
	int sv_errno = errno;
	bool res = true;
	char* ep;

	errno = 0;
	*pval = strtod(cp, &ep);

	if (errno || ep == cp || *ep || !isdigit(*cp))
		res = false;

	errno = sv_errno;

	return res;
}

