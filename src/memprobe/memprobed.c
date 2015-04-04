/*
 *  memprobed.c - per-VM MEMBALANCE daemon
 *
 *     MEMPROBED runs as a probe inside a VM and periodically samples memory
 *     pressure in a local VM and reports it to MEMBALANCED running in the
 *     hypervisor domain to let MEMBALANCED dynamically adjust memory allotment
 *     between the cooperating VMs according to their current demand and system
 *     administrator's control settings.
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif

#ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 199309
#endif

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
#include <xenstore.h>


/******************************************************************************
*                             local definitions                               *
******************************************************************************/

/* exit status codes */
#define EXIT_SUCCESS  0
#define EXIT_FAILURE  1

/*
 * CLOCK_xxx definitions extracted from <linux-src-root>/include/uapi/linux/time.h.
 * We cannot include <linux/time.h> directly since it is incompatible with <time.h>.
 */
#ifndef CLOCK_MONOTONIC
  #define CLOCK_MONOTONIC		1
#endif

#ifndef CLOCK_MONOTONIC_RAW
  #define CLOCK_MONOTONIC_RAW		4
#endif

#ifndef CLOCK_BOOTTIME
  #define CLOCK_BOOTTIME		7
#endif

#define countof(x)  (sizeof(x) / sizeof((x)[0]))
#define min(a, b)  ((a) < (b) ? (a) : (b))

#define CHECK(cond)  do { if (!(cond))  goto cleanup; } while (0)
#define PCHECK(cond, msg)  do { if (!(cond)) { error_perror(msg);  goto cleanup; }} while (0)

#define debug_msg(level, fmt, ...)  do {	\
	if (debug_level >= (level))    		\
	    notice_msg(fmt, ##__VA_ARGS__);     \
} while (0)

// #define UUID_STRING_SIZE (36 + 1)

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

/* milliseconds in a second */
#define MSEC_PER_SEC  1000

/* microseconds in a millsecond */
#define USEC_PER_MSEC 1000

/* nanoseconds in a millsecond */
#define NSEC_PER_MSEC  (1000 * 1000)

/* no xenstore transaction */
#define XS_TRANSACTION_NULL 0

/* static assert */
#define COMPILE_TIME_ASSERT3(COND, MSG)   typedef char static_assertion_##MSG[(!!(COND))*2-1]
#define COMPILE_TIME_ASSERT2(X, L)        COMPILE_TIME_ASSERT3(X, static_assertion_at_line_##L)
#define COMPILE_TIME_ASSERT(X)            COMPILE_TIME_ASSERT2(X, __LINE__)

struct paging_data
{
	/* data capture timestamp */
	struct timespec ts;

	/* running counter of size of data read from block devices
	   to buffer cache, data and VM (in KB, not pages) */
	uintmax_t pgpgin;

	/* free pages (in pages) */
	uintmax_t nr_free_pages;

	/* system memory pages */
	long mem_pages;
};

/* XenStore transaction status */
typedef enum __XsTransactionStatus
{
	XSTS_OK = 0,	/* success */
	XSTS_RETRY,     /* retry again */
	XSTS_NORETRY,   /* out of retry limit */
	XSTS_FAIL       /* failed */
}
XsTransactionStatus;

/* index of poll descriptors */
enum
{
	NPFD_SIGTERM = 0,
	NPFD_SIGHUP,
	NPFD_XS,	/* must be the last */
	NPFD_COUNT
};

COMPILE_TIME_ASSERT(NPFD_XS + 1 == NPFD_COUNT);

#ifdef DEVEL
  #define IF_DEVEL_ELSE(a, b)  (a)
#else
  #define IF_DEVEL_ELSE(a, b)  (b)
#endif


/******************************************************************************
*                          program identification                             *
******************************************************************************/

static const char *progname = "memprobed";
static const char *progversion = "0.1";


/******************************************************************************
*                              configuration                                  *
******************************************************************************/

/*
 * Data rate sampling interval.
 *
 * Memprobed takes memory pressure reading every @interval seconds
 * and reports it to membalance(d) running in the hypervisor domain.
 * Initial default value is 5 seconds, but is dynamically adjustable
 * by membalanced.
 */
static int interval = 5;

/*
 * Interval tolerance.
 *
 * Consider time points closer than @tolerance_ms to targeted sample taking
 * time to be good enough for sample taking.
 */
const static int tolerance_ms = 200;

/*
 * Debugging output level.
 */
static int debug_level = 0;

/*
 * Maximum XenStore transaction retry attempts.
 * Used to retry begin_xs() and commit_xs().
 */
const static int max_xs_retries = 20;

/*
 * Limit data update interval to 600 seconds max
 */
const static int max_interval = 600;

/*
 * When starting up as a daemon and Xen has not fully completed its initialization
 * yet, wait up to @max_xen_init_retries seconds for Xen to initialize before
 * giving up, and log message about waiting after @xen_init_retry_msg seconds.
 */
const static int max_xen_init_retries = 300;
const static int xen_init_retry_msg = 15;

/*
 * If @true, enable command line option to simulate data rate and       		      .
 * free system memory percentage.
 * Set @true only for development, shut off in release build.
 */
static const bool enable_simulation = IF_DEVEL_ELSE(true, false);


/******************************************************************************
*                              static data                                    *
******************************************************************************/

static int fd_sighup = -1;      	  /* handle for SIGHUP */
static int fd_sigterm = -1;     	  /* handle for SIGTERM */
static clockid_t clk_id;		  /* clock to use for timing (CLOCK_BOOTTIME etc.) */
static char *vmstat = NULL;     	  /* buffer for reading /proc/vmstat */
static size_t vmstat_size = 0;  	  /* ... */

// static long page_size;		  /* system page size */

static struct xs_handle *xs = NULL;        		/* xenstore connection */
static xs_transaction_t xst = XS_TRANSACTION_NULL;      /* xenstore transaction */

static unsigned long long report_seq = 1;		/* seq# of report to membalance */

// static char vm_uuid[UUID_STRING_SIZE]; /* uuid of local VM */

static bool subscribed_membalance = false;  /* subscribed to watching @membalance_interval_path */

static bool need_update_membalance_settings = false;  	/* need to call update_membalance_settings() soon */

static FILE *log_fp = NULL;    		  /* file for logging (normally stderr or NULL) */

static bool log_syslog = false; 	  /* log to syslog */

static bool initialized_xs = false;       /* xenstore connection initialized */

static bool run_as_daemon = false;	  /* run as a daemon */

/*
 * Path in xenstore for report update interval.
 * Created and written by MEMBALANCED, read only by MEMPROBED.
 */
static const char membalance_interval_path[] = "/tool/membalance/interval";

/*
 * Path in xenstore that points to report key location.
 * Created by MEMBALANCED, read by MEMPROBED.
 * Relative to domain path (/local/domain/<domid>).
 */
static const char membalance_report_link_path[] = "membalance/report_path";

/*
 * Path in xenstore to report this domain's data map-in rate to.
 * Created and read by MEMBALANCED, written by MEMPROBED.
 * The value comes from @membalance_report_link_path.
 */
static char* membalance_report_path = NULL;

/*
 * Simulated data rate (if >=0, overrides actual rate in the report)
 */
static int64_t simulated_kbs = -1;

/*
 * Simulated free memory percentage (if >=0, overrides actual data in the report)
 */
static double simulated_free_pct = -1;


/******************************************************************************
*                          forward declarations                               *
******************************************************************************/

static void handle_cmdline(int argc, char **argv);
static void ivarg(const char *arg)  __noreturn__;
static void daemonize(void);
static void select_clk_id(void);
static void handle_signals(struct pollfd *pollfds, bool *p_recheck_time);
static struct timespec getnow(void);
static int64_t timespec_diff_ms(const struct timespec ts1, const struct timespec ts0);
static void fatal_perror(const char *fmt, ...) __noreturn__ __format_printf__;
static void fatal_msg(const char *fmt, ...) __noreturn__ __format_printf__;
static void error_perror(const char *fmt, ...) __format_printf__;
static void error_msg(const char *fmt, ...) __format_printf__;
static void notice_msg(const char *fmt, ...) __format_printf__;
static void out_of_memory(void) __noreturn__;
static void terminate(void) __noreturn__;
static void get_paging_data(struct paging_data *pd);
static void read_whole_file(const char *path, char **ppbuf, size_t *psize, size_t initial_size);
static bool is_key(char **pp, const char *key);
static bool consume_umax(char **pp, uintmax_t *pval);
static void process_sample(const struct paging_data *pd1, const struct paging_data *pd0);
static void shutdown_xs(void);
static bool begin_xs(void);
static void abort_xs(void);
static XsTransactionStatus commit_xs(int *p_nretries);
static bool begin_singleop_xs(void);
static void abort_singleop_xs(void);
static XsTransactionStatus commit_singleop_xs(int* p_nretries);
static void retry_wait(int cycle);
static void verify_hypervisor(void);
static void initialize_xs(struct pollfd *pollfds);
static void open_xs_connection(struct pollfd *pollfds);
static void try_subscribe_membalance_settings(void);
static void update_membalance_settings(void);
static void handle_xs_watch(void);


/******************************************************************************
*                             main routines                                   *
******************************************************************************/

static void usage(int exitcode)
{
	FILE *fp = stderr;
	fprintf(fp, "Usage:\n");
	fprintf(fp, "    --daemon           run as a daemon\n");
	fprintf(fp, "    --version          print version (%s %s)\n", progname, progversion);
	fprintf(fp, "    --help             print this text\n");
	fprintf(fp, "    --debug-level <n>  set debug level\n");
	if (enable_simulation)
	{
		fprintf(fp, "    --fake-rate <n>    simulate rate of <n> KB/sec\n");
		fprintf(fp, "    --fake-free <n>    simulate free memory <n>%%\n");
	}
	exit(exitcode);
}

int main(int argc, char **argv)
{
	sigset_t sigmask;
	struct pollfd pollfds[NPFD_COUNT];
	int npollfds;
	int k;
	int64_t wait_ms;
	struct paging_data pd0;
	struct paging_data pd1;
	bool recheck_time;
	int init_pass = 0;

	/*
	 * if running interactively, output initially to the terminal,
	 * otherwise to syslog right away
	 */
	if (isatty(fileno(stderr)))
	{
		log_fp = stderr;
		log_syslog = false;
	}
	else
	{
		log_fp = NULL;
		log_syslog = true;
	}

	if (log_syslog)
		openlog(progname, LOG_CONS | LOG_PID, LOG_DAEMON);

	/* parse arguments */
	handle_cmdline(argc, argv);

	if (run_as_daemon)
		daemonize();

	debug_msg(1, "debug level set to %d", debug_level);

	// page_size = sysconf(_SC_PAGESIZE);
	// if (page_size <= 0)
	//	fatal_msg("unable to get system page size");

	/*
	 * Block signals so that they aren't handled according to their
	 * default dispositions
	 */
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGHUP);
	sigaddset(&sigmask, SIGTERM);
        if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0)
		fatal_perror("sigprocmask");

	/*
	 * Receive signals on file descriptors
	 */
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGHUP);
	fd_sighup = signalfd(-1, &sigmask, 0);
	if (fd_sighup < 0)
		fatal_perror("signalfd(SIGHUP)");

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGTERM);
	fd_sigterm = signalfd(-1, &sigmask, 0);
	if (fd_sigterm < 0)
		fatal_perror("signalfd(SIGTERM)");

	/* select clock to use */
	select_clk_id();

	/* verify we are running under a hypervisor */
	verify_hypervisor();

	pollfds[NPFD_SIGTERM].fd = fd_sigterm;
	pollfds[NPFD_SIGTERM].events = POLLIN|POLLPRI;

	pollfds[NPFD_SIGHUP].fd = fd_sighup;
	pollfds[NPFD_SIGHUP].events = POLLIN|POLLPRI;

	/* keep xs poll fd last in the array */
	pollfds[NPFD_XS].fd = -1;
	pollfds[NPFD_XS].events = POLLIN|POLLPRI;

	/* initialize xenstore structure */
	initialize_xs(pollfds);
	pollfds[NPFD_XS].fd = xs_fileno(xs);

	get_paging_data(&pd0);

        for (;;)
	{
		try_subscribe_membalance_settings();

		/* calculate sleep time till next sample point */
		if (initialized_xs)
		{
			wait_ms = timespec_diff_ms(pd0.ts, getnow()) + interval * MSEC_PER_SEC;
		}
		else
		{
			/* if xs not initialzied, keep retrying */
			wait_ms = interval * MSEC_PER_SEC;
			if (++init_pass > 30)
				wait_ms *= 2;
		}

		if (wait_ms >= tolerance_ms)
		{
			for (k = 0;  k < countof(pollfds);  k++)
				pollfds[k].revents = 0;

			/* if settings need to be updated, retry in 1 sec or sooner */
			if (need_update_membalance_settings)
				wait_ms = min(wait_ms, MSEC_PER_SEC);

			/* include xs in the poll only if has initialized xs */
			npollfds = countof(pollfds);
			if (!initialized_xs)
				npollfds--;

			if (poll(pollfds, npollfds, wait_ms) < 0)
			{
				if (errno == EINTR)
					continue;
				fatal_perror("poll");
			}

			recheck_time = false;

			handle_signals(pollfds, &recheck_time);

			if (!initialized_xs)
			{
				/* try to initialzie xs */
				initialize_xs(NULL);
				/* if successful, start monitoring page map-in rate */
				if (initialized_xs)
					get_paging_data(&pd0);
				continue;
			}

			if (pollfds[NPFD_XS].revents & (POLLIN|POLLPRI))
			{
				/* a watched value in xenstore has been changed */
				handle_xs_watch();
				recheck_time = true;
			}

			if (need_update_membalance_settings)
				update_membalance_settings();

			if (recheck_time)
			{
				/* go sleep again if wait interval has not expired yet */
				if (timespec_diff_ms(getnow(), pd0.ts) < interval * MSEC_PER_SEC - tolerance_ms)
					continue;
			}
		}

		if (!initialized_xs)
		{
			/* try to initialzie xs */
			initialize_xs(NULL);
			/* if successful, start monitoring page map-in rate */
			if (initialized_xs)
				get_paging_data(&pd0);
			continue;
		}

		/* process sample and set it as the new "previous" one */
		get_paging_data(&pd1);
		process_sample(&pd1, &pd0);
		pd0 = pd1;
	}

	/* close connection to xenstore */
	shutdown_xs();

	return EXIT_SUCCESS;
}

/*
 * Parse command line arguments
 */
static void handle_cmdline(int argc, char **argv)
{
	int k;
	char *cp, *ep = NULL;
	long val;

	for (k = 1;  k < argc;  k++)
	{
		if (0 == strcmp(argv[k], "--daemon"))
		{
			run_as_daemon = true;
		}
		else if (0 == strcmp(argv[k], "--version"))
		{
			printf("%s version %s\n", progname, progversion);
			exit(EXIT_SUCCESS);
		}
		else if (0 == strcmp(argv[k], "--help"))
		{
			usage(EXIT_SUCCESS);
		}
		else if (0 == strcmp(argv[k], "--debug-level"))
		{
			if (k == argc - 1)
				ivarg(argv[k]);
			cp = argv[++k];
			errno = 0;
			val = strtol(cp, &ep, 10);
			if (errno || ep == cp || *ep || val < 0 || val > INT_MAX)
				ivarg(cp);
			debug_level = (int) val;
		}
		else if (enable_simulation && 0 == strcmp(argv[k], "--fake-rate"))
		{
			if (k == argc - 1)
				ivarg(argv[k]);
			cp = argv[++k];
			errno = 0;
			val = strtol(cp, &ep, 10);
			if (errno || ep == cp || *ep || val < 0)
				ivarg(cp);
			simulated_kbs = val;
		}
		else if (enable_simulation && 0 == strcmp(argv[k], "--fake-free"))
		{
			if (k == argc - 1)
				ivarg(argv[k]);
			cp = argv[++k];
			errno = 0;
			val = strtol(cp, &ep, 10);
			if (errno || ep == cp || *ep || val < 0 || val > 100)
				ivarg(cp);
			simulated_free_pct = (double) val;
		}
		else
		{
			ivarg(argv[k]);
		}
	}
}

/*
 * Print "invalid argument" message and exit
 */
static void ivarg(const char *arg)  /* __noreturn__ */
{
	if (log_fp)
		fprintf(log_fp, "%s: invalid argument: %s (use --help)\n", progname, arg);
	if (log_syslog)
		syslog(LOG_ERR, "%s: invalid argument: %s (use --help)", progname, arg);
	exit(EXIT_FAILURE);
}

/*
 * Fork, exit parent, daemonize child process
 */
static void daemonize(void)
{
	int fd, nullfd;
	long fd_max;
	int sig;
	sigset_t sigmask;

	/*
	 * reset all signal handlers to their default,
	 * also reset blocked signal mask
	 */
	sigemptyset(&sigmask);
	for (sig = 1;  sig <= _NSIG;  sig++)
	{
		signal(sig, SIG_DFL);
		sigaddset(&sigmask, sig);
	}
	if (sigprocmask(SIG_UNBLOCK, &sigmask, NULL) < 0)
		fatal_perror("sigprocmask");

	switch (fork()) {
	case -1:
		fatal_perror("fork");
	case 0:
		/* child */
		break;
	default:
		/* parent */
		exit(EXIT_SUCCESS);
	}

	/* at this point we are executing as the child process */

	/* disconnect */
	if (setsid() < 0)
		fatal_perror("setsid");

	/* change the file mode mask */
	umask(0);

	/* close extra file handles */
	if (log_syslog)
	{
		closelog();
		log_syslog = false;
	}

	if (log_fp != stderr)
		fclose(log_fp);
	log_fp = NULL;

	fd_max = sysconf(_SC_OPEN_MAX);
	for (fd = 3;  fd < fd_max;  fd++)
		close(fd);

	/* switch error reporting to syslog */
	openlog(progname, LOG_CONS | LOG_PID, LOG_DAEMON);
	log_syslog = true;

	/* release current directory */
	if (chdir("/") < 0)
		fatal_perror("chdir to root");

	/* open /dev/null */
	nullfd = open("/dev/null", O_RDWR);
	if (nullfd < 0)
		fatal_perror("cannot open /dev/null");

	/* set up stdin, stdout, stderr to /dev/null */
	if (dup2(nullfd, STDIN_FILENO) != STDIN_FILENO ||
	    dup2(nullfd, STDOUT_FILENO) != STDOUT_FILENO ||
	    dup2(nullfd, STDERR_FILENO) != STDERR_FILENO)
	{
		fatal_perror("dup2");
	}

	close(nullfd);

	/*
	 * fork once more to ensure the daemon can never reacquire
	 * a terminal again
	 */
	switch (fork())
	{
	case -1:
		fatal_perror("fork");
	case 0:
		/* child: at this point re-parented to init (ppid=1) */
		break;
	default:
		/* parent */
		exit(EXIT_SUCCESS);
	}
}

/*
 * Abort current xenstore transaction if any
 * and shutdown connection to xenstore
 */
static void shutdown_xs(void)
{
	abort_xs();

	if (xs != NULL)
	{
		xs_close(xs);
		xs = NULL;
	}
}

/*
 * Start XenStore transaction.
 * If successful, return @true.
 * On error, return @false and log error message.     		      .
 */
static bool begin_xs(void)
{
	int nretries = 0;

	if (xst != XS_TRANSACTION_NULL)
		fatal_msg("bug: begin_xs inside a transaction");

	for (;;)
	{
		xst = xs_transaction_start(xs);
		if (xst != XS_TRANSACTION_NULL)
			break;

		/*
		 * ENOSPC means too many concurrent transactions from the domain
		 */
		if (errno == ENOSPC)
		{
			if (++nretries > max_xs_retries)
			{
				error_perror("unable to start xenstore transaction (retry limit exceeded)");
				break;
			}

			retry_wait(nretries);
			debug_msg(2, "retrying to start xenstore transaction");
		}
		else
		{
			error_perror("unable to start xenstore transaction");
			break;
		}
	}

	if (xst != XS_TRANSACTION_NULL)
	{
		debug_msg(15, "started transaction");
		return true;
	}
	else
	{
		return false;
	}
}


/*
 * Abort current xenstore transaction if any
 */
static void abort_xs(void)
{
	if (xst != XS_TRANSACTION_NULL)
	{
		xs_transaction_end(xs, xst, TRUE);
		xst = XS_TRANSACTION_NULL;
		debug_msg(15, "aborted transaction");
	}
}

/*
 * Commit current xenstore transaction.
 *
 * Returns:
 * 	XSTS_OK	       success
 * 	XSTS_RETRY     retry the transaction
 *	XSTS_NORETRY   out of retry limit (message logged)
 *	XSTS_FAIL      commit failed (message logged)
 */
static XsTransactionStatus commit_xs(int* p_nretries)
{
	if (xst == XS_TRANSACTION_NULL)
		fatal_msg("bug: commit_xs outside of a transaction");

	if (xs_transaction_end(xs, xst, FALSE))
	{
		debug_msg(15, "committed transaction");
		xst = XS_TRANSACTION_NULL;
		return XSTS_OK;
	}

	xst = XS_TRANSACTION_NULL;

	if (errno == EAGAIN)
	{
		if (++(*p_nretries) > max_xs_retries)
		{
			error_perror("unable to write xenstore (transaction retry limit exceeded)");
			return XSTS_NORETRY;
		}

		retry_wait(*p_nretries);

		debug_msg(2, "restarting xenstore transaction");
		return XSTS_RETRY;
	}
	else
	{
		error_perror("unable to write xenstore (xs_transaction_end)");
		return XSTS_FAIL;
	}
}

/*
 * Begin a pseudo-transaction that involves just one read or write operation
 */
static bool begin_singleop_xs(void)
{
	if (xst != XS_TRANSACTION_NULL)
		fatal_msg("bug: begin_singleop_xs inside a transaction");
	return true;
}

/*
 * Abort a pseudo-transaction that involves just one read or write operation
 */
static void abort_singleop_xs(void)
{
	if (xst != XS_TRANSACTION_NULL)
		fatal_msg("bug: abort_singleop_xs inside a real transaction");
}

/*
 * Commit a pseudo-transaction that involves just one read or write operation
 */
static XsTransactionStatus commit_singleop_xs(int* p_nretries)
{
	if (xst != XS_TRANSACTION_NULL)
		fatal_msg("bug: commit_singleop_xs inside a real transaction");
	return XSTS_OK;
}

/*
 * Backoff time till next retry cycle.
 */
static void retry_wait(int cycle)
{
	if (cycle < max_xs_retries / 2)
		return;

	/*
	 * Increase pre-retry wait time linearly from 20ms to 5 sec between
	 * attempt [max_xs_retries / 2] and [max_xs_retries].
	 *
	 * If contention ever gets really high and deemed to cause a problem,
	 * can implement randomized truncated exponential back-off.
	 */
	const double w1 = 20;
	const double w2 = 5 * MSEC_PER_SEC;
	const int n1 = max_xs_retries / 2;
	const int n2 = max_xs_retries;

	double w = w1 + (cycle - n1) * (w2 - w1) / (n2 - n1);

	usleep((int) (USEC_PER_MSEC * w));
}

/*
 * Process expected signals
 */
static void handle_signals(struct pollfd *pollfds, bool *p_recheck_time)
{
	struct signalfd_siginfo fdsi;
	ssize_t sz;

	/* SIGTERM - exit gracefully */
	if (pollfds[NPFD_SIGTERM].revents & (POLLIN|POLLPRI))
	{
		shutdown_xs();
		notice_msg("terminating...");
		exit(EXIT_SUCCESS);
	}

	/* SIGHUP - reload configuration */
	if (pollfds[NPFD_SIGHUP].revents & (POLLIN|POLLPRI))
	{
		sz = read(fd_sighup, &fdsi, sizeof(fdsi));
		if (sz < 0)
			fatal_perror("read signalfd");
		if (sz != sizeof(fdsi))
			fatal_msg("read signalfd");
		/* reload configuration -- currently none for memprobed */
		if (p_recheck_time)
			*p_recheck_time = true;
	}
}

/*
 * Initialize xenstore structure.
 * May have to be called multiple times.
 *
 * On the very first call always opens xs connection.
 * Aborts if unable to open xs connection in a reasonable time.
 * Always returns the very first time with xs != null.
 *
 * Also tries to initialize other xs structures.
 * If successful, returns with @initialized_xs = true.
 * If unsuccessful, returns with @initialized_xs = false and must be called
 * again later to retry the initialization.
 *
 * @pollfds defines of signal-handling descriptors to be watched if has to wait
 * during the initialization. Better be non-NULL on the first call, but can be
 * left NULL on subsequent calls (as they do not wait long).
 *
 * Will not touch pollfds[NPFD_XS].
 */
static void initialize_xs(struct pollfd *pollfds)
{
	XsTransactionStatus status;
	char *keyvalue = NULL;
	unsigned int len;
	int nretries;

	/* already initialized? */
	if (initialized_xs)
		return;

	/* open a connection to xenstore*/
	open_xs_connection(pollfds);

	/*
	 * Try to reset the report key.
	 */
	for (nretries = 0; ;)
	{
		/*
		 * Wait until xenstore "membalance/report_path" key and the key it points
		 * to have been created by membalanced with appropriate access permissions.
		 *
		 * First try without actual transaction, to make the cost for
		 * xenstore cheaper.
		 */
		if (membalance_report_path == NULL)
		{
			CHECK(begin_singleop_xs());

			keyvalue = xs_read(xs, xst, membalance_report_link_path, &len);
			if (keyvalue == NULL)
			{
				if (errno != ENOENT)
					error_perror("cannot read xenstore (%s)", membalance_report_link_path);
				debug_msg(3, "%s does not exist yet", membalance_report_link_path);
				/*
				 * Leave initialzed_xs = false, so will retry later
				 */
				abort_singleop_xs();
				return;
			}

			membalance_report_path = keyvalue;
			abort_singleop_xs();
		}

		if (nretries == 0)
		{
			CHECK(begin_singleop_xs());

			keyvalue = xs_read(xs, xst, membalance_report_path, &len);
			if (keyvalue == NULL)
			{
				if (errno != ENOENT)
					error_perror("cannot read xenstore (%s)", membalance_report_path);
				debug_msg(3, "%s does not exist yet", membalance_report_path);
				/*
				 * Leave initialzed_xs = false, so will retry later
				 */
				abort_singleop_xs();
				return;
			}

			free(keyvalue);
			abort_singleop_xs();
		}

		/*
		 * Wait until a key for data reporting has been created in
		 * xenstore by membalanced with appropriate access permissions.
		 *
		 * Now within a real transaction.
		 */
		CHECK(begin_xs());

		keyvalue = xs_read(xs, xst, membalance_report_path, &len);
		if (keyvalue == NULL)
		{
			if (errno != ENOENT)
				error_perror("cannot read xenstore (%s)", membalance_report_path);
			debug_msg(3, "%s does not exist yet", membalance_report_path);
			/*
			 * Leave initialzed_xs = false, so will retry later
			 */
			abort_xs();
			return;
		}

		free(keyvalue);

		PCHECK(xs_write(xs, xst, membalance_report_path, "", strlen("")),
		       "cannot write to xenstore");

		status = commit_xs(&nretries);

		switch (status)
		{
		case XSTS_OK:       break;	    /* done */
		case XSTS_RETRY:    continue;	    /* keep retrying */
		case XSTS_NORETRY:  CHECK(false);   /* abort */
		case XSTS_FAIL:     CHECK(false);   /* abort */
		}

		break;
	}

	initialized_xs = true;

	try_subscribe_membalance_settings();

	return;

cleanup:

	terminate();
}

/*
 * Open connection to xenstore.
 *
 * @pollfds defines of signal-handling descriptors to be watched
 * if has to wait.
 *
 * Will not touch pollfds[NPFD_XS].
 */
static void open_xs_connection(struct pollfd *pollfds)
{
	struct timespec ts0;
	int64_t wsec;
	bool displayed_msg = false;
	int k;

	/*
	 * If we are running as a daemon during system startup, there
	 * can be a race condition with continuing Xen initialization
	 * (/proc/xen including /proc/xen/privcmd may be unavailable yet,
	 * still to be created even though Xen service startup completion
	 * has already been signalled), so wait for a while if necessary.
	 */
	for (ts0 = getnow();;)
	{
		if (!xs)
			xs = xs_open(0);
		if (xs)
			break;

		/* time since the start */
		wsec = timespec_diff_ms(getnow(), ts0) / MSEC_PER_SEC;

		if (run_as_daemon && wsec < max_xen_init_retries)
		{
			if (wsec >= xen_init_retry_msg && !displayed_msg)
			{
				notice_msg("waiting for Xen to initialize");
				displayed_msg = true;
			}

			if (pollfds != NULL)
			{
				int npollfds = NPFD_COUNT - 1;
				for (k = 0;  k < npollfds;  k++)
					pollfds[k].revents = 0;
				if (poll(pollfds, npollfds, 1 * MSEC_PER_SEC) < 0)
					fatal_perror("poll");
				handle_signals(pollfds, NULL);
			}
			else
			{
				sleep(1);
			}
		}
		else
		{
			fatal_perror("unable to open connection to xenstore");
		}
	}
}

/*
 * Try to subscribe to receiving notification of membalance settings updates
 */
static void try_subscribe_membalance_settings(void)
{
	if (!initialized_xs || subscribed_membalance)
		return;

	/*
	 * @membalance_interval_path may not exist or be accessible yet.
	 * It takes time for membalanced to enable new VM access rights
	 * to the key after a new VM gets started.
	 */
	if (!xs_watch(xs, membalance_interval_path, membalance_interval_path))
		return;

	subscribed_membalance = true;
	debug_msg(2, "subscribed membalance settings");

	update_membalance_settings();
}

/*
 * Read and apply updated membalance settings
 */
static void update_membalance_settings(void)
{
	char *pv = NULL;
	unsigned int len;
	long v;
	char* ep = NULL;

	if (!subscribed_membalance)
		return;

	pv = xs_read(xs, xst, membalance_interval_path, &len);
	if (!pv)
	{
		/*
		 * In the event of a temporary glitch, schedule to read the
		 * settings a little later. Could be a temporary shortage
		 * of resources, or membalanced did not yet add this VM to the
		 * permission list on the key.
		 */
		error_perror("unable to read xenstore (%s)", membalance_interval_path);
		need_update_membalance_settings = true;
		return;
	}

	need_update_membalance_settings = false;

	errno = 0;
	v = strtol(pv, &ep, 10);
	if (errno || ep == pv || *ep || v <= 0)
	{
		error_msg("ignoring invalid refresh interval setting");
		goto cleanup;
	}

	/* sanity check */
	if (v > max_interval)
	{
		error_msg("restricting value of refresh interval to %d seconds", max_interval);
		v = max_interval;
	}

	interval = v;

	debug_msg(2, "updated interval to %d seconds", interval);

cleanup:

	if (pv)
		free(pv);
}

/*
 * Called when a watched value in xenstore has been changed
 */
static void handle_xs_watch(void)
{
	char *path, *token, **vec;
	unsigned int k, num;

	vec = xs_read_watch(xs, &num);

	if (vec == NULL)
	{
		/* we currently watch only for one thing,
		   so check it anyway */
		update_membalance_settings();
		return;
	}

	for (k = 0;  k < num;  k += 2)
	{
		path  = vec[k + XS_WATCH_PATH];
		token = vec[k + XS_WATCH_TOKEN];
		if (0 == strcmp(path, membalance_interval_path))
			update_membalance_settings();
	}

	free(vec);
}

/*
 * Read memory statistics data
 */
static void get_paging_data(struct paging_data *pd)
{
	static const char *key_pgpgin = "pgpgin";
	static const char *key_nr_free_pages = "nr_free_pages";
	static const char parse_error_msg[] = "error parsing /proc/vmstat";
	char *cp, *xp;
	bool found_pgpgin = false;
	bool found_nr_free_pages = false;
	int nfound = 0;
	const int need_found = 2;

	pd->ts = getnow();

	pd->mem_pages = sysconf(_SC_PHYS_PAGES);
	if (pd->mem_pages <= 0)
		fatal_msg("unable to get system memory size");

	read_whole_file("/proc/vmstat", &vmstat, &vmstat_size, 4096);

	for (cp = vmstat; *cp != '\0';)
	{
		if (is_key(&cp, key_pgpgin))
		{
			if (!consume_umax(&cp, &pd->pgpgin))
				fatal_msg(parse_error_msg);

			if (!found_pgpgin)
			{
				found_pgpgin = true;
				if (need_found == ++nfound)
					break;
			}
		}
		else if (is_key(&cp, key_nr_free_pages))
		{
			if (!consume_umax(&cp, &pd->nr_free_pages))
				fatal_msg(parse_error_msg);

			if (!found_nr_free_pages)
			{
				found_nr_free_pages = true;
				if (need_found == ++nfound)
					break;
			}
		}
		else
		{
			xp = strchr(cp, '\n');
			if (!xp) break;
			cp = xp + 1;
		}
	}

	if (need_found != nfound)
		fatal_msg(parse_error_msg);
}

/*
 * Check if *@pp points to the line with specified @key.
 * If not, return false and leave *@pp unaltered.
 * If yes, return true and advance *@pp past the key and the space after it.
 */
static bool is_key(char **pp, const char *key)
{
	char *p = *pp;

	while (*key)
	{
		if (*p++ != *key++)
			return false;
	}

	if (!isblank(*p))
		return false;
	while (isblank(*p))
		p++;

	*pp = p;

	return true;
}


/*
 * Check if *@pp points a number optionally followed by spaces and nl.
 * If not, return false and *@pp is unaltered.
 * If yes, parse the number into *@pval, return true and advance *@pp   				.
 * past the number and optional spaces and nl after it.
 */
static bool consume_umax(char **pp, uintmax_t *pval)
{
	char *p = *pp;
	uintmax_t val;

	if (!isdigit(*p))
		return false;
	errno = 0;
	val = strtoumax(p, &p, 10);
	if (errno || p == *pp)
		return false;

	while (isblank(*p))
		p++;

	if (*p == '\n')
	{
		*pp = p + 1;
		*pval = val;
		return true;
	}
	else if (*p == '\0')
	{
		*pp = p;
		*pval = val;
		return true;
	}
	else
	{
		return false;
	}
}

/*
 * Process next sample of memory statistics data taken after another @interval.
 * @pd1 = current reading
 * @pd0 = reading taken @interval ago
 */
static void process_sample(const struct paging_data *pd1, const struct paging_data *pd0)
{
	int64_t ms;
	int64_t kbs, kbs_sec;
	double free_pct;
	char report[512];
	char struct_version = 'A';
	int nretries = 0;

	/* discard sample if weird timing */
	ms = timespec_diff_ms(pd1->ts, pd0->ts);
	if (ms < tolerance_ms || ms > interval * MSEC_PER_SEC * 10)
		return;

	/* discard the sample if weird data */
	kbs = (int64_t) (pd1->pgpgin - pd0->pgpgin);
	if (kbs < 0)
		return;
	kbs_sec = (kbs * MSEC_PER_SEC) / ms;

	free_pct = 100.0 * (double) pd1->nr_free_pages / (double) pd1->mem_pages;

	/* manual override (development time only) */
	if (simulated_kbs >= 0)
		kbs_sec = simulated_kbs;

	if (simulated_free_pct >= 0)
		free_pct = simulated_free_pct;

	if (log_fp && debug_level >= 3)
	{
		fprintf(log_fp, "pgpgin=%llu, nr_free_pages=%llu, kbs=%lu, kbs_sec=%lu, free%%=%f\n",
			(unsigned long long) pd1->pgpgin,
			(unsigned long long) pd1->nr_free_pages,
			(unsigned long) kbs,
			(unsigned long) kbs_sec,
			free_pct);
	}

	/* format memory pressure report string */
	/* (could use json if data structure were more complicated and changeable) */
	sprintf(report, "%c\n"
			"action: report\n"
			"progname: %s\n"
			"progversion: %s\n"
			"seq: %llu\n"
			"kb: %lu\n"
			"kbsec: %lu\n"
			"freepct: %f\n",
		struct_version,
		progname,
		progversion,
		report_seq++,
		(unsigned long) kbs,
		(unsigned long) kbs_sec,
		free_pct);

	/* write report data to xenstore */
	for (;;)
	{
		if (!begin_singleop_xs())
			break;

		if (!xs_write(xs, xst, membalance_report_path, report, strlen(report)))
		{
			error_perror("unable to write xenstore");
			abort_singleop_xs();
			break;
		}

		if (commit_singleop_xs(&nretries) != XSTS_RETRY)
			break;
	}
}

/*
 * Verify we are running under Xen,
 * and not in the root domain
 */
static void verify_hypervisor(void)
{
	FILE *fp = NULL;
	char buf[256];
	char *p;
	const static char root_uuid[] = "00000000-0000-0000-0000-000000000000";
	const char *rp;

	/*
	 * Check that file /sys/hypervisor/type is present
	 * and contains "xen"
	 */
	if (access("/sys/hypervisor/type", F_OK) == -1 &&
	    (errno == ENOENT || errno == ENOTDIR))
		fatal_msg("not running under a hypervisor");

	fp = fopen("/sys/hypervisor/type", "r");
	if (fp == NULL || NULL == fgets(buf, countof(buf), fp))
		fatal_msg("unable to read /sys/hypervisor/type");

	buf[countof(buf) - 1] = '\0';
	p = strchr(buf, '\n');
	if (p)  *p = '\0';
	if (0 != strcasecmp(buf, "xen"))
		fatal_msg("hypervisor is not Xen");

	fclose(fp);

	/*
	 * Read Xen domain UUID from /sys/hypervisor/uuid
	 * and check it is not the root domain
	 */
	fp = fopen("/sys/hypervisor/uuid", "r");
	if (fp == NULL || NULL == fgets(buf, countof(buf), fp))
		fatal_msg("unable to read /sys/hypervisor/uuid");

	buf[countof(buf) - 1] = '\0';
	p = strchr(buf, '\n');
	if (p)  *p = '\0';
	if (0 == strcmp(buf, root_uuid))
		fatal_msg("may run only in a VM, not root domain");

	fclose(fp);

	if (strlen(buf) != strlen(root_uuid) /* ||
	    strlen(buf) != UUID_STRING_SIZE - 1 */)
		fatal_msg("unexpected domain uuid");

	for (p = buf, rp = root_uuid; *rp;  p++, rp++)
	{
		if (*rp == '-' && *p == '-')  continue;
		if (isxdigit(*rp) && isxdigit(*p))  continue;
		fatal_msg("unexpected domain uuid");
	}

	// strcpy(vm_uuid, buf);
}

/*
 * Select preferred interval clock among available ones
 */
static void select_clk_id(void)
{
	struct timespec ts;

	clk_id = CLOCK_BOOTTIME;
	if (0 == clock_gettime(clk_id, &ts))
		return;

	clk_id = CLOCK_MONOTONIC_RAW;
	if (0 == clock_gettime(clk_id, &ts))
		return;

	clk_id = CLOCK_MONOTONIC;
	if (0 == clock_gettime(clk_id, &ts))
		return;

	fatal_msg("unable to select clk_id");
}


/*
 * Read in the whole content of file @path into the buffer pointed by *@ppbuf,
 * as nul-terminated string.
 * Reallocate *@ppbuf if necessary to be large enough.
 * The size of the buffer is kept in *@psize.
 * Initial size of the buffer to use on first allocation is given by @initial_size.
 * Abort on error.
 */
static void read_whole_file(const char *path, char **ppbuf, size_t *psize, size_t initial_size)
{
	ssize_t rsz;
	ssize_t rd;
	ssize_t rem;
	int fd = -1;

	if (*psize == 0)
	{
		*psize = initial_size;
		*ppbuf = malloc(*psize);
		if (!*ppbuf)
			out_of_memory();
	}

	for (;;)
	{
		if (fd == -1)
			close (fd);
		fd = open(path, O_RDONLY);
		if (fd == -1)
			fatal_perror("unable to open file %s", path);
		rd = 0;

		for (;;)
		{
			rem = *psize - rd;
			if (rem <= 0) break;
			rsz = read(fd, *ppbuf, rem);
			if (rsz < 0)
				fatal_perror("unable to read file %s", path);
			if (rsz == 0)  break;
			rd += rsz;
		}

		if (rd <= *psize - 1)
		{
			(*ppbuf)[rd] = '\0';
			break;
		}

		*psize += *psize / 2;
		*ppbuf = realloc(*ppbuf, *psize);
		if (!*ppbuf)
			out_of_memory();
	}

	close(fd);
}

/******************************************************************************
*                              utility routines                               *
******************************************************************************/

/*
 * Return (ts1 - ts0) in milliseconds
 */
static int64_t timespec_diff_ms(const struct timespec ts1, const struct timespec ts0)
{
	int64_t ms = (int64_t) ts1.tv_sec - (int64_t) ts0.tv_sec;
	ms *= MSEC_PER_SEC;
	ms += ((int64_t) ts1.tv_nsec - (int64_t) ts0.tv_nsec) / NSEC_PER_MSEC;
	return ms;
}

/*
 * Get current timestamp.
 * Preserves previous value of errno.
 */
static struct timespec getnow(void)
{
	int sv_errno = errno;
	struct timespec ts;

	if (clock_gettime(clk_id, &ts))
		fatal_perror("clock_gettime");

	errno = sv_errno;
	return ts;
}

/*
 * Log/print error message (incl. errno) and terminate
 */
static void fatal_perror(const char *fmt, ...) /* __noreturn__  __format_printf__ */
{
	char *errno_msg;
	char errno_msg_buf[128];
	char msg[512];
	int size = 0;
	va_list ap;

	errno_msg = strerror_r(errno, errno_msg_buf, countof(errno_msg_buf));

	strcpy(msg, "fatal error: ");

	size = strlen(msg);
	va_start(ap, fmt);
	vsnprintf(msg + size, countof(msg) - size, fmt, ap);
	va_end(ap);

	size = strlen(msg);
	snprintf(msg + size, countof(msg) - size, ": %s", errno_msg);

	fatal_msg("%s", msg);
}

/*
 * Log/print error message and terminate
 */
static void fatal_msg(const char *fmt, ...) /* __noreturn__  __format_printf__ */
{
	va_list ap;
	va_start(ap, fmt);

	if (log_fp)
	{
		vfprintf(log_fp, fmt, ap);
		fputc('\n', log_fp);
	}

	if (log_syslog)
		vsyslog(LOG_ERR, fmt, ap);

	va_end(ap);

	terminate();
}

/*
 * Log/print error message (incl. errno)
 */
static void error_perror(const char *fmt, ...)
{
	char *errno_msg;
	char errno_msg_buf[128];
	char msg[512];
	int size = 0;
	va_list ap;

	errno_msg = strerror_r(errno, errno_msg_buf, countof(errno_msg_buf));

	strcpy(msg, "error: ");

	size = strlen(msg);
	va_start(ap, fmt);
	vsnprintf(msg + size, countof(msg) - size, fmt, ap);
	va_end(ap);

	size = strlen(msg);
	snprintf(msg + size, countof(msg) - size, ": %s", errno_msg);

	error_msg("%s", msg);
}

/*
 * Log/print error message
 */
static void error_msg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);

	if (log_fp)
	{
		vfprintf(log_fp, fmt, ap);
		fputc('\n', log_fp);
	}

	if (log_syslog)
		vsyslog(LOG_ERR, fmt, ap);

	va_end(ap);
}

/*
 * Log/print notice message
 */
static void notice_msg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);

	if (log_fp)
	{
		vfprintf(log_fp, fmt, ap);
		fputc('\n', log_fp);
	}

	if (log_syslog)
		vsyslog(LOG_NOTICE, fmt, ap);

	va_end(ap);
}

/*
 * Log/print "out of memory" message and terminate
 */
static void out_of_memory(void) /* __noreturn__  __format_printf__ */
{
	fatal_msg("out of memory");
}

/*
 * Terminate, logging along "terminating" message
 */
static void terminate(void)  /* __noreturn__  __format_printf__ */
{
	if (log_syslog)
		syslog(LOG_ERR, "terminating");
	shutdown_xs();
	exit(EXIT_FAILURE);
}

