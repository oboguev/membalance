/*
 *  MEMBALANCE daemon
 *
 *  membalanced.cpp - entry point and daemon body
 *
 *     MEMBALANCED runs in the root Xen domain (Dom0) and listens to memory pressure
 *     notifications reported by instances of MEMPROBED running inside active VMs
 *     (DomU's). MEMBALANCED expands or shrinks memory allocated to DomU domains
 *     depending on their current demand and system administrator's control settings.
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

#define EXTERN_ALLOC		/* allocate globals in this module */
#include "membalanced.h"


/******************************************************************************
*                               static data                                   *
******************************************************************************/

static int fd_sighup = -1;      	  /* handle for SIGHUP */
static int fd_sigterm = -1;     	  /* handle for SIGTERM */
static int fd_sigusr1 = -1;     	  /* handle for SIGUSR1 */
static int fd_sigctrl = -1;     	  /* handle for SIG_CTRL */

static bool use_private_log = false;      /* log to @membalanced_log_path */

static FILE* private_log_fp = NULL;       /* handle for @membalanced_log_path */

static long page_size;		  	  /* system page size */

static bool resuming_memsched = false;    /* resuming to call memsched,
                                             possibly after a long sleep */

/******************************************************************************
*                           forward declarations                              *
******************************************************************************/

static int do_main_membalanced(int argc, char** argv);
static void handle_cmdline(int argc, char** argv);
static void ivarg(const char* arg)  __noreturn__;
static void usage(int exitcode)  __noreturn__;
static void daemonize(void);
static void shutdown(void);
static void verify_hypervisor(void);
static void realloc_pollfds(struct pollfd** pp, int* nalloc, int nreq);
static void runtime_asserts(void);
static void do_use_private_log(void);
static FILE* open_private_log(bool fatal);
static void one_instance(void);


/******************************************************************************
*                                 routines                                    *
******************************************************************************/

static void usage(int exitcode)
{
	FILE* fp = stderr;
	fprintf(fp, "Usage:\n");
	fprintf(fp, "    --daemon             run as a daemon\n");
	fprintf(fp, "    --version            print version (%s %s)\n", progname, progversion);
	fprintf(fp, "    --help               print this text\n");
	fprintf(fp, "    --debug-level <n>    set debug level\n");
	fprintf(fp, "    --log                instead of syslog, log to %s\n", membalanced_log_path);
	fprintf(fp, "    --no-log-timestamps  do not prefix log records with timestamps\n");
	exit(exitcode);
}

int main(int argc, char** argv)
{
	int excode = EXIT_FAILURE;
	const char* pname;
	const char* cp;

	runtime_asserts();

	/*
	 * This single executable hosts both membalanced and membalancectl.
	 * Get invoked program name.
	 */
	pname = argv[0];
	if (!pname)
		pname = "<unknown>";
	if (NULL != (cp = strrchr(pname, '/')))
		pname = cp + 1;

	if (streq(pname, "membalancectl"))
	{
		prog_membalancectl = true;
	}
	else if (streq(pname, "membalanced"))
	{
		prog_membalancectl = false;
	}
	else
	{
		fprintf(stderr,
			"error: unrecognized program name \"%s\", "
			"must be either membalancectl or membalanced\n",
			pname);
		exit(EXIT_FAILURE);
	}

	/*
	 * membalancectl:
	 *     output to stderr
	 *
	 * membalanced:
	 *     if running interactively, output initially to the terminal,
	 *     otherwise to syslog right away
	 */
	if (prog_membalancectl)
	{
		log_fp = stderr;
		log_syslog = false;
	}
	else if (isatty(fileno(stderr)))
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

	/*
	 * bracket real "main" into the handler to log and handle c++ exception
	 * information if an exception is ever thrown
	 */
	try
	{
		if (prog_membalancectl)
			excode = do_main_membalancectl(argc, argv);
		else
			excode = do_main_membalanced(argc, argv);
	}
	catch (std::bad_alloc &ex)
	{
		fatal_msg("out of memory");
	}
	catch(std::exception& ex)
	{
		const char* what = ex.what();
		if (!(what && *what))
			what = "unknown c++ exception";
		fatal_msg("error: c++ exception: %s", what);
	}
	catch (...)
	{
		fatal_msg("error: unknown c++ exception");
	}

	return excode;
}

static int do_main_membalanced(int argc, char** argv)
{
	sigset_t sigmask;
	struct pollfd* pollfds = NULL;
	int npollfds, nrpcs, npollfds_alloc = 0;
	int k;
	int64_t wait_ms = 0;
	bool wait_ms_valid = false;
	bool recheck_time;
	struct timespec ts0_sched;
	struct timespec ts0_pending;
	struct timespec now;

	/* reset configuration to defaults */
	config.defaults();
	default_config.defaults();

	/* parse arguments */
	handle_cmdline(argc, argv);

	load_configuration();

	if (run_as_daemon)
		daemonize();

	if (use_private_log)
		do_use_private_log();

	debug_msg(1, "debug level set to %d", debug_level);

	/*
	 * Block signals so that they aren't handled according to their
	 * default dispositions
	 */
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGHUP);
	sigaddset(&sigmask, SIGTERM);
	sigaddset(&sigmask, SIGUSR1);
	sigaddset(&sigmask, SIG_CTRL);
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

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGUSR1);
	fd_sigusr1 = signalfd(-1, &sigmask, 0);
	if (fd_sigusr1 < 0)
		fatal_perror("signalfd(SIGUSR1)");

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIG_CTRL);
	fd_sigctrl = signalfd(-1, &sigmask, 0);
	if (fd_sigctrl < 0)
		fatal_perror("signalfd(SIG_CTRL)");

	/* select clock to use */
	select_clk_id();

	/* verify we are running under a hypervisor */
	verify_hypervisor();

	/* verify no other instance is running */
	one_instance();

	/*
	 * calculate meaningful Xen domain memory allocation quant
	 * (assume one page, although Xen itself does an accounting
	 * in KB units, with granularity 1 KB)
	 */
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		fatal_perror("unable to get system page size");
	if (page_size % 1024)
		fatal_msg("system page size (%ld) is not multiple of 1024", page_size);
	memquant_kbs = pagesize_kbs = page_size / 1024;

	/* initial allocation of poll descriptors */
	realloc_pollfds(&pollfds, &npollfds_alloc, NPFD_COUNT + 10);

	pollfds[NPFD_SIGTERM].fd = fd_sigterm;
	pollfds[NPFD_SIGTERM].events = POLLIN|POLLPRI;

	pollfds[NPFD_SIGHUP].fd = fd_sighup;
	pollfds[NPFD_SIGHUP].events = POLLIN|POLLPRI;

	pollfds[NPFD_SIGUSR1].fd = fd_sigusr1;
	pollfds[NPFD_SIGUSR1].events = POLLIN|POLLPRI;

	pollfds[NPFD_SIG_CTRL].fd = fd_sigctrl;
	pollfds[NPFD_SIG_CTRL].events = POLLIN|POLLPRI;

	/* keep xs poll fd last in the array */
	pollfds[NPFD_XS].fd = -1;
	pollfds[NPFD_XS].events = POLLIN|POLLPRI;

	/* initialize xenstore structure */
	initialize_xl();
	initialize_xs(pollfds);
	pollfds[NPFD_XS].fd = xs_fileno(xs);
	config_eval_after_xl_init();

	/* start built-in RPC server for daemon management requests */
	start_rcmd_server();

	/* resresh membalance per-domain xenstore strucutres, and sync up doms.qid */
	resync_qid();

	/* initial scan of all local domains */
	enumerate_local_domains_as_pending();
	process_pending_domains();

	/* establish initial reference time points  */
	ts0_pending = ts0_sched = getnow();

	/*
	 * Main loop. Perform:
	 *
	 *     - memory scheduling for managed domains (every @interval seconds)
	 *     - processing of pending domains (every 1 second)
	 *     - handle signals (SIGTERM - exit, SIGHUP - reload config,
	 *                       SIGUSR1 - debug, SIG_CTRL - pause/resume)
	 *     - xenstore watch events (domain created/destroyed/changed)
	 *     - RPC requests to manage the daemon
	 */
	for (;;)
	{
		/* calculate sleep time till next processing point */
		if (!wait_ms_valid)
			wait_ms = calc_wait_ms(ts0_sched, ts0_pending);

		wait_ms_valid = false;
		recheck_time = false;

		if (wait_ms >= config.tolerance_ms)
		{
			for (k = 0;  k < NPFD_COUNT;  k++)
				pollfds[k].revents = 0;

			/* include RPC server handles */
			npollfds = NPFD_COUNT;
			nrpcs = rcmd_get_npollfds();
			realloc_pollfds(&pollfds, &npollfds_alloc, npollfds + nrpcs);
			rcmd_setup_pollfds(pollfds + npollfds);

			if (poll(pollfds, npollfds + nrpcs, wait_ms) < 0)
			{
				if (errno == EINTR)
					continue;
				fatal_perror("poll");
			}

			handle_signals(pollfds, &recheck_time);

			if (pollfds[NPFD_XS].revents & (POLLIN|POLLPRI))
			{
				/* a watched value in xenstore has been changed */
				handle_xs_watch();
				recheck_time = true;
			}

			if (rcmd_handle_pollfds(pollfds + npollfds))
				recheck_time = true;
		}

		/* if still too far away from scheduled processing, go sleep again */
		if (recheck_time)
		{
			wait_ms = calc_wait_ms(ts0_sched, ts0_pending);
			if (wait_ms >= config.tolerance_ms)
			{
				wait_ms_valid = true;
				continue;
			}
		}

		/*
		 * Any pending domains due to be processed?
		 */
		now = getnow();
		if (doms.pending.size() &&
		    timespec_diff_ms(now, ts0_pending) >=
		    1 * MSEC_PER_SEC - config.tolerance_ms)
		{
			process_pending_domains();
			ts0_pending = now = getnow();
		}

		/*
		 * Any managed domains due to be processed?
		 */
		if (doms.managed.size() &&
		    timespec_diff_ms(now, ts0_sched) >=
		        config.interval * MSEC_PER_SEC - config.tolerance_ms)
		{
			if (resuming_memsched)
				sched_slept(timespec_diff_ms(now, ts0_sched));
			sched_memory();
			ts0_sched = getnow();
			resuming_memsched = false;
		}
	}

	/* NOTREACHED */

	/* clean up */
	shutdown();

	return EXIT_SUCCESS;
}

/*
 * Called when a domain is added to the managed domains list
 */
void on_add_managed(void)
{
	if (doms.managed.size() == 1)
		resuming_memsched = true;
}

/*
 * Clean up on normal or error exit
 */
static void shutdown(void)
{
	/* close connection to xenstore */
	shutdown_xs();

	/* stop built-in RPC server */
	stop_rcmd_server();
}

/*
 * Calculate time (ms) till next scheduled work to do:
 * either domain memory allocation scheduling point,
 * or processing of "pending" domains (doms.pending).
 */
int64_t calc_wait_ms(const struct timespec& ts0_sched, const struct timespec& ts0_pending)
{
	struct timespec now = getnow();
	int64_t wms;

	/* if nothing to do, wake up once in a blue moon */
	int64_t wait_ms = 24 * 3600 * MSEC_PER_SEC;

	/* time till next scheduling point */
	if (doms.managed.size())
	{
		wms = timespec_diff_ms(ts0_sched, now) + config.interval * MSEC_PER_SEC;
		wait_ms = min(wait_ms, wms);
	}

	/* time till next "pending" processing point */
	if (doms.pending.size())
	{
		wms = timespec_diff_ms(ts0_pending, now) + 1 * MSEC_PER_SEC;
		wait_ms = min(wait_ms, wms);
	}

	if (wait_ms < 0)
		wait_ms = 0;

	return wait_ms;
}

/*
 * Parse command line arguments
 */
static void handle_cmdline(int argc, char** argv)
{
	int k;
	char* cp;
	long lv;

	for (k = 1;  k < argc;  k++)
	{
		if (streq(argv[k], "--daemon"))
		{
			run_as_daemon = true;
		}
		else if (streq(argv[k], "--version"))
		{
			printf("%s version %s\n", progname, progversion);
			exit(EXIT_SUCCESS);
		}
		else if (streq(argv[k], "--help"))
		{
			usage(EXIT_SUCCESS);
		}
		else if (streq(argv[k], "--debug-level"))
		{
			if (k == argc - 1)
				ivarg(argv[k]);
			cp = argv[++k];
			if (!a2long(cp, &lv) || lv < 0 || lv > INT_MAX)
				ivarg(cp);
			debug_level = (int) lv;
		}
		else if (streq(argv[k], "--log"))
		{
			use_private_log = true;
		}
		else if (streq(argv[k], "--no-log-timestamps"))
		{
			log_timestamps = false;
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
static void ivarg(const char* arg)  /* __noreturn__ */
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
	int fd, nullfd, logfd;
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

	switch (fork())
	{
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

	/* open membalanced log */
	logfd = open(membalanced_log_path, O_CREAT | O_APPEND | O_RDWR, S_IRWXU);
	if (logfd < 0)
		fatal_perror("cannot open %s", membalanced_log_path);

	/*
	 * set up stdin to /dev/null,
	 * stdout, stderr to membalanced log
	 */
	if (dup2(nullfd, STDIN_FILENO) != STDIN_FILENO ||
	    dup2(logfd, STDOUT_FILENO) != STDOUT_FILENO ||
	    dup2(logfd, STDERR_FILENO) != STDERR_FILENO)
	{
		fatal_perror("dup2");
	}

	close(nullfd);
	close(logfd);

	/* flush stdout and stderr right away after every nl */
	setlinebuf(stdout);
	setlinebuf(stderr);

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
 * Process expected signals
 */
void handle_signals(struct pollfd* pollfds, bool* p_recheck_time)
{
	struct signalfd_siginfo fdsi;
	bool ack = false;
	bool recheck_time = false;

	/* SIGTERM - exit gracefully */
	if (pollfds[NPFD_SIGTERM].revents & (POLLIN|POLLPRI))
	{
		shutdown();
		notice_msg("terminating...");
		exit(EXIT_SUCCESS);
	}

	/* SIGHUP - reload configuration */
	if (pollfds[NPFD_SIGHUP].revents & (POLLIN|POLLPRI))
	{
		read_siginfo(fd_sighup, &fdsi);
		reload_configuration();
		recheck_time = true;
	}

	/* SIGUSR1 - display debugging information */
	if (pollfds[NPFD_SIGUSR1].revents & (POLLIN|POLLPRI))
	{
		read_siginfo(fd_sigusr1, &fdsi);
		notice_msg("received SIGUSR1, dumping the state to log file (%s) ...",
			   membalanced_log_path);
		show_debugging_info();
		notice_msg("SIGUSR1 dump completed.");
		recheck_time = true;
	}

	/* SIG_CTRL - pause or resume */
	if (pollfds[NPFD_SIG_CTRL].revents & (POLLIN|POLLPRI))
	{
		read_siginfo(fd_sigctrl, &fdsi);
		u_int resp = 0;
		switch (fdsi.ssi_int)
		{
		case CTL_REQ_PAUSE:
			resp = pause_memsched();
			ack = true;
			break;
		case CTL_REQ_RESUME:
			resp = resume_memsched(false);
			ack = true;
			break;
		case CTL_REQ_RESUME_FORCE:
			resp = resume_memsched(true);
			ack = true;
			break;
		}

		/* ack to signal sender */
		if (ack)
		{
			union sigval sigval;
			zap(sigval);
			sigval.sival_int = (int) resp;
			sigqueue(fdsi.ssi_pid, SIGUSR1, sigval);
		}

		recheck_time = true;
	}

	if (recheck_time && p_recheck_time)
		*p_recheck_time = true;
}

/*
 * Process pending Xenstore events (if there are any)
 */
void refresh_xs(void)
{
	struct pollfd pollfd;
	int rc;

	zap(pollfd);
	pollfd.fd = xs_fileno(xs);
	pollfd.events = POLLIN|POLLPRI;

	DO_RESTARTABLE(rc, poll(&pollfd, 1, 0));

	if (pollfd.revents & (POLLIN|POLLPRI))
		handle_xs_watch();
}

/* pause memory adjustment */
u_int pause_memsched(void)
{
	memsched_pause_level++;

	notice_msg("domain memory adjustment paused "
		   "by system administrator (pause level %d)",
		   memsched_pause_level);

	return memsched_pause_level;
}

/* resume memory adjustment */
u_int resume_memsched(bool force)
{
	if (force)
	{
		memsched_pause_level = 0;
	}
	else if (memsched_pause_level)
	{
		memsched_pause_level--;
	}

	if (memsched_pause_level == 0)
		notice_msg("domain memory adjustment resumed by system administrator");
	else
		notice_msg("domain memory adjustment pause level reduced "
			   "by system administrator (to %d)",
			   memsched_pause_level);

	return memsched_pause_level;
}

/*
 * Verify we are running under Xen, and in the root domain (Dom0)
 */
static void verify_hypervisor(void)
{
	FILE* fp = NULL;
	char buf[256];
	char* p;
	const static char root_uuid[] = "00000000-0000-0000-0000-000000000000";

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
	if (0 != strcmp(buf, root_uuid))
		fatal_msg("may run only in a root domain");

	fclose(fp);
}

/*
 * Terminate, logging along "terminating" message
 */
void terminate(void)  /* __noreturn__ */
{
	if (log_syslog)
		syslog(LOG_ERR, "terminating");

	if (log_fp && use_private_log)
	{
		print_timestamp(log_fp);
    		fprintf(log_fp, "terminating\n");
		fflush(log_fp);
	}

	shutdown();

	exit(EXIT_FAILURE);
}

/*
 * Reallocate an array of poll descriptors to provide at least the
 * requested number of entries.
 *
 * @pp - pointer to an array (in-out parameter)
 * @nalloc - currently allocated number of entries (in-out parameter)
 * @nreq - how much to allocate
 *
 * Aborts if cannot allocate.
 */
static void realloc_pollfds(struct pollfd** pp, int* nalloc, int nreq)
{
	if (nreq > *nalloc)
	{
		*pp = (struct pollfd*) realloc(*pp, sizeof(struct pollfd) * nreq);
		if (!*pp)
			out_of_memory();
		*nalloc = nreq;
	}
}


/*
 * Direct logging to membalanced log (@which = 1)
 * or to syslog (@which = 0).
 * If @which is -1, do not change logging sink.
 * Returns previous sink identifier.
 */
int set_log_sink(int which)
{
	int res = use_private_log ? 1 : 0;

	/* log to syslog */
	if (which == 0 && use_private_log)
	{
		if (!log_syslog)
		{
			openlog(progname, LOG_CONS | LOG_PID, LOG_DAEMON);
			log_syslog = true;
		}

		log_fp = NULL;
		use_private_log = false;
	}

	/* log to private log */
	if (which == 1 && !use_private_log)
	{
		do_use_private_log();
	}

	return res;
}

static void do_use_private_log(void)
{
	/* daemon has log file already opened on stderr */
	log_fp = run_as_daemon ? stderr
			       : open_private_log(true);
	use_private_log = true;

	if (log_syslog)
	{
		closelog();
		log_syslog = false;
	}
}

static FILE* open_private_log(bool fatal)
{
	if (private_log_fp == NULL)
		private_log_fp = fopen(membalanced_log_path, "a+");

	if (private_log_fp == NULL)
	{
		if (fatal)
			fatal_perror("unable to open %s", membalanced_log_path);
		else
			error_perror("unable to open %s", membalanced_log_path);
	}

	return private_log_fp;
}

/*
 * Ensure directory @membalance_dir_path exists and has correct mode
 */
void make_membalance_rundir(void)
{
	if (access(membalance_dir_path, F_OK))
	{
		if (errno != ENOENT)
			fatal_perror("cannot access %s", membalance_dir_path);
		if (mkdir(membalance_dir_path, S_IRWXU) && errno != EEXIST)
			fatal_perror("unable to create directory %s", membalance_dir_path);
	}

	if (chmod(membalance_dir_path, S_IRWXU))
		fatal_perror("unable to set protection on directory %s", membalance_dir_path);
}

/*
 * Ensure only one instance of the daemon is running
 */
static void one_instance(void)
{
	int lock_fd;
	struct flock lk;

	make_membalance_rundir();

	lock_fd = open(membalanced_interlock_path, O_CREAT | O_RDWR, S_IRWXU);
	if (lock_fd < 0)
	{
		fatal_perror("unable to create or open file %s",
			     membalanced_interlock_path);
	}

	zap(lk);
	lk.l_type = F_WRLCK;
	lk.l_whence = SEEK_SET;
	lk.l_start = 0;
	lk.l_len = 1;
	if (0 == fcntl(lock_fd, F_SETLK, &lk))
		return;

	if (errno == EACCES || errno == EAGAIN)
	{
		fatal_msg("another instance of %s is already running",
			  progname);
	}
	else
	{
		fatal_perror("unable to acquire lock on file %s",
			     membalanced_interlock_path);
	}
}


/******************************************************************************
*                                 debugging                                   *
******************************************************************************/

void show_debugging_info(FILE* fp)
{
	bool sep = false;

	if (fp != NULL)
	{
		/* use provided file handle */
	}
	else
	{
		fp = open_private_log(false);
		if (!fp)
			return;
		sep = true;
	}

	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);

	char tms[64];
	strftime(tms, countof(tms), "%F %T", &tm);

	if (sep)
	fprintf(fp, "======================================================\n\n");

	fprintf(fp, "%s %s at %s\n", progname, progversion, tms);
	fprintf(fp, "\n");
	fprintf(fp, "domain adjustment:       ");
	if (memsched_pause_level)
		fprintf(fp, "paused (depth %u)\n", memsched_pause_level);
	else
		fprintf(fp, "enabled\n");
	fprintf(fp, "\n");
	fprintf(fp, "sched interval:          %d sec\n", config.interval);
	fprintf(fp, "domain_pending_timeout:  %d sec\n", config.domain_pending_timeout);
	fprintf(fp, "\n");
	fprintf(fp, "host_reserved_hard:      %ld KB\n", config.host_reserved_hard);
	fprintf(fp, "host_reserved_soft:      %ld KB\n", config.host_reserved_soft);
	fprintf(fp, "rate_high:               %lu KB/s\n", config.rate_high);
	fprintf(fp, "rate_low:                %lu KB/s\n", config.rate_low);
	fprintf(fp, "rate_zero:               %lu KB/s\n", config.rate_zero);
	fprintf(fp, "dmem_incr:               %g%%\n", config.dmem_incr * 100.0);
	fprintf(fp, "dmem_decr:               %g%%\n", config.dmem_decr * 100.0);
	fprintf(fp, "guest_free_threshold:    %g%%\n", config.guest_free_threshold * 100.0);
	fprintf(fp, "startup_time:            %d sec\n", config.startup_time);
	fprintf(fp, "trim_unresponsive:       %d sec\n", config.trim_unresponsive);
	fprintf(fp, "trim_unmanaged:          %s\n", config.trim_unmanaged ? "yes" : "no");
	fprintf(fp, "\n");
	fprintf(fp, "debug_level:             %d\n", debug_level);
	fprintf(fp, "max_xs_retries:          %d\n", config.max_xs_retries);
	fprintf(fp, "max_xen_init_retries:    %d sec\n", config.max_xen_init_retries);
	fprintf(fp, "xen_init_retry_msg:      %d sec\n", config.xen_init_retry_msg);
	fprintf(fp, "\n");

	show_domains(fp);

	if (sep)
	fprintf(fp, "\n------------------------------------------------------\n");

	fflush(fp);
}

/*
 * Asserts that cannot be executed at compile time
 */
static void runtime_asserts(void)
{
	if (!(SIG_CTRL >= SIGRTMIN && SIG_CTRL <= SIGRTMAX))
	{
		fprintf(stderr, "bug: "
			"SIG_CTRL (%d) >= SIGRTMIN (%d) && SIG_CTRL <= SIGRTMAX (%d)\n",
			(int) SIG_CTRL, (int) SIGRTMIN, (int) SIGRTMAX);
		exit(EXIT_FAILURE);
	}
}

