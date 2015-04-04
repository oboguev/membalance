/*
 *  MEMBALANCE daemon
 *
 *  domain.h - Membalance descriptor for Xen domain
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

#ifndef __MEMBALANCE_DOMAIN_H__
#define __MEMBALANCE_DOMAIN_H__

#include "domain_info.h"


/******************************************************************************
*                               helper classes                                *
******************************************************************************/

typedef std::map<long, domain_info*>  domid2info;
typedef std::set<long> domid_set;
typedef std::set<domain_info*> dominfo_set;

/* check if @set contains @domain_id */
static inline bool contains(const domid_set& dset, long domain_id)
{
	return dset.find(domain_id) != dset.end();
}

/* check if @map contains @domain_id */
static inline bool contains(const domid2info& dmap, long domain_id)
{
	return dmap.find(domain_id) != dmap.end();
}

static inline bool contains(const domid2string& dmap, long domain_id)
{
	return dmap.find(domain_id) != dmap.end();
}

/* enumerate @map keys into @set */
static inline void keyset(domid_set& dset, const domid2info& dmap)
{
	dset.clear();
	for (domid2info::const_iterator it = dmap.begin();  it != dmap.end();   ++it)
		dset.insert(it->first);
}

static inline bool contains(const dominfo_set& dset, domain_info* dom)
{
	return dset.find(dom) != dset.end();
}

/******************************************************************************
*                         helper class: domvector                             *
******************************************************************************/

class domvector : public std::vector<domain_info*>
{
public:
	/* sort by expand_force in decreasing order */
	void sort_desc_by_expand_force(void)
	{
		std::sort(begin(), end(), sortfunc_desc_by_expand_force);
	}

	/* sort by resist_force in increasing order */
	void sort_asc_by_resist_force(void)
	{
		std::sort(begin(), end(), sortfunc_asc_by_resist_force);
	}

	/* sort by time_rate_below_low in descending order */
	void sort_desc_by_time_rate_below_low(void)
	{
		std::sort(begin(), end(), sortfunc_desc_by_time_rate_below_low);
	}

	/* sort by time_rate_below_high in descending order */
	void sort_desc_by_time_rate_below_high(void)
	{
		std::sort(begin(), end(), sortfunc_desc_by_time_rate_below_high);
	}

	/* sort by memory allocation change in ascending order */
	void sort_asc_by_memalloc_change(void)
	{
		std::sort(begin(), end(), sortfunc_asc_by_memalloc_change);
	}

	/* sort by expand_force0 in descending order */
	void sort_desc_by_expand_force0(void)
	{
		std::sort(begin(), end(), sortfunc_desc_by_expand_force0);
	}


protected:
	/*
	 * For better performance of std::sort predicate functions should be
	 * inline-able. Predicate function should return true if (arg1 < arg2)
	 * in strict weak ordering (https://www.sgi.com/tech/stl/StrictWeakOrdering.html).
	 * In particular:
	 *     predicate(x,x)  =>  false
	 *     predicate(x,y)  =>  !predicate(y,x)
	 */
	static bool sort_asc(domain_info* d1, domain_info* d2, double delta)
	{
		if (delta < 0)   return true;
		if (delta > 0)   return false;
		return d1 < d2;
	}

	static bool sort_desc(domain_info* d1, domain_info* d2, double delta)
	{
		return sort_asc(d1, d2, -delta);
	}

	static bool sort_asc(domain_info* d1, domain_info* d2, long delta)
	{
		if (delta < 0)   return true;
		if (delta > 0)   return false;
		return d1 < d2;
	}

	static bool sort_desc(domain_info* d1, domain_info* d2, long delta)
	{
		return sort_asc(d1, d2, -delta);
	}

	/* return true if @d1 > @d2 */
	static bool sortfunc_desc_by_expand_force(domain_info* d1, domain_info* d2)
	{
		return sort_desc(d1, d2, d1->expand_force - d2->expand_force);
	}

	/* return true if @d1 < @d2 */
	static bool sortfunc_asc_by_resist_force(domain_info* d1, domain_info* d2)
	{
		return sort_asc(d1, d2, d1->resist_force - d2->resist_force);
	}

	/* return true if @d1 > @d2 */
	static bool sortfunc_desc_by_time_rate_below_low(domain_info* d1, domain_info* d2)
	{
		return sort_desc(d1, d2, d1->time_rate_below_low - d2->time_rate_below_low);
	}

	/* return true if @d1 > @d2 */
	static bool sortfunc_desc_by_time_rate_below_high(domain_info* d1, domain_info* d2)
	{
		return sort_desc(d1, d2, d1->time_rate_below_high - d2->time_rate_below_high);
	}

	/* return true if @d1 < @d2 */
	static bool sortfunc_asc_by_memalloc_change(domain_info* d1, domain_info* d2)
	{
		long delta1 = d1->memsize - d1->memsize0;
		long delta2 = d2->memsize - d2->memsize0;
		return sort_asc(d1, d2, delta1 - delta2);
	}

	/* return true if @d1 > @d2 */
	static bool sortfunc_desc_by_expand_force0(domain_info* d1, domain_info* d2)
	{
		return sort_desc(d1, d2, d1->expand_force0 - d2->expand_force0);
	}
};

/******************************************************************************
*                         helper class: dom2xcinfo                            *
******************************************************************************/

class domid2xcinfo : public std::map<long, xc_domaininfo_t*>
{
public:
	domid2xcinfo();
	~domid2xcinfo();
	void collect(void);
	void reset(void);
	const xc_domaininfo_t* get(long domain_id) const;

protected:
	xc_domaininfo_t* info;
};


/******************************************************************************
*                              helper constructs                              *
******************************************************************************/

/* allows to delete an element at __it */
#define foreach_managed_domain(dom)							\
	for (domid2info::const_iterator __it = doms.managed.begin();    		\
	     __it != doms.managed.end() ? (dom = (__it++)->second, true) : false; )

#endif // __MEMBALANCE_DOMAIN_H__

