/*
 *  MEMBALANCECTL utility
 *
 *  membalancectl.cpp
 *
 *     MEMBALANCECTL is used to control MEMBALANCED, e.g. to suspend and resume
 *     its activity
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

#include "membalanced.h"

#include <sys/socket.h>
#include <sys/un.h>

extern "C"
{
#include "rcmd.h"
void rcmd_clnt_set_timeout(struct timeval* ptv);
}


/******************************************************************************
*                               static data                                   *
******************************************************************************/

static long ack_timeout = 10;		/* timeout for ack from daemon (sec) */
static int verbose = 0; 		/* verbosity level */
static bool quiet = false;       	/* be quiet */
static bool human = false;      	/* format output human-readable */
static const char* execname = NULL;     /* name or path of membalanced executable */
static CLIENT* clnt = NULL;     	/* RPC client */
static int sock = -1;   		/* socket for RPC connection */

static const char* rpc_call_error_msg = "RPC call to membalanced daemon failed";


/******************************************************************************
*                           forward declarations                              *
******************************************************************************/

static void usage(int exitcode = EXIT_FAILURE)  __noreturn__;
static void handle_cmdline_common_options(int* pargc, char*** pargv, bool* pdone);
static void ivarg(const char* arg)  __noreturn__;
static int cmd_list(int argc, char** argv);
static int cmd_pause(int argc, char** argv);
static int cmd_resume(int argc, char** argv);
static int cmd_free_memory(int argc, char** argv);
static int cmd_dump_debug(int argc, char** argv);
static int cmd_show_debug(int argc, char** argv);
static int cmd_log_level(int argc, char** argv);
static int cmd_log_sink(int argc, char** argv);
static int cmd_manage_domain(int argc, char** argv);
static int cmd_pause_sig(int argc, char** argv);
static int cmd_resume_sig(int argc, char** argv);
static int cmd_test(int argc, char** argv);
static int do_pause_resume_sig(char action, bool force);
static void done_pause(u_int level);
static void done_resume(u_int level, bool force);
static long find_daemon(void);
static bool match_execname(const char* path);
static bool is_daemon_uid(long pid);
static void create_rpc_client(void);
static int encode_sink(const char* sink);
static const char* decode_sink(int sink);
static unsigned long get_domain_id(const char* cp);
static bool is_achieved(long needed_mem, bool above_slack, const rcmd_freemem_res& res);
static void show_free(const char* msg, bool above_slack, const rcmd_freemem_res& res);
static long fetch_domain_config_needed_memory(const char* config_file);


/******************************************************************************
*                      top-level command parser/dispatcher                    *
******************************************************************************/

static void show_usage()
{
	FILE* fp = stderr;
	fprintf(fp, "Usage:\n");
	fprintf(fp, "\n");
	fprintf(fp, "    list                 display domains and memory status\n");
	fprintf(fp, "\n");
	fprintf(fp, "    pause                suspend automatic domain memory balancing\n");
	fprintf(fp, "        [--quiet]        do not print pause level\n");
	fprintf(fp, "\n");
	fprintf(fp, "    resume               resume automatic domain memory balancing\n");
	fprintf(fp, "        [--force]        drop remaining pause level to zero\n");
	fprintf(fp, "        [--quiet]        do not print remaining pause level\n");
	fprintf(fp, "\n");
	fprintf(fp, "    free-memory          ensure sufficient amount of free memory\n");
	fprintf(fp, "        <n>[k|m|g]       specify required amount explicitly\n");
	fprintf(fp, "        --config <x.cfg> use the amount requested by domain config file\n");
	fprintf(fp, "        [--above-slack]  the amount is on top of Xen free memory slack\n");
	fprintf(fp, "        [--use-reserved-hard]   draw on host_reserved_hard if necessary\n");
	fprintf(fp, "        [--must]         terminate with failure status if could not ...\n");
	fprintf(fp, "                         ... allocate the full requested amount\n");
	fprintf(fp, "\n");
	fprintf(fp, "    manage-domain <id>   request to manage domain that was in unmanaged state\n");
	fprintf(fp, "    manage-domain --all  request to manage all currently unmanaged domains\n");
	fprintf(fp, "\n");
	fprintf(fp, "    log-level [n]        set and/or show logging level\n");
	fprintf(fp, "    log-sink [which]     set logging sink to \"syslog\" or \"logfile\"\n");
	fprintf(fp, "    dump-debug           dump daemon internal state to %s\n", membalanced_log_path);
	fprintf(fp, "    show-debug           dump daemon internal state to stdout\n");
#ifdef DEVEL
	fprintf(fp, "\n");
	fprintf(fp, "    test <id> [args...]  execute development-time test\n");
#endif
	fprintf(fp, "\n");
	fprintf(fp, "    --version            print version (%s %s)\n", progname, progversion);
	fprintf(fp, "    --help               print this text\n");
	fprintf(fp, "\n");
	fprintf(fp, "Common options:\n");
	fprintf(fp, "\n");
	fprintf(fp, "    --verbose, -v        verbose output\n");
	fprintf(fp, "    -vv                  even more verbose output\n");
	fprintf(fp, "    -vvv                 most verbose output\n");
	fprintf(fp, "    --quiet              quiet operation\n");
	fprintf(fp, "    --human, -h          display data in human-readable format\n");
	fprintf(fp, "    --timeout <sec>      time to wait for ack from the daemon (default: 10)\n");
	fprintf(fp, "    --exec <path>        specify the pathname of membalanced executable\n");
}

static void usage(int exitcode)
{
	show_usage();
	exit(exitcode);
}

int do_main_membalancectl(int argc, char** argv)
{
	int rc = EXIT_SUCCESS;

	/*
	 * Designate membalancectl as an active program
	 */
	progname = membalancectl_progname;
	progversion = membalancectl_progversion;
	log_timestamps = false;

	argc--;
	argv++;
	bool cmd_done = false;
	handle_cmdline_common_options(&argc, &argv, &cmd_done);

	if (argc == 0)
	{
		if (!cmd_done)
			usage();
		return EXIT_SUCCESS;
	}

	/* fetch the verb */
	const char* verb = argv[0];
	argv++;  argc--;

	/* process commands */
	if (streq(verb, "list"))
		rc = cmd_list(argc, argv);
	else if (streq(verb, "pause"))
		rc = cmd_pause(argc, argv);
	else if (streq(verb, "resume"))
		rc = cmd_resume(argc, argv);
	else if (streq(verb, "free-memory"))
		rc = cmd_free_memory(argc, argv);
	else if (streq(verb, "dump-debug"))
		 rc = cmd_dump_debug(argc, argv);
	else if (streq(verb, "show-debug"))
		 rc = cmd_show_debug(argc, argv);
	else if (streq(verb, "log-level"))
		rc = cmd_log_level(argc, argv);
	else if (streq(verb, "log-sink"))
		rc = cmd_log_sink(argc, argv);
	else if (streq(verb, "manage-domain"))
		rc = cmd_manage_domain(argc, argv);
#ifdef DEVEL
	else if (streq(verb, "test"))
		rc = cmd_test(argc, argv);
#endif
	else
		usage();

	return rc;
}

/*
 * Process common options and weed them out from argv.
 * Adjust argc and argv to reflect the change.
 *
 * If a meaningful action has been performed (such as --help or
 * --version), *@pdone is set to @true.
 */
static void handle_cmdline_common_options(int* pargc, char*** pargv, bool* pdone)
{
	bool done = false;
	char* cp;

	int argc = *pargc;
	char** argv = *pargv;

	if (argc == 0)
		return;

	/*
	 * will be copying non-common options to (ac, av)
	 */
	char** av = (char**) xmalloc(argc * sizeof(char*));
	int ac = 0;

	for (int k = 0;  k < argc;  k++)
	{
		if (streq(argv[k], "--timeout"))
		{
			if (k == argc - 1)
				usage();
			cp = argv[++k];
			if (!a2long(cp, &ack_timeout) || ack_timeout < 1)
				ivarg(cp);
		}
		else if (streq(argv[k], "--version"))
		{
			printf("%s version %s\n", progname, progversion);
			done = true;
		}
		else if (streq(argv[k], "--help"))
		{
			show_usage();
			done = true;
		}
		else if (streq(argv[k], "--quiet"))
		{
			quiet = true;
		}
		else if (streq(argv[k], "--verbose") || streq(argv[k], "-v"))
		{
			verbose = 1;
		}
		else if (streq(argv[k], "-vv"))
		{
			verbose = 2;
		}
		else if (streq(argv[k], "-vvv"))
		{
			verbose = 3;
		}
		else if (streq(argv[k], "--human") || streq(argv[k], "-h"))
		{
			human = true;
		}
		else if (streq(argv[k], "--exec"))
		{
			if (k == argc - 1)
				usage();
			execname = argv[++k];
		}
		else
		{
			av[ac++] = argv[k];
		}
	}

	/*
	 * Replace the original argc/argv with the filtered version.
	 */
	*pargc = ac;
	*pargv = av;

	if (done)
		*pdone = true;
}

/*
 * Print "invalid argument" message and exit
 */
static void ivarg(const char* arg)  /* __noreturn__ */
{
	fprintf(log_fp, "%s: invalid argument: %s (use --help)\n", progname, arg);
	exit(EXIT_FAILURE);
}


/******************************************************************************
*                           domain/memory list command                        *
******************************************************************************/

/*
 * "List" command handler
 */
static int cmd_list(int argc, char** argv)
{
	if (argc)
		usage();

	create_rpc_client();

	char* result = NULL;
	enum clnt_stat rpc_status = rcmd_show_status_1(verbose, &result, clnt);
	CHECK(rpc_status == RPC_SUCCESS);

	printf("%s", result);

cleanup:

	if (rpc_status != RPC_SUCCESS)
		clnt_perror(clnt, rpc_call_error_msg);

	free_ptr(result);

	return rpc_status == RPC_SUCCESS ? EXIT_SUCCESS
					 : EXIT_FAILURE;
}


/******************************************************************************
*                            pause/resume commands                            *
******************************************************************************/

/*
 * "Pause" command handler
 */
static int cmd_pause(int argc, char** argv)
{
	if (argc)
		usage();

	create_rpc_client();

	u_int level;
	enum clnt_stat rpc_status = rcmd_pause_1(&level, clnt);
	if (rpc_status != RPC_SUCCESS)
	{
		clnt_perror(clnt, rpc_call_error_msg);
		return EXIT_FAILURE;
	}

	done_pause(level);

	return EXIT_SUCCESS;
}

static void done_pause(u_int level)
{
	if (verbose || (level != 1 && !quiet))
	{
		printf("Automatic domain memory adjustment has been paused "
		       "(pause depth: %u).\n", level);
	}

}

/*
 * "Resume" command handler
 */
static int cmd_resume(int argc, char** argv)
{
	bool force = false;

	for (int k = 0;  k < argc;  k++)
	{
		if (streq(argv[k], "--force"))
			force = true;
		else
			usage();
	}

	create_rpc_client();

	u_int level;
	enum clnt_stat rpc_status = rcmd_resume_1(force, &level, clnt);
	if (rpc_status != RPC_SUCCESS)
	{
		clnt_perror(clnt, rpc_call_error_msg);
		return EXIT_FAILURE;
	}

	done_resume(level, force);

	return EXIT_SUCCESS;
}

static void done_resume(u_int level, bool force)
{
	bool show = false;

	if (verbose)
		show = true;
	else if (force)
		show = (level != 0);  /* should never happen */
	else
		show = (level != 0) && !quiet;

	if (show)
	{
		if (level == 0)
			printf("Automatic domain memory adjustment has been resumed.\n");
		else
			printf("Remaining pause level for automatic domain memory "
			       "adjustment: %d.\n", level);
	}
}


/******************************************************************************
*                             "free-memory" command                           *
******************************************************************************/

static int cmd_free_memory(int argc, char** argv)
{
	bool above_slack = false;
	bool must = false;
	bool use_reserved_hard = false;
	const char* amount_str = NULL;
	const char* config_file = NULL;
	long needed_mem;
	rcmd_freemem_res res;
	zap(res);
	bool done = false;

	/*
	 * Process command arguments
	 */
	for (int k = 0;  k < argc;  k++)
	{
		if (streq(argv[k], "--above-slack"))
		{
			above_slack = true;
		}
		else if (streq(argv[k], "--must"))
		{
			must = true;
		}
		else if (streq(argv[k], "--use-reserved-hard"))
		{
			use_reserved_hard = true;
		}
		else if (streq(argv[k], "--config"))
		{
			if (config_file || k == argc - 1)
				usage();
			config_file = argv[++k];
		}
		else if (argv[k][0] != '-')
		{
			if (amount_str)
				usage();
			amount_str = argv[k];
		}
		else
		{
			usage();
		}
	}

	if (amount_str && config_file)
		usage();

	if (!amount_str && !config_file)
		usage();

	/*
	 * Find out the requested amount of memory
	 */
	if (config_file)
	{
		needed_mem = fetch_domain_config_needed_memory(config_file);
	}
	else if (!parse_kb("the command line", "memory amount",
			   amount_str, "mb", &needed_mem))
	{
		return EXIT_FAILURE;
	}

	/*
	 * Invoke membalance daemon
	 */
	create_rpc_client();
	enum clnt_stat rpc_status = rcmd_freemem_1((u_quad_t) needed_mem,
						   above_slack,
						   use_reserved_hard,
						   must,
						   ((int) ack_timeout * 3) / 4,
						   &res,
						   clnt);
	CHECK(rpc_status == RPC_SUCCESS);

	/*
	 * Process the response
	 */
	switch (res.status)
	{
	case 'N':
		/* @must must be @false */
		error_msg("Error: insufficient free memory");
		show_free("Maximum freeable memory", above_slack, res);
		goto cleanup;

	case 'A':
		break;

	case 'P':
		fatal_msg("Error: automatic domain memory allocation adjustment is not paused");

	default:
		fatal_msg("Error: unrecognized response from the daemon");
	}

	if (is_achieved(needed_mem, above_slack, res))
	{
		if (verbose)
			show_free("Sufficient free memory is available", above_slack, res);

		if (verbose > 1 && above_slack)
			show_free("Free memory available with slack", false, res);

		done = true;
	}
	else if (must)
	{
		error_msg("Error: insufficient free memory");
		show_free("Free memory available", above_slack, res);
		if (above_slack)
			show_free("Free memory available with slack", false, res);
	}
	else
	{
		warning_msg("Warning: insufficient free memory");
		show_free("Free memory available", above_slack, res);
		if (above_slack)
			show_free("Free memory available with slack", false, res);
		done = true;
	}

cleanup:

	if (rpc_status != RPC_SUCCESS)
		clnt_perror(clnt, rpc_call_error_msg);

	/* deallocate results */
	xdr_free((xdrproc_t) xdr_rcmd_freemem_res, (caddr_t) &res);

	return done ? EXIT_SUCCESS
		    : EXIT_FAILURE;
}

/*
 * Check if the goal is achieved
 */
static bool is_achieved(long needed_mem, bool above_slack, const rcmd_freemem_res& res)
{
	return needed_mem <= (long) (above_slack ? res.freemem_less_slack
			                         : res.freemem_with_slack);
}

/*
 * Show how much memory is was attained or is attainable
 */
static void show_free(const char* msg, bool above_slack, const rcmd_freemem_res& res)
{
	long avail = (long) (above_slack ? res.freemem_less_slack
					 : res.freemem_with_slack);

	fprintf(stderr, "%s: %ld kbs = %ld mb + %ld kb\n",
		        msg, avail,
		        avail / 1024, avail % 1024);
}

/*
 * Parse Xen domain configuration file @config_file and determine
 * how much memory it requires to start the domain
 */
static long fetch_domain_config_needed_memory(const char* config_file)
{
	XLU_Config* xlu_config = NULL;
	libxl_domain_build_info* b_info;
	uint32_t need_memkb;
	int e;
	long lv;
	const char* cp;
	libxl_domain_config d_config;

	initialize_xl_ctl();

	/*
	 * We have to do it all here because XL does not export parse_config_data(...)
	 * routine. The code below might result in values slightly off if XL code
	 * changes.
	 *
	 * Another alternative would be to read and parse JSON output of
	 * "xl create <config> --dryrun", about just as messy and with the same
	 * issue of potential divergence with XL-calculated value of need_memkb.
	 */
	libxl_domain_config_init(&d_config);
	b_info = &d_config.b_info;

	/* init the parser */
	xlu_config = xlu_cfg_init(stderr, config_file);
	if (!xlu_config)
		fatal_msg("unable to initialize Xen config parser");

	/* parse config (relevant pieces) */
	e = xlu_cfg_readfile(xlu_config, config_file);
	if (e)
		fatal_msg("unable to parse %s: %s", config_file, strerror(e));

	if (0 == xlu_cfg_get_string(xlu_config, "builder", &cp, 0) && streq(cp, "hvm"))
		b_info->type = LIBXL_DOMAIN_TYPE_HVM;
	else
		b_info->type = LIBXL_DOMAIN_TYPE_PV;

	if (0 == xlu_cfg_get_long(xlu_config, "memory", &lv, 0))
		b_info->target_memkb = b_info->max_memkb = lv * 1024;

	if (0 == xlu_cfg_get_long(xlu_config, "maxmem", &lv, 0))
		b_info->max_memkb = lv * 1024;

	xlu_cfg_get_defbool(xlu_config, "device_model_stubdomain_override",
                            &b_info->device_model_stubdomain, 0);

	if (0 == xlu_cfg_get_long(xlu_config, "vcpus", &lv, 0))
		b_info->max_vcpus = lv;

	if (0 == xlu_cfg_get_long(xlu_config, "maxvcpus", &lv, 0))
		b_info->max_vcpus = lv;

	if (0 == xlu_cfg_get_long(xlu_config, "shadow_memory", &lv, 0))
	{
		b_info->shadow_memkb = lv * 1024;
	}
	else
	{
		b_info->shadow_memkb = libxl_get_required_shadow_memory(
						b_info->max_memkb,
						b_info->max_vcpus);
	}

	if (libxl_domain_need_memory(xl_ctx, b_info, &need_memkb))
		fatal_msg("unable to parse config file %s", config_file);

	if (xlu_config)
		xlu_cfg_destroy(xlu_config);

	libxl_domain_config_dispose(&d_config);

	return need_memkb;
}


/******************************************************************************
*                             daemon debug commands                           *
******************************************************************************/

/*
 * "Dump-debug" command handler
 */
static int cmd_dump_debug(int argc, char** argv)
{
	if (argc)
		usage();

	create_rpc_client();

	void* result;
	enum clnt_stat rpc_status = rcmd_debug_dump_1(&result, clnt);
	if (rpc_status != RPC_SUCCESS)
	{
		clnt_perror(clnt, rpc_call_error_msg);
		return EXIT_FAILURE;
	}

	if (verbose)
		printf("Daemon state was dumped to log file %s\n", membalanced_log_path);

	return EXIT_SUCCESS;
}

/*
 * "Show-debug" command handler
 */
static int cmd_show_debug(int argc, char** argv)
{
	if (argc)
		usage();

	create_rpc_client();

	char* result = NULL;
	enum clnt_stat rpc_status = rcmd_debug_dump_to_string_1(&result, clnt);
	CHECK(rpc_status == RPC_SUCCESS);

	printf("%s\n", result);

cleanup:

	if (rpc_status != RPC_SUCCESS)
		clnt_perror(clnt, rpc_call_error_msg);

	free_ptr(result);

	return rpc_status == RPC_SUCCESS ? EXIT_SUCCESS
					 : EXIT_FAILURE;
}

/*
 * "Log-level" command handler
 */
static int cmd_log_level(int argc, char** argv)
{
	int new_level;
	int old_level;

	if (argc == 0)
	{
		new_level = -1;
	}
	else if (argc == 1)
	{
		if (!a2int(argv[0], &new_level) || new_level < 0)
			ivarg(argv[0]);
	}
	else
	{
		usage();
	}

	create_rpc_client();

	enum clnt_stat rpc_status = rcmd_set_debug_level_1(new_level, &old_level, clnt);
	CHECK(rpc_status == RPC_SUCCESS);

	if (new_level == -1)
		printf("Current logging level: %d\n", old_level);
	else if (verbose)
		printf("Previous logging level: %d, new level: %d\n", old_level, new_level);

cleanup:

	if (rpc_status != RPC_SUCCESS)
		clnt_perror(clnt, rpc_call_error_msg);

	return rpc_status == RPC_SUCCESS ? EXIT_SUCCESS
					 : EXIT_FAILURE;
}

/*
 * "Log-sink" command handler
 */
static int cmd_log_sink(int argc, char** argv)
{
	int new_sink;
	int old_sink;

	if (argc == 0)
	{
		new_sink = -1;
	}
	else if (argc == 1)
	{
		new_sink = encode_sink(argv[0]);
	}
	else
	{
		usage();
	}

	create_rpc_client();

	enum clnt_stat rpc_status = rcmd_set_logging_sink_1(new_sink, &old_sink, clnt);
	CHECK(rpc_status == RPC_SUCCESS);

	if (new_sink == -1)
	{
		printf("Logging to %s\n", decode_sink(old_sink));
	}
	else if (verbose)
	{
		printf("Previous was logging to: %s, now logging to: %s\n",
		       decode_sink(old_sink),
		       decode_sink(new_sink));
	}

cleanup:

	if (rpc_status != RPC_SUCCESS)
		clnt_perror(clnt, rpc_call_error_msg);

	return rpc_status == RPC_SUCCESS ? EXIT_SUCCESS
					 : EXIT_FAILURE;
}

/* sink name -> id */
static int encode_sink(const char* sink)
{
	if (streq(sink, "syslog"))
		return 0;
	else if (streq(sink, "logfile") || streq(sink, "log-file"))
		return 1;
	else
		usage();
}

/* sink id -> name */
static const char* decode_sink(int sink)
{
	switch (sink)
	{
	case 0:	   return "syslog";
	case 1:    return "logfile";
	default:   return "<unknown>";
	}
}


/******************************************************************************
*                            domain-related commands                          *
******************************************************************************/

/*
 * "Manage-domain" command handler
 */
static int cmd_manage_domain(int argc, char** argv)
{
	unsigned long domain_id;
	int exstat = EXIT_FAILURE;
	FILE* fp = NULL;

	rcmd_status res;
	zap(res);

	if (argc == 1)
	{
		if (streq(argv[0], "--all") || streq(argv[0], "-a"))
			domain_id = (unsigned long) -1;
		else
			domain_id = get_domain_id(argv[0]);
	}
	else
	{
		usage();
	}

	create_rpc_client();
	enum clnt_stat rpc_status = rcmd_manage_domain_1(domain_id, &res, clnt);
	CHECK(rpc_status == RPC_SUCCESS);

	if (res.status == 'X')
		fp = stderr;
	else if (verbose)
		fp = stdout;

	if (fp && res.message && *res.message)
		fprintf(fp, "%s\n", res.message);

	if (res.status != 'X')
		exstat = EXIT_SUCCESS;

cleanup:

	if (rpc_status != RPC_SUCCESS)
		clnt_perror(clnt, rpc_call_error_msg);

	/* deallocate results */
	xdr_free((xdrproc_t) xdr_rcmd_status, (caddr_t) &res);

	return exstat;
}


/******************************************************************************
*                                execute a test                               *
******************************************************************************/

#ifdef DEVEL
static int cmd_test(int argc, char** argv)
{
	int exstat = EXIT_FAILURE;
	rcmd_str_listentry* args = NULL;
	rcmd_str_listentry* ap;
	rcmd_str_listentry* prev = NULL;
	rcmd_status res;
	zap(res);

	if (argc == 0)
		usage();

	/* create argument list for marshalling */
	for (int k = 0;  k < argc;  k++)
	{
		ap = (rcmd_str_listentry*) xmalloc(sizeof(*ap));
		ap->next = NULL;
		ap->value = argv[k];
		if (args == NULL)
			args = ap;
		if (prev)
			prev->next = ap;
		prev = ap;
	}

	/* execute the call */
	create_rpc_client();
	enum clnt_stat rpc_status = rcmd_test_1(args, &res, clnt);
	CHECK(rpc_status == RPC_SUCCESS);

	/* display results */
	printf("Test exit status: %d", res.status);
	if (res.message[0])
		printf(", response: [%s]", res.message);
	printf("\n");

	exstat = EXIT_SUCCESS;

cleanup:

	if (rpc_status != RPC_SUCCESS)
		clnt_perror(clnt, rpc_call_error_msg);

	/* deallocate results */
	xdr_free((xdrproc_t) xdr_rcmd_status, (caddr_t) &res);

	/* deallocate args list */
	while (args != NULL)
	{
		ap = args;
		args = args->next;
		free(ap);
	}

	return exstat;
}
#endif // DEVEL


/******************************************************************************
*                            pause/resume commands                            *
*                         (old version using signals)                         *
******************************************************************************/

/*
 * "Pause" command handler
 */
static int cmd_pause_sig(int argc, char** argv)
{
	if (argc)
		usage();

	return do_pause_resume_sig('P', false);
}

/*
 * "Resume" command handler
 */
static int cmd_resume_sig(int argc, char** argv)
{
	bool force = false;

	for (int k = 0;  k < argc;  k++)
	{
		if (streq(argv[k], "--force"))
			force = true;
		else
			usage();
	}

	return do_pause_resume_sig('R', force);
}

static int do_pause_resume_sig(char action, bool force)
{
	union sigval sigval;
	sigset_t sigmask;
	struct pollfd pollfds[1];
	struct timespec ts0;
	int64_t wait_ms;
	int fd_sigusr1 = -1;     	/* handle for SIGUSR1 */
	long daemon_pid;

	/*
	 * Set up signal to receive ack from the daemon.
	 *
	 * First block signals so that they aren't handled according
	 * to their default dispositions.
	 */
	signal(SIGUSR1, SIG_DFL);
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGUSR1);
	if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0)
		fatal_perror("sigprocmask");

	/*
	 * Receive SIGUSR1 on file descriptor
	 */
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGUSR1);
	fd_sigusr1 = signalfd(-1, &sigmask, 0);
	if (fd_sigusr1 < 0)
		fatal_perror("signalfd(SIGUSR1)");

	/* select clock to use */
	select_clk_id();

	/* find daemon process */
	daemon_pid = find_daemon();

	/*
	 * Send control request to the daemon
	 */
	sigval.sival_ptr = NULL;
	if (action == 'P')
		sigval.sival_int = CTL_REQ_PAUSE;
	else if (force)
		sigval.sival_int = CTL_REQ_RESUME_FORCE;
	else
		sigval.sival_int = CTL_REQ_RESUME;

	if (sigqueue(daemon_pid, SIG_CTRL, sigval))
		fatal_perror("unable to send a signal to daemon process, pid %ld", daemon_pid);

	/*
	 * Wait for the acknowledegement from the daemon
	 */
	pollfds[0].fd = fd_sigusr1;
	pollfds[0].events = POLLIN|POLLPRI;

	ts0 = getnow();

	for (;;)
	{
		wait_ms = timespec_diff_ms(ts0, getnow()) + ack_timeout * MSEC_PER_SEC;
		if (wait_ms < 100)
			fatal_msg("sent request to the daemon, "
				  "timed out waiting for acknowledgement");

		pollfds[0].revents = 0;
		if (poll(pollfds, countof(pollfds), wait_ms) < 0)
			fatal_perror("poll");

		if (pollfds[0].revents & (POLLIN|POLLPRI))
		{
			struct signalfd_siginfo fdsi;
			read_siginfo(fd_sigusr1, &fdsi);
			u_int level = (u_int) fdsi.ssi_int;
			if (action == 'P')
				done_pause(level);
			else
				done_resume(level, force);
			break;
		}
	}

	return EXIT_SUCCESS;
}


/******************************************************************************
*                            find daemon process                              *
******************************************************************************/

/*
 * Locate daemon process.
 * Return its pid.
 * Abort with error message if cannot find.
 */
static long find_daemon(void)
{
	DIR* dir;
	struct dirent* dent;
	char* dname;
	long pid;
	struct stat st;
	pid_t msctl_pid = getpid();
	char path[PATH_MAX];
	char link[PATH_MAX];
	ssize_t sz;
	long daemon_pid = -1;

	/*
	 * Enumerate processes in the system
	 */
	dir = opendir("/proc");
	if (!dir)
		fatal_perror("cannot access /proc");

	for (;;)
	{
		errno = 0;
		if (NULL == (dent = readdir(dir)))
		{
			if (errno == 0)
				break;
			fatal_perror("cannot enumerate /proc");
		}
		dname = dent->d_name;

		/* ignore non-numeric directories */
		if (!isdigit(*dname))
			continue;

		/* extract pid */
		if (!a2long(dname, &pid) || pid <= 0)
			continue;

		/* ignore ourselves */
		if (pid == msctl_pid)
			continue;

		/* proc/<pid> must be a subdirectory */
		snprintf(path, countof(path), "/proc/%ld", pid);
		if (lstat(path, &st))
			fatal_perror("cannot access /proc/%ld", pid);
		if (!S_ISDIR(st.st_mode))
			continue;

		/* get the name of an executable */
		snprintf(path, countof(path), "/proc/%ld/exe", pid);
		sz = readlink(path, link, countof(link));

		if (sz < 0)
		{
			if (errno == ENOENT)
				continue;
			fatal_perror("cannot access %s", path);
		}

		if (sz >= (ssize_t) countof(link))
		{
			error_msg("exec path name for process %ld is too long, ignoring", pid);
			continue;
		}

		link[sz] = '\0';

		if (!match_execname(link) || !is_daemon_uid(pid))
			continue;

		if (daemon_pid != -1)
			fatal_msg("daemon search is ambigious: pids %ld and %ld match", daemon_pid, pid);

		daemon_pid = pid;
	}

	closedir(dir);

	if (daemon_pid == -1)
		fatal_msg("membalanced daemon is not running");

	return daemon_pid;
}

/*
 * Check if executable name matches the daemon
 */
static bool match_execname(const char* path)
{
	const char* ep = strrchr(path, '/');
	ep = ep ? ep + 1 : path;

	if (execname == NULL)
	{
		/* match only basename */
		return streq(ep, "membalanced");
	}
	else if (NULL != strchr(execname, '/'))
	{
		/* match full pathname */
		return streq(path, execname);
	}
	else
	{
		/* match only basename */
		return streq(ep, execname);
	}
}

/*
 * Check if the process runs as the daemon (euid = root)
 */
static bool is_daemon_uid(long pid)
{
	char path[PATH_MAX];
	char buf[512];
	FILE* fp;
	const static char prefix[] = "Uid:";
	bool found = false;
	unsigned long ruid, euid, svuid, fsuid;
	char c;

	snprintf(path, countof(path), "/proc/%ld/status", pid);
	fp = fopen(path, "r");
	if (!fp)
		fatal_perror("cannot open file %s", path);

	while (fgets(buf, countof(buf), fp))
	{
		if (0 != strncmp(buf, prefix, countof(prefix) - 1))
			continue;
		found = true;
		break;
	}

	if (!found)
	{
		error_msg("could not find Uid line in %s", path);
		return false;
	}

	if (4 != sscanf(buf, "Uid: %lu %lu %lu %lu\n%c", &ruid, &euid, &svuid, &fsuid, &c))
	{
		error_msg("could not parse Uid line in %s", path);
		return false;
	}

	return euid == 0;
}


/******************************************************************************
*                           RPC client handling                               *
******************************************************************************/

static void create_rpc_client(void)
{
	struct sockaddr_un addr;
	struct timeval tv;
	int rc;

	if (clnt != NULL)
		return;

	DO_RESTARTABLE(sock, socket(AF_UNIX, SOCK_STREAM, 0));
	if (sock == -1)
		fatal_perror("unable to create RPC socket");

	/* build address block */
	if (strlen(socket_path) >= sizeof(addr.sun_path))
		fatal_msg("RPC socket path is too long");

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, socket_path);

	/* connect to daemon */
	DO_RESTARTABLE(rc, connect(sock, (struct sockaddr*)&addr, sizeof addr));
	if (rc)
	{
		if (errno == ECONNREFUSED)
			fatal_msg("unable to connect to membalance daemon, daemon probably is not running");
		else
			fatal_perror("unable to connect to membalance daemon");
	}

	/* create RPC transport */
	clnt = clntunix_create(&addr, RCMD_MEMBALANCED, RCMD_MEMBALANCED_V1, &sock, 0, 0);
	if (!clnt)
		fatal_msg("unable to create RPC client");

	/*
	 * Set timeout on the transport
	 *
	 */
	tv.tv_sec = ack_timeout;
	tv.tv_usec = 0;
	if (false)
	{
		/*
		* This currently does not work because of the bug in glibc.
		* clntunix_control(CLSET_TIMEOUT) currently does not set ct_waitset
		* to true, so a workaround is to append a routine to rcmd_clnt.c
		* during the build to change TIMEOUT in there.
		*/
		clnt_control(clnt, CLSET_TIMEOUT, (char*) &tv);
	}
	else
	{
		rcmd_clnt_set_timeout(&tv);
	}
}


/******************************************************************************
*                             utility routines                                *
******************************************************************************/

/*
 * Parse xen domain id
 */
static unsigned long get_domain_id(const char* cp)
{
	unsigned long val;

	if (!a2ulong(cp, &val))
		ivarg(cp);

	return val;
}

