/*
 * This file is appended to rcmd_clnt.c during the build.
 *
 * It is a workaround for a bug in GLIBC that does not allow
 * to set timeout on AF_UNIX sockets via clnt_control(CLSET_TIMEOUT).
 */

void rcmd_clnt_set_timeout(struct timeval* ptv)
{
	TIMEOUT = *ptv;
}

