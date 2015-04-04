/*
 * Config file parser
 */

#ifndef __CONFIG_PARSER_H__
#define __CONFIG_PARSER_H__

typedef struct __cfg_database_opaque* cfg_database_t;

/*
 * Get descriptive name of the configuration source
 */
const char* cfg_dbname(cfg_database_t vcfg);

/*
 * Parse configuration.
 * Issue a message if syntax error is detected.
 *
 * Return parsed database handle.
 * On hard error returns NULL.
 */
cfg_database_t cfg_parse(const char* dbname, int fd);
cfg_database_t cfg_parse(const char* dbname, char* content, size_t content_size);

/*
 * Destroy created configuration database.
 */
void cfg_destroy(cfg_database_t cfg);

/*
 * Get key value as string.
 * Return @true if value is present, false if not.
 */
bool cfg_get_string(cfg_database_t cfg, const char* key, const char** value);

/*
 * Get key value as specified type.
 * Return:
 *     @true if key value is present and valid
 *     @false if key value is present and not valid (messge is issued)
 *     @maybe if key value is not present
 */
tribool cfg_get_bool(cfg_database_t cfg, const char* key, bool* value);
tribool cfg_get_int(cfg_database_t cfg, const char* key, int* value);
tribool cfg_get_uint(cfg_database_t cfg, const char* key, unsigned int* value);
tribool cfg_get_long(cfg_database_t cfg, const char* key, long* value);
tribool cfg_get_ulong(cfg_database_t cfg, const char* key, unsigned long* value);

/*
 * Get key value as specified type.
 * Return:
 *     @true if key value is valid
 *     @false if key value is not valid (messge is issued)
 */
bool cfg_get_bool(const char* cname, const char* key, const char* sv, bool* value);

/*
 * Convert @svalue to (@value and @used_unit).
 * Format of @svalue is "value[space][unit]".
 *
 * For example: "10%" or "5 %" or "10kb" or "5 Gb/s".
 *
 * The list of valid units is passed in @units as null-terminated array.
 * May include empty string.
 *
 * Default unit is specified in @default_unit (may be NULL).
 *
 * Returns:
 *      @true if parsed fine
 *      @false if cannot parse (error message is issued)
 *
 * If successful, return result to @value, and used unit
 * to @used_unit.
 *
 * @dbname and @key are used for diagnostic messages only.
 */
bool cfg_get_long_units(const char* dbname, const char* key,
			const char* svalue,
			const char** units, const char* default_unit,
			long* value, const char** used_unit);
bool cfg_get_ulong_units(const char* dbname, const char* key,
			const char* svalue,
			const char** units, const char* default_unit,
			unsigned long* value, const char** used_unit);
bool cfg_get_int_units(const char* dbname, const char* key,
			const char* svalue,
			const char** units, const char* default_unit,
			int* value, const char** used_unit);
bool cfg_get_uint_units(const char* dbname, const char* key,
			const char* svalue,
			const char** units, const char* default_unit,
			unsigned int* value, const char** used_unit);
bool cfg_get_float_units(const char* dbname, const char* key,
			const char* svalue,
			const char** units, const char* default_unit,
			double* value, const char** used_unit);

/*
 * Similar to cfg_get_xxx_units above, however the value is fetched
 * from @cfg for the specified key.
 *
 * Return:
 *     @true if key value is present and valid
 *     @false if key value is present and not valid (messge is issued)
 *     @maybe if key value is not present
 */
tribool cfg_get_long_units(cfg_database_t cfg, const char* key,
			const char** units, const char* default_unit,
			long* value, const char** used_unit);
tribool cfg_get_ulong_units(cfg_database_t cfg, const char* key,
			const char** units, const char* default_unit,
			unsigned long* value, const char** used_unit);
tribool cfg_get_int_units(cfg_database_t cfg, const char* key,
			const char** units, const char* default_unit,
			int* value, const char** used_unit);
tribool cfg_get_uint_units(cfg_database_t cfg, const char* key,
			const char** units, const char* default_unit,
			unsigned int* value, const char** used_unit);
tribool cfg_get_float_units(cfg_database_t cfg, const char* key,
			const char** units, const char* default_unit,
			double* value, const char** used_unit);

/*
 * Validate for the absence of unknown (e.g. mis-spelled) keys.
 * Return:
 *     @true if no unrecognized keys
 *     @false if there are unrecognized keys (message issued)
 */
bool cfg_validate_keys(cfg_database_t cfg, const char** keys);
bool cfg_validate_keys(cfg_database_t cfg, const string_set& keys);

/* helper routines */
bool a2ulong(const char* cp, unsigned long* pval);
bool a2long(const char* cp, long* pval);
bool a2int(const char* cp, int* pavl);
bool a2double(const char* cp, double* pval);

#endif // __CONFIG_PARSER_H__

