/*
 *  MEMBALANCE daemon
 *
 *  rcmd_server.cpp - server side for MEMBALANCED management RPC interface
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
SVCXPRT *svcunixfd_create(int fd, u_int sendsize, u_int recvsize);
void rcmd_membalanced_1(struct svc_req *rqstp, register SVCXPRT *transp);
}


/******************************************************************************
*                             forward declarations                            *
******************************************************************************/

static void on_addrinuse(const struct sockaddr_un& addr);


/******************************************************************************
*                                static data                                  *
******************************************************************************/

static int sock = -1;			/* listener socket */
static bool sock_unlink = false;	/* remove socket file on shutdown */
static bool svc_registered = false;	/* service registered */


/******************************************************************************
*                           start/stop RPC server                             *
******************************************************************************/

/*
 * Start built-in RPC server
 */
void start_rcmd_server(void)
{
	struct sockaddr_un addr;
	int rc;

	if (sock != -1)
		return;

	/* ensure socket directory exists */
	make_membalance_rundir();

	for (;;)
	{
		/* create socket */
		if (sock == -1)
		{
			DO_RESTARTABLE(sock, socket(AF_UNIX, SOCK_STREAM, 0));
			if (sock == -1)
				fatal_perror("unable to create RPC socket");
		}

		/* build address block */
		if (strlen(socket_path) >= sizeof(addr.sun_path))
			fatal_msg("RPC socket path is too long");

		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strcpy(addr.sun_path, socket_path);

		/* try to bind socket to a name (and create socket file in file system) */
		DO_RESTARTABLE(rc, bind(sock, (struct sockaddr*)&addr, sizeof addr));
		if (rc != 0)
		{
			/* socket file already exists? */
			if (errno == EADDRINUSE)
			{
				on_addrinuse(addr);
				continue;
			}
			else
			{
				fatal_perror("unable to bind RPC socket to %s", socket_path);
			}
		}

		/* successfully created socket file */
		sock_unlink = true;
		if (chmod(socket_path, S_IRWXU))
		{
			stop_rcmd_server();
			fatal_perror("unable to set protection on socket file %s", socket_path);
		}

		DO_RESTARTABLE(rc, listen(sock, SOMAXCONN));
		if (rc == 0)
			break;

		stop_rcmd_server();

		if (errno == EADDRINUSE)
		{
			on_addrinuse(addr);
			continue;
		}
		else
		{
			fatal_perror("unable to listen on socket file %s", socket_path);
		}
	}

	if (!svc_registered)
	{
		if (!svc_register(NULL, RCMD_MEMBALANCED, RCMD_MEMBALANCED_V1, rcmd_membalanced_1, 0))
			fatal_msg("unable to register RPC server (svc_register)");
		svc_registered = true;
	}

	debug_msg(7, "registered RPC server");
}

/*
 * Socket file with the same name already exists.
 * If another instance of membalanced is already running, abort.
 * Otherwise delete leftover file, and let the caller retry again.
 */
static void on_addrinuse(const struct sockaddr_un& addr)
{
	int fd;
	int rc;

	DO_RESTARTABLE(fd, socket(AF_UNIX, SOCK_STREAM, 0));
	if (fd == -1)
		fatal_perror("unable to create RPC socket");

	DO_RESTARTABLE(rc, connect(fd, (struct sockaddr*)&addr, sizeof addr));
	if (rc == 0)
		fatal_msg("membalanced is already running and owns RPC socket");
	if (errno != ECONNREFUSED)
		fatal_perror("socket file %s in a bad state", socket_path);

	/* socket exists, but no one is lisening on it */
	if (unlink(socket_path) && errno != ENOENT)
		fatal_perror("unable to delete socket file %s", socket_path);
}

/*
 * Shutdown built-in RPC server.
 * Preserves errno.
 */
void stop_rcmd_server(void)
{
	int sv_errno = errno;

	/* destroy socket file */
	if (sock_unlink)
	{
		sock_unlink = false;
		(void) unlink(socket_path);
	}

	/* close socket */
	if (sock != -1)
	{
		(void) close(sock);
		sock = -1;
	}

	errno = sv_errno;
}


/******************************************************************************
*                     plug RPC server into main poll loop                     *
******************************************************************************/

/*
 * Get the number of poll descriptors required for RPC
 */
int rcmd_get_npollfds(void)
{
	/* RPC connection sockets + listener socket */
	int nrpcs = svc_max_pollfd;
	if (sock != -1)
		nrpcs++;
	return nrpcs;
}

/*
 * Set up poll descriptors required for RPC.
 * The number of entries in the array is per
 * previous rcmd_get_npollfds(...).
 */
void rcmd_setup_pollfds(struct pollfd* pollfds)
{
	struct pollfd* pollfd;
	int k;

	/* RPC connection sockets */
	for (k = 0, pollfd = pollfds;  k < svc_max_pollfd;  k++, pollfd++)
	{
		pollfd->fd = svc_pollfd[k].fd;
		pollfd->events = svc_pollfd[k].events;
		pollfd->revents = 0;
	}

	/* listener socket */
	if (sock != -1)
	{
		pollfd = pollfds + svc_max_pollfd;
		pollfd->fd = sock;
		pollfd->events = POLLIN | POLLPRI | POLLRDNORM | POLLRDBAND;
		pollfd->revents = 0;
	}
}

/*
 * Handle poll(...) completion.
 * Return @true if any event was processed,
 * @false otherwise.
 */
bool rcmd_handle_pollfds(struct pollfd* pollfds)
{
	struct pollfd* pollfd;
	int nfds = svc_max_pollfd;
	int k, fd;
	struct sockaddr_un addr;
	socklen_t addrlen = sizeof addr;
	bool res = false;

	/* any events on RPC connection sockets? */
	for (k = 0;  k < nfds;  k++)
	{
		if (pollfds[k].revents && pollfds[k].fd != -1)
			res = true;
	}

	/* handle events on RPC connection sockets */
	if (res)
	{
		debug_msg(40, "calling svc_getreq_poll");
		svc_getreq_poll(pollfds, nfds);
		debug_msg(40, "finished svc_getreq_poll");
	}

	/* handle events (if any) on the listener socket */
	if (sock != -1)
	{
		pollfd = pollfds + nfds;

		if (pollfd->revents & pollfd->events)
		{
			res = true;
			debug_msg(12, "accepting RPC connection");
			DO_RESTARTABLE(fd, accept(sock, (struct sockaddr*) &addr, &addrlen));
			if (fd == -1)
			{
				error_perror("unable to accept RPC connection");
			}
			else
			{
				debug_msg(12, "accepted RPC connection");
				SVCXPRT* transp = svcunixfd_create(fd, 0, 0);
				if (transp == NULL)
				{
					error_msg("unable to accept RPC connection (svcunixfd_create failed)");
					close(fd);
				}
			}
		}
	}

	return res;
}


/******************************************************************************
*                             RPC service routines                            *
******************************************************************************/

/*
 * Ping
 */
bool_t
rcmd_null_1_svc(void *result, struct svc_req *rqstp)
{
	return true;
}

/*
 * Pause domain memory adjustment
 */
bool_t
rcmd_pause_1_svc(u_int *level, struct svc_req *rqstp)
{
	*level = pause_memsched();
	return true;
}

/*
 * Resume domain memory adjustment
 */
bool_t
rcmd_resume_1_svc(bool_t force, u_int *level, struct svc_req *rqstp)
{
	*level = resume_memsched(force);
	return true;
}

/*
 * Try to bring host free memory up to the requested amount,
 * by shrinking domains if necessary.
 *
 * @above_slack argument is @true if the amount is on top of Xen free memory
 * slack, otherwise false.
 *
 * If @must is true and target result is not achievable, membalance won't
 * even try shrinking the domains.
 *
 * If @use_reserved_hard is true, membalance will draw on host_reserved_hard
 * if necessary.
 *
 * Return the amount of free host memory attained (with and without slack).
 * The amounts are in KBs.
 *
 * If Xen memory is in the state of a flux, e.g. domains are continuing to
 * expand or contract, allow membalance daemon to wait up to @timeout
 * seconds for the flux to stabilize if deemed necessary.
 */
bool_t
rcmd_freemem_1_svc(u_quad_t amt,
		   bool_t above_slack,
		   bool_t use_reserved_hard,
		   bool_t must,
		   int timeout,
		   rcmd_freemem_res *result, struct svc_req *rqstp)
{
	result->status = sched_freemem(amt, above_slack, use_reserved_hard, must,
				       timeout,
				       &result->freemem_with_slack,
				       &result->freemem_less_slack);

	return true;
}

/*
 * Rescan domain and try to make it managed.
 * When returns, the domain may still be in a pending state.
 * If @domain_id is -1, rescan all unmanaged domains.
 */
bool_t
rcmd_manage_domain_1_svc(u_quad_t arg1, rcmd_status *result, struct svc_req *rqstp)
{
	result->status = rescan_domain((long) arg1, &result->message);
	return true;
}

/*
 * Show membalance status (settings, domains etc.).
 * The argument is a verbosity level.
 */
bool_t
rcmd_show_status_1_svc(int arg1, char **result, struct svc_req *rqstp)
{
	*result = show_status(arg1);
	return true;
}

/*
 * Dump debugging state to log file
 */
bool_t
rcmd_debug_dump_1_svc(void *result, struct svc_req *rqstp)
{
	notice_msg("received debug dump request, "
		   "dumping the state to log file (%s) ...",
		   membalanced_log_path);
	show_debugging_info();
	notice_msg("debug dump completed.");
	return true;
}

/*
 * Dump debugging state to a result string
 */
bool_t
rcmd_debug_dump_to_string_1_svc(char **result, struct svc_req *rqstp)
{
	char* buffer = NULL;
	size_t buffer_size = 0;
	FILE* fp = NULL;

	fp = open_memstream(&buffer, &buffer_size);
	if (!fp)
	{
		error_perror("unable to create in-memory file");
		*result = xstrdup("Error: out of memory");
		goto cleanup;
	}

	show_debugging_info(fp);
	fclose(fp);

	*result = buffer ? buffer : xstrdup("Error: out of memory");

cleanup:

	return true;
}

/*
 * Set logging level.
 * If parameter is -1, do not change logging level.
 * Returns previous logging level.
 */
bool_t
rcmd_set_debug_level_1_svc(int arg1, int *result,  struct svc_req *rqstp)
{
	*result = debug_level;
	if (arg1 >= 0)
	{
		debug_level = arg1;
		notice_msg("Setting logging level to %d", debug_level);
	}
	return true;
}

/*
 * Direct logging to membalanced log (parameter = 1)
 * or to syslog (parameter = 0).
 * If parameter is -1, do not change logging sink.
 * Returns previous sink identifier.
 */
bool_t
rcmd_set_logging_sink_1_svc(int arg1, int *result,  struct svc_req *rqstp)
{
	*result = set_log_sink(arg1);
	return true;
}

/*
 * Show domain settings
 */
bool_t
rcmd_get_domain_settings_1_svc(u_quad_t arg1,
			       struct rcmd_domain_settings_res *result,
			       struct svc_req *rqstp)
{
	struct rcmd_kv_listentry* kvp;
	map_ss kvm;

	/* get properties as a key-value map */
	result->status = get_domain_settings((long) arg1, &result->message, kvm);

	/* marshal properties as a key-value list */
	for (map_ss::const_iterator it = kvm.begin();  it != kvm.end();  ++it)
	{
		kvp = (struct rcmd_kv_listentry*) xmalloc(sizeof(*kvp));
		kvp->key = xstrdup(it->first.c_str());
		kvp->value = xstrdup(it->second.c_str());
		kvp->next = result->kvs;
		result->kvs = kvp;
	}

	return true;
}

/*
 * Change domain settings
 */
bool_t
rcmd_set_domain_settings_1_svc(u_quad_t arg1,
			       rcmd_kv_listentry_ptr arg2,
			       struct rcmd_domain_settings_res *result,
			       struct svc_req *rqstp)
{
	/*
	 * not implemented yet
	 */
	result->status = 'X';
	result->message = xstrdup("function is not implemented yet");
	result->kvs = NULL;

	return true;
}

/*
 * Execute development-time test
 */
bool_t
rcmd_test_1_svc(rcmd_str_listentry_ptr arg1, rcmd_status *result, struct svc_req *rqstp)
{
	int argc = 0;
	char** argv = NULL;

	rcmd_str_listentry* p;
	int k;

	for (p = arg1;  p;  p = p->next)
		argc++;

	argv = (char**) xmalloc(sizeof(char*) * (argc + 1));

	for (k = 0, p = arg1;  p;  p = p->next)
		argv[k++] = p->value;
	argv[k] = NULL;

	result->status = execute_test(argc, argv, &result->message);

	/* XDR does not tolerate NULL string pointers */
	if (result->message == NULL)
		result->message = xstrdup("");

	free_ptr(argv);

	return true;
}

/*
 * May insert additional freeing code here, if needed
 */
int
rcmd_membalanced_1_freeresult (SVCXPRT *transp, xdrproc_t xdr_result, caddr_t result)
{
	xdr_free (xdr_result, result);
	/* insert here */
	return 1;
}


