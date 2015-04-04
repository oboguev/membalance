/*
 *  MEMBALANCE daemon
 *
 *  membalanced.h - Master include for membalanced
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

#ifndef __MEMBALANCED_H__
#define __MEMBALANCED_H__

#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif

#ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 199309
#endif

#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <poll.h>
#include <stdarg.h>
#include <syslog.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <uuid/uuid.h>
#include <math.h>
#include <float.h>

extern "C" {
#include <xenctrl.h>
#include <xenstore.h>
#include <libxl.h>
#include <libxlutil.h>
#include <libxl_utils.h>
}

#include <iostream>
#include <new>
#include <string>
#include <set>
#include <map>
#include <vector>
#include <algorithm>
#include <iterator>
using namespace std;


/******************************************************************************
*                             local definitions                               *
******************************************************************************/

/* development-time conditional */
#ifdef DEVEL
  #define IF_DEVEL_ELSE(a, b)  (a)
#else
  #define IF_DEVEL_ELSE(a, b)  (b)
#endif

/* likely or unlikely conditions */
#define likely(x)       __builtin_expect((x), true)
#define unlikely(x)     __builtin_expect((x), false)

/* declaration/initialization of global variables */
#ifdef EXTERN_ALLOC
  #define EXTERN
  #define INIT(v)  = v
#else
  #define EXTERN  extern
  #define INIT(v)
#endif

#define EXTERN_CONST  static const
#define CINIT(v)  = v

/* exit status codes */
#define EXIT_SUCCESS  0
#define EXIT_FAILURE  1

#define countof(x)  (sizeof(x) / sizeof((x)[0]))

#ifndef min
  #define min(a, b)  ((a) < (b) ? (a) : (b))
#endif

#ifndef max
  #define max(a, b)  ((a) > (b) ? (a) : (b))
#endif

#define CHECK(cond)  do { if (!(cond))  goto cleanup; } while (0)
#define PCHECK(cond, msg)  do { if (!(cond)) { error_perror(msg);  goto cleanup; }} while (0)

#define streq(s1, s2)  (0 == strcmp((s1), (s2)))
#define streqi(s1, s2)  (0 == strcasecmp((s1), (s2)))

#define debug_msg(level, fmt, ...)  do {	\
	if (unlikely(debug_level >= (level)))   \
	    notice_msg(fmt, ##__VA_ARGS__);     \
} while (0)

#define free_ptr(p)  do {	\
	if (p) {		\
		free(p);	\
		(p) = NULL;     \
	}       		\
} while (0)

#define zap(x)  memset(&(x), 0, sizeof(x))

#define UUID_STRING_SIZE (36 + 1)

#define __noreturn__         __attribute__ ((noreturn))
#define __format_printf__    __attribute__ ((format (printf, 1, 2)))

// typedef int bool;

#ifndef TRUE
  #define TRUE 1
  #define FALSE 0
#endif

#ifndef true
  #define true 1
  #define false 0
#endif

/* static assert */
#define COMPILE_TIME_ASSERT3(COND, MSG)   typedef char static_assertion_##MSG[(!!(COND))*2-1]
#define COMPILE_TIME_ASSERT2(X, L)        COMPILE_TIME_ASSERT3(X, static_assertion_at_line_##L)
#define COMPILE_TIME_ASSERT(X)            COMPILE_TIME_ASSERT2(X, __LINE__)

/* milliseconds in a second */
#define MSEC_PER_SEC   1000

/* microseconds in a millsecond */
#define USEC_PER_MSEC 1000

/* microseconds in a second */
#define USEC_PER_SEC (1000 * 1000)

/* nanoseconds in a millsecond */
#define NSEC_PER_MSEC  (1000 * 1000)

/*
 * execute operation as restartable after a signal
 *
 * note: ERESTARTSYS can also leak to userspace in some rare cases
 *       (e.g. under strace), however ERESTARTSYS is not defined
 *       in userspace includes
 */
#define DO_RESTARTABLE(rc, op)  						\
	do { (rc) = (op); }							\
	while ((rc) == -1 && errno == EINTR)

/* tri-state type */
typedef enum __tribool
{
	TriTrue = true,
	TriFalse = false,
	TriMaybe = -1
}
tribool;

/* XenStore transaction status */
typedef enum __XsTransactionStatus
{
	XSTS_OK = 0,	/* success */
	XSTS_RETRY,     /* retry again */
	XSTS_NORETRY,   /* out of retry limit */
	XSTS_FAIL       /* failed */
}
XsTransactionStatus;

#ifndef __MAP_SS_DEFINED__
  #define __MAP_SS_DEFINED__
  typedef std::map<std::string, std::string> map_ss;
#endif

/* @map[@key] */
inline static const char* k2v(const map_ss& kv, const char* key)
{
	map_ss::const_iterator it = kv.find(std::string(key));
	if (it == kv.end())  return NULL;
	return it->second.c_str();
}

/* kv[key] = val */
inline static void set_kv(map_ss& kv, const char* key, const char* val)
{
	kv[std::string(key)] = std::string(val);
}

typedef std::map<long, std::string>   domid2string;
typedef std::set<std::string> string_set;

/* dset += cp */
inline static void insert(string_set& dset, const char* cp)
{
	dset.insert(std::string(cp));
}

inline static bool contains(const string_set& dset, const std::string& s)
{
	return dset.find(s) != dset.end();
}

inline static bool contains(const string_set& dset, const char* s)
{
	return contains(dset, std::string(s));
}

template<typename T>
inline static void remove_at(std::vector<T>& vec, unsigned k)
{
	vec.erase(vec.begin() + k);
}

template<typename T>
inline static void insert_at(std::vector<T>& vec, unsigned k, const T& val)
{
	vec.insert(vec.begin() + k, val);
}

/* round @n upwards to next multiple of @r */
#define roundup(n, r)  ((((n) + (r) - 1) / (r)) * (r))

/* round @n downwards to multiple of @r */
#define rounddown(n, r)  (((n) / (r)) * (r))

/* Configuration */
#include "config.h"

/* membalance Xen domain descriptor */
#include "domain.h"

/* index of poll descriptors */
enum
{
	NPFD_SIGTERM = 0,
	NPFD_SIGHUP,
	NPFD_SIGUSR1,
	NPFD_SIG_CTRL,
	NPFD_XS,	/* must be the last */
	NPFD_COUNT
};

enum
{
	CTL_REQ_PAUSE = 1,
	CTL_REQ_RESUME,
	CTL_REQ_RESUME_FORCE
};

COMPILE_TIME_ASSERT(NPFD_XS + 1 == NPFD_COUNT);

/*
 * Signal to use for membalancectl -> membalanced signalling.
 *
 * We use real-time signals instead of say SIGUSR2 because individual real-time signals
 * are queued, whereas regular signals coalesce and only the last pending signal of a
 * given kind is reported -- preceding pending signals of the same kins are lost.
 */
#define SIG_CTRL (SIGRTMIN + 5)


/******************************************************************************
*                          forward declarations                               *
******************************************************************************/

int do_main_membalancectl(int argc, char** argv);
void make_membalance_rundir(void);
void load_configuration(bool reload = false);
void reload_configuration(void);
void select_clk_id(void);
struct timespec getnow(void);
int64_t timespec_diff_ms(const struct timespec& ts1, const struct timespec& ts0);
int64_t calc_wait_ms(const struct timespec& ts0_sched, const struct timespec& ts0_pending);
void handle_signals(struct pollfd* pollfds, bool* p_recheck_time);
void fatal_perror(const char* fmt, ...) __noreturn__ __format_printf__;
void fatal_msg(const char* fmt, ...) __noreturn__ __format_printf__;
void error_perror(const char* fmt, ...) __format_printf__;
void warning_perror(const char* fmt, ...) __format_printf__;
void error_msg(const char* fmt, ...) __format_printf__;
void warning_msg(const char* fmt, ...) __format_printf__;
void notice_msg(const char* fmt, ...) __format_printf__;
void out_of_memory(void) __noreturn__;
void print_timestamp(FILE* fp);
const char* plural(long n);
void terminate(void) __noreturn__;
void shutdown_xs(void);
void start_rcmd_server(void);
void stop_rcmd_server(void);
int rcmd_get_npollfds(void);
void rcmd_setup_pollfds(struct pollfd* pollfds);
bool rcmd_handle_pollfds(struct pollfd* pollfds);
void begin_xs(void);
void abort_xs(void);
XsTransactionStatus commit_xs(int* p_nretries);
bool in_transaction_xs(void);
void begin_singleop_xs(void);
XsTransactionStatus commit_singleop_xs(int* p_nretries);
void abort_singleop_xs(void);
void refresh_xs(void);
void handle_xs_watch(void);
void handle_xs_watch_event(long domain_id, const char* subpath);
void initialize_xs(struct pollfd* pollfds);
void initialize_xl(void);
void initialize_xl_ctl(void);
void on_add_managed(void);
void enumerate_local_domains(domid_set& ids);
void enumerate_local_domains_as_pending(void);
void process_pending_domains(void);
tribool domain_alive(long domain_id);
void unmanage_domain(long domain_id);
void transition_managed_dead(long domain_id);
void transition_unmanaged_dead(long domain_id);
void transition_pending_dead(long domain_id);
void transition_managed_unmanaged(long domain_id);
void transition_pending_unmanaged(long domain_id);
void resync_qid(void);
void qid_dead(long domain_id);
void rescan_domains_on_config_change(const membalance_config& old_config);
int rescan_domain(long domain_id, char** message);
void update_membalance_interval_and_protection(void);
void update_membalance_interval(void);
bool init_membalance_report(long domain_id);
tribool read_value_from_xs(domain_info* dom, const char* subpath, long* p_value, long minval);
tribool read_value_from_xs(domain_info* dom, const char* subpath, char** p_value);
int get_domain_settings(long domain_id, char** message, map_ss& kv);
void show_domains(FILE* fp);
void sched_memory(void);
void sched_slept(int64_t ms);
int sched_freemem(u_quad_t amt,
		  bool above_slack,
		  bool draw_reserved_hard,
		  bool must,
		  int timeout,
		  u_quad_t* p_freemem_with_slack,
		  u_quad_t* p_freemem_less_slack);
void collect_domain_memory_info(void);
void read_siginfo(int fd, struct signalfd_siginfo* fdsi);
u_int pause_memsched(void);
u_int resume_memsched(bool force);
int set_log_sink(int which);
void show_debugging_info(FILE* fp = NULL);
void* xmalloc(size_t size);
char* xstrdup(const char* cp);
char* xprintf(const char* fmt, ...);
void setfmt(map_ss& kv, const char* key, const char* fmt, ...);
bool starts_with(const char* s, const char* prefix);
bool parse_kb(const char* dbname, const char* key, const char* svalue,
	      const char* default_unit, long* pvalue);
bool parse_kb_sec(const char* dbname, const char* key, const char* svalue,
		  const char* default_unit, long* pvalue);
bool parse_pct(const char* dbname, const char* key, const char* svalue,
	       double* pvalue);
bool parse_pct(const char* dbname, const char* key, const char* svalue,
	       double* pvalue, double vmin, double vmax);
bool parse_sec(const char* dbname, const char* key, const char* svalue,
	       int* pvalue);
bool parse_bool(const char* dbname, const char* key, const char* svalue,
	        tribool* pvalue);
bool parse_bool(const char* dbname, const char* key, const char* svalue,
	        bool* pvalue);
bool parse_control_mode(const char* dbname, const char* key, const char* svalue,
			unsigned int* pvalue);
void config_eval_after_xl_init(void);
void read_domain_reports(void);
bool trim_to_quota(domain_info* dom);
void do_expand_domain(domain_info* dom, long size);
void do_shrink_domain(domain_info* dom, long size);
void do_resize_domain(domain_info* dom, long size, char action = 0);
long xen_wait_free_memory(long free_target, int timeout_ms);
long get_xen_free_memory(void);
long get_xen_free_slack(void);
long get_xen_physical_memory(void);
long get_xen_dom0_minsize(void);
long get_xen_dom0_target(void);
long get_xen_dom_target(long domain_id);
long xen_wait_free_memory_stable(int timeout_ms);
long xen_domain_uptime(long domain_id);
bool is_runnable(const xc_domaininfo_t* xcinfo);
bool is_runnable(domain_info* dom);
char* show_status(int verbosity);
const char* decode_memsize(long kbs);


/******************************************************************************
*                          program identification                             *
******************************************************************************/

EXTERN const char* progname     INIT("membalanced");
EXTERN const char* progversion  INIT("0.1");

EXTERN const char* membalancectl_progname     INIT("membalancectl");
EXTERN const char* membalancectl_progversion  INIT("0.1");


/******************************************************************************
*                             configuration                                   *
******************************************************************************/

/* If set to true, runs as membalancectl rather than membalanced */
EXTERN bool prog_membalancectl INIT(false);

/* Debugging output level */
EXTERN int debug_level INIT(0);

/* Include timestamps in the log file or console records */
EXTERN bool log_timestamps INIT(true);

/* Configuration */
EXTERN membalance_config config;			/* current */
EXTERN membalance_config default_config;		/* default */

/* Directory where membalanced(d) keeps it files */
#define MEMBALANCE_DIR_PATH "/var/run/membalance"
EXTERN const char* membalance_dir_path INIT(MEMBALANCE_DIR_PATH);

/* Daemon interlock file */
EXTERN const char* membalanced_interlock_path INIT(MEMBALANCE_DIR_PATH "/membalanced.lock");

/* Socket for daemon management connection */
EXTERN const char* socket_path INIT(MEMBALANCE_DIR_PATH "/membalanced.socket");

#ifdef DEVEL
  /* When not zero, membalanced runs in test mode */
  EXTERN int testmode INIT (0);
#else
  /* Let compiler weed out test code branches */
  const static int testmode = 0;
#endif


/******************************************************************************
*                                global data                                  *
******************************************************************************/

EXTERN bool run_as_daemon INIT(false);	/* run as a daemon */

EXTERN FILE* log_fp INIT(NULL);		/* file for logging (normally stderr or NULL) */

EXTERN bool log_syslog INIT(false);	/* log to syslog */

EXTERN struct xs_handle* xs INIT(NULL);	/* xenstore connection */

EXTERN libxl_ctx* xl_ctx INIT(NULL);    /* libxl context */

EXTERN xc_interface* xc_handle INIT(NULL);     /* handle to xenctrl interface */

EXTERN bool update_interval_in_xs INIT(true);  /* update interval key in xenstore */

EXTERN u_int memsched_pause_level INIT(0);     /* suspend memory scheduling when not 0 */

EXTERN u_long memquant_kbs;       	/* Xen memory allocation quant in kbs */
EXTERN u_long pagesize_kbs;       	/* Xen page size in kbs */

EXTERN long xen_free_slack;		/* Xen free memory slack (in kbs) */

EXTERN struct __doms
{
	/*
	 * domains subject to the adjustment of their memory allocation by membalance
	 */
	domid2info  managed;

	/*
	 * domains not subject to the adjustment of their memory allocation by membalance
	 */
	domid2info  unmanaged;

	/*
	 * domains with information about them still being collected and parsed
	 * and that will be relegated to managed or unmanaged later,
	 * but currently not subject to the adjustment of their memory
	 * allocation by membalance yet
	 */
	domid2info  pending;

	/*
	 * qid's for all known domains (managed, unmanaged and pending)
	 * who had qid's assigned in the past.
	 *
	 * qid is used for linking paths in xenstore (see explanation in
	 * the header of xenstore.cpp)
	 */
	domid2string  qid;
} doms;

EXTERN const char* membalanced_log_path INIT("/var/log/membalanced.log");

/******************************************************************************
*                          dependend includes                                 *
******************************************************************************/

#include "config_parser.h"
#include "test.h"

#endif // __MEMBALANCED_H__

