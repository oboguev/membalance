/*
 *  MEMBALANCE daemon
 *
 *  util.cpp - Utility routines
 *
 *  Portions Copyright (C) 2014 Sergey Oboguev (oboguev@yahoo.com)
 *  For licensing terms see license.txt
 */

#include "membalanced.h"

/******************************************************************************
*                             local definitions                               *
******************************************************************************/

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


/******************************************************************************
*                                static data                                  *
******************************************************************************/

static clockid_t clk_id;		  /* clock to use for timing (CLOCK_BOOTTIME etc.) */


/******************************************************************************
*                                time routines                                *
******************************************************************************/

/*
 * Select preferred interval clock among available ones
 */
void select_clk_id(void)
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
 * Get current timestamp.
 * Preserves previous value of errno.
 */
struct timespec getnow(void)
{
	int sv_errno = errno;
	struct timespec ts;

	if (clock_gettime(clk_id, &ts))
		fatal_perror("clock_gettime");

	errno = sv_errno;
	return ts;
}

/*
 * Return (ts1 - ts0) in milliseconds
 */
int64_t timespec_diff_ms(const struct timespec& ts1, const struct timespec& ts0)
{
	int64_t ms = (int64_t) ts1.tv_sec - (int64_t) ts0.tv_sec;
	ms *= MSEC_PER_SEC;
	ms += ((int64_t) ts1.tv_nsec - (int64_t) ts0.tv_nsec) / NSEC_PER_MSEC;
	return ms;
}

/******************************************************************************
*                                   logging                                   *
******************************************************************************/

/*
 * Log/print error message (incl. errno) and terminate
 */
void fatal_perror(const char* fmt, ...) /* __noreturn__  __format_printf__ */
{
	char* errno_msg;
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
void fatal_msg(const char* fmt, ...) /* __noreturn__  __format_printf__ */
{
	va_list ap;
	va_start(ap, fmt);

	if (log_fp)
	{
		print_timestamp(log_fp);
		vfprintf(log_fp, fmt, ap);
		fputc('\n', log_fp);
		fflush(log_fp);
	}

	if (log_syslog)
		vsyslog(LOG_ERR, fmt, ap);

	va_end(ap);
	test_debugger();
	::terminate();
}

/*
 * Log/print error message (incl. errno)
 */
void error_perror(const char* fmt, ...)
{
	char* errno_msg;
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
void error_msg(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);

	if (log_fp)
	{
		print_timestamp(log_fp);
		vfprintf(log_fp, fmt, ap);
		fputc('\n', log_fp);
		fflush(log_fp);
	}

	if (log_syslog)
		vsyslog(LOG_ERR, fmt, ap);

	va_end(ap);
	test_debugger();
}

/*
 * Log/print warning message (incl. errno)
 */
void warning_perror(const char* fmt, ...)
{
	char* errno_msg;
	char errno_msg_buf[128];
	char msg[512];
	int size = 0;
	va_list ap;

	errno_msg = strerror_r(errno, errno_msg_buf, countof(errno_msg_buf));

	strcpy(msg, "warning: ");

	size = strlen(msg);
	va_start(ap, fmt);
	vsnprintf(msg + size, countof(msg) - size, fmt, ap);
	va_end(ap);

	size = strlen(msg);
	snprintf(msg + size, countof(msg) - size, ": %s", errno_msg);

	warning_msg("%s", msg);
}

/*
 * Log/print warning message
 */
void warning_msg(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);

	if (log_fp)
	{
		print_timestamp(log_fp);
		vfprintf(log_fp, fmt, ap);
		fputc('\n', log_fp);
		fflush(log_fp);
	}

	if (log_syslog)
		vsyslog(LOG_WARNING, fmt, ap);

	va_end(ap);
	test_debugger();
}

/*
 * Log/print notice message
 */
void notice_msg(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);

	if (log_fp)
	{
		print_timestamp(log_fp);
		vfprintf(log_fp, fmt, ap);
		fputc('\n', log_fp);
		fflush(log_fp);
	}

	if (log_syslog)
		vsyslog(LOG_NOTICE, fmt, ap);

	va_end(ap);
}

/*
 * Log/print "out of memory" message and terminate
 */
void out_of_memory(void) /* __noreturn__  __format_printf__ */
{
	fatal_msg("out of memory");
}

/*
 * Print out a timestamp before logging a record
 */
void print_timestamp(FILE* fp)
{
	if (!log_timestamps)
		return;

	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);

	char tms[64];
	strftime(tms, countof(tms), "%F %T", &tm);
	fprintf(fp, "[%s] ", tms);
}

/*
 * Return proper plural-form suffix for message printing
 */
const char* plural(long n)
{
	return (n > 1) ? "s" : "";
}


/******************************************************************************
*                                     misc                                    *
******************************************************************************/

/*
 * Read signal information from signal fd
 */
void read_siginfo(int fd, struct signalfd_siginfo* fdsi)
{
	ssize_t sz = read(fd, fdsi, sizeof(*fdsi));
	if (sz < 0)
		fatal_perror("read signalfd");
	if (sz != sizeof(*fdsi))
		fatal_msg("read signalfd");
}

/*
 * Duplicate a string.
 * Abort if out of memory.
 */
char* xstrdup(const char* cp)
{
	if (cp == NULL)
		return NULL;
	char* p = strdup(cp);
	if (!p)
		out_of_memory();
	return p;
}

/*
 * Allocate memory.
 * Abort if out of memory.
 */
void* xmalloc(size_t size)
{
	void* p = malloc(size);
	if (!p)
		out_of_memory();
	return p;
}

/*
 * Format into an allocated string.
 * Result must eventually be free'd.
 */
char* xprintf(const char* fmt, ...)
{
	char buf[512];
	char* bp = buf;
	int bsize = countof(buf);
	va_list ap;
	int n;

	for (;;)
	{
		va_start(ap, fmt);
		n = vsnprintf(bp, bsize, fmt, ap);
		va_end(ap);

		if (n < bsize)
			break;

		bsize = n + 1;
		if (bp != buf)
			free(bp);
		bp = (char*) xmalloc(bsize);
	}

	if (n < 0)
		fatal_msg("bug: xprintf(\"%s\")", fmt);

	if (bp == buf)
		bp = xstrdup(buf);

	return bp;
}

/*
 * kv[key] = sprintf(fmt, ...)
 */
void setfmt(map_ss& kv, const char* key, const char* fmt, ...)
{
	char buf[512];
	char* bp = buf;
	int bsize = countof(buf);
	va_list ap;
	int n;

	for (;;)
	{
		va_start(ap, fmt);
		n = vsnprintf(bp, bsize, fmt, ap);
		va_end(ap);

		if (n < bsize)
			break;

		bsize = n + 1;
		if (bp != buf)
			free(bp);
		bp = (char*) xmalloc(bsize);
	}

	if (n < 0)
		fatal_msg("bug: setfmt(\"%s\")", fmt);

	set_kv(kv, key, bp);

	if (bp != buf)
		free(bp);
}

bool starts_with(const char* s, const char* prefix)
{
	return 0 == strncmp(s, prefix, strlen(prefix));
}

