/*
 *  MEMBALANCE daemon
 *
 *  config.h - Configuration, mostly readable from /etc/membalance.conf
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

/*
 * Define item identifiers (config_item_xxx)
 */
#undef CONFIG_ITEM
#undef CONFIG_ITEM_CONST
#define CONFIG_ITEM(name, type, defval)       config_item_##name,
#define CONFIG_ITEM_CONST(name, type, defval) CONFIG_ITEM(name, type, defval)
enum config_item_seq
{
	#include "config_def.h"
};

/*
 * Define configuration structure
 *
 * For every config item XXX, the following fields methods are generated:
 *
 *     config.isset_XXX()     if item value has been explicitly set
 *     config.isdef_XXX()     if item value has been defaulted
 *     config.isval_XXX()     it item value is available (explicitly set or defaulted)
 *     config.XXX             item value (if available: check isval_XXX)
 *
 */
class membalance_config
{
public:
	membalance_config();
	void defaults(void);
	void merge(const membalance_config& src);
	membalance_config& operator=(const membalance_config& src);

public:
	/*
	 * Sequence number
	 */
	unsigned long seq;          /* incremented by constructor */

	/*
	 * Mask of all items states
	 */
	unsigned long isset_mask;   /* for explicitly set items */
	unsigned long isdef_mask;   /* for defaulted items */

	/*
	 * Define item masks (config_itemmask_xxx)
	 */
#undef CONFIG_ITEM
#undef CONFIG_ITEM_CONST
#define CONFIG_ITEM(name, type, defval)       const static unsigned long config_itemmask_##name = (1UL << config_item_##name);
#define CONFIG_ITEM_CONST(name, type, defval) CONFIG_ITEM(name, type, defval)
#include "config_def.h"

	/*
	 * Mask of all constant items (config_itemmask_all_const)
	 */
#undef CONFIG_ITEM
#undef CONFIG_ITEM_CONST
#define CONFIG_ITEM(name, type, defval)
#define CONFIG_ITEM_CONST(name, type, defval)    config_itemmask_##name |
	const static unsigned long config_itemmask_all_const =
		#include "config_def.h"
		0;

	/*
	 * Mask of all non-constant items (config_itemmask_all_nonconst)
	 */
#undef CONFIG_ITEM
#undef CONFIG_ITEM_CONST
#define CONFIG_ITEM(name, type, defval)    config_itemmask_##name |
#define CONFIG_ITEM_CONST(name, type, defval)
	const static unsigned long config_itemmask_all_nonconst =
		#include "config_def.h"
		0;

	/*
	 * Define fields (xxx) and methods isval_xxx(), set_xxx() etc.
	 */
#undef CONFIG_ITEM
#undef CONFIG_ITEM_CONST

#define CONFIG_ITEM(name, type, defval)       					\
	type name;								\
	void set_##name(type val)			       			\
		{       							\
		  name = val;   						\
		  isset_mask |= config_itemmask_##name; 			\
		  isdef_mask &= ~config_itemmask_##name;			\
		}       							\
	bool isval_##name(void)	const						\
		{ return (isset_mask | isdef_mask) & config_itemmask_##name; }  \
	bool isset_##name(void)	const						\
		{ return isset_mask & config_itemmask_##name; }  		\
	bool isdef_##name(void)	const						\
		{ return isdef_mask & config_itemmask_##name; }

#define CONFIG_ITEM_CONST(name, type, defval) 					\
	static const type name = defval;					\
	bool isval_##name() const { return true; }      			\
	bool isset_##name() const { return false; }     			\
	bool isdef_##name() const { return true; }

#include "config_def.h"

protected:
	static unsigned long seq_seq;
};

#ifdef EXTERN_ALLOC
unsigned long membalance_config::seq_seq = 0;
#endif

/*
 * Constructor
 */
inline membalance_config::membalance_config()
{
	/*
	 * Call defaults() to suppress compiler warning about an access to
	 * possibly uninitialized members (compiler does not follow
	 * member <-> mask relationship).
	 */
	isset_mask = 0;
	isdef_mask = 0;
	seq = ++seq_seq;
	defaults();
}

/*
 * Default values
 */
#undef CONFIG_ITEM
#undef CONFIG_ITEM_CONST
#define CONFIG_ITEM(name, type, defval)				\
	if (!isset_##name())    				\
	{       						\
		name = defval;  				\
		isdef_mask |= config_itemmask_##name;   	\
	}
#define CONFIG_ITEM_CONST(name, type, defval)
inline void membalance_config::defaults(void)
{
	isdef_mask |= config_itemmask_all_const;
	#include "config_def.h"
}

/*
 * Merge in values from @src
 */
#undef CONFIG_ITEM
#undef CONFIG_ITEM_CONST
#define CONFIG_ITEM(name, type, defval)						\
	if ((!isval_##name() && src.isval_##name()) ||  			\
	    (isdef_##name() && src.isset_##name()))      		        \
	    {   								\
		    name = src.name;    					\
		    isset_mask = (isset_mask & ~config_itemmask_##name) |       \
				 (src.isset_mask & config_itemmask_##name);     \
		    isdef_mask = (isdef_mask & ~config_itemmask_##name) |       \
			         (src.isdef_mask & config_itemmask_##name);     \
	    }
#define CONFIG_ITEM_CONST(name, type, defval)
inline void membalance_config::merge(const membalance_config& src)
{
	#include "config_def.h"
}

/*
 * Copy assignment
 */
#undef CONFIG_ITEM
#undef CONFIG_ITEM_CONST
#define CONFIG_ITEM(name, type, defval)				\
	if (src.isval_##name())					\
		name = src.name;
#define CONFIG_ITEM_CONST(name, type, defval)
inline membalance_config& membalance_config::operator=(const membalance_config& src)
{
	#include "config_def.h"
	seq = src.seq;
	isset_mask = src.isset_mask;
	isdef_mask = src.isdef_mask;
	return *this;
}

#undef CONFIG_ITEM
#undef CONFIG_ITEM_CONST

#endif // __CONFIG_H__

