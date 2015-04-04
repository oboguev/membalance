/*
 *  MEMBALANCE daemon
 *
 *  test.h - Membalance testing
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

#ifndef __MEMBALANCE_TEST_H__
#define __MEMBALANCE_TEST_H__

int execute_test(int argc, char** argv, char** message);

long test_get_xen_free_memory(void);
long test_get_xen_free_slack(void);
bool test_trim_to_quota(domain_info* dom);
void test_read_domain_reports(void);
void test_do_resize_domain(domain_info* dom, long size, char action);
long test_xen_domain_uptime(long domain_id);
int test_xcinfo_collect(xc_domaininfo_t** ppinfo);
void test_debugger(void);

#endif // __MEMBALANCE_TEST_H__

