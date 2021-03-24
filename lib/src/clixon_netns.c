/*
  * 
  * Network namespace code 
  * @thanks Anders Franzén, especially get_sock() and send_sock() functions
  * 
  * fork, 
  * child: 
  *   switch to ns, 
  *   create sock, 
  *   bind to address, 
  *   sendmsg sock back to parent
  * parent: 
  *   readmsg sock from child
  *   kill child?
  *   return sock
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_SETNS /* linux network namespaces */
#include <sched.h>  /* setns / unshare */
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_netns.h"

/*
 * @thanks Anders Franzén
 */
static int
send_sock(int usock,
	  int fd)
{
    int             retval = -1;
    int            *fdptr;
    struct msghdr   msg={0};
    struct cmsghdr *cmsg;
    char            buf[CMSG_SPACE(sizeof(fd))];

    memset(buf,0,sizeof(buf));
    msg.msg_control=buf;
    msg.msg_controllen=sizeof(buf);
    cmsg=CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level=SOL_SOCKET;
    cmsg->cmsg_type=SCM_RIGHTS;
    cmsg->cmsg_len=CMSG_LEN(sizeof(fd));
    fdptr=(int *)CMSG_DATA(cmsg);
    memcpy(fdptr,&fd,sizeof(fd));
    msg.msg_controllen=CMSG_SPACE(sizeof(fd));
    if (sendmsg(usock, &msg, 0) < 0){
	clicon_err(OE_UNIX, errno, "sendmsg");
	goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*
 * @thanks Anders Franzén
 */
static int
get_sock(int  usock,
	 int *fd)
{
    int             retval = -1;
    struct msghdr   msg={0};
    struct cmsghdr *cmsg;
    char            buf[128];

    msg.msg_iov=0;
    msg.msg_iovlen=0;
    msg.msg_control=buf;
    msg.msg_controllen=sizeof(buf);
    /* Block here */
    if (recvmsg(usock, &msg, 0) < 0){
	clicon_err(OE_UNIX, errno, "recvmsg");
	goto done;
    }
    cmsg=CMSG_FIRSTHDR(&msg);
    memcpy(fd, CMSG_DATA(cmsg), sizeof(*fd));
    retval = 0;
 done:
    return retval;
}

/*! Create and bind stream socket
 * @param[in]  sa       Socketaddress
 * @param[in]  sa_len   Length of sa. Tecynicaliyu to be independent of sockaddr sa_len
 * @param[in]  backlog  Listen backlog, queie of pending connections
 * @param[out] sock     Server socket (bound for accept)
 */
int
create_socket(struct sockaddr *sa,
	      size_t           sin_len,		    
	      int              backlog,
	      int             *sock)
{
    int    retval = -1;
    int    s = -1;
    int    on = 1;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (sock == NULL){
	clicon_err(OE_PROTO, EINVAL, "Requires socket output parameter");
	goto done;
    }
    /* create inet socket */
    if ((s = socket(sa->sa_family,
		    SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
		    0)) < 0) {
	clicon_err(OE_UNIX, errno, "socket");
	goto done;
    }
    if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (void *)&on, sizeof(on)) == -1) {
	clicon_err(OE_UNIX, errno, "setsockopt SO_KEEPALIVE");
	goto done;
    }
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void *)&on, sizeof(on)) == -1) {
	clicon_err(OE_UNIX, errno, "setsockopt SO_REUSEADDR");
	goto done;
    }
    /* only bind ipv6, otherwise it may bind to ipv4 as well which is strange but seems default */
    if (sa->sa_family == AF_INET6 &&
	setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) == -1) {
	clicon_err(OE_UNIX, errno, "setsockopt IPPROTO_IPV6");
	goto done;
    }
    if (bind(s, sa, sin_len) == -1) {
	clicon_err(OE_UNIX, errno, "bind");
	goto done;
    }
    if (listen(s, backlog) < 0){
	clicon_err(OE_UNIX, errno, "listen");
	goto done;
    }
    if (sock)
	*sock = s;
    retval = 0;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    if (retval != 0 && s != -1)
	close(s);
    return retval;
}

/*! Fork a child, create and bind a socket in a separate network namespace and send back to parent
 *
 * @param[in]  netns    Network namespace
 * @param[in]  sa       Socketaddress
 * @param[in]  sa_len   Length of sa. Tecynicaliyu to be independent of sockaddr sa_len
 * @param[in]  backlog  Listen backlog, queie of pending connections
 * @param[out] sock     Server socket (bound for accept)
 */
int
fork_netns_socket(const char      *netns,
		  struct sockaddr *sa,
		  size_t           sin_len,		    
		  int              backlog,
		  int             *sock)
{
    int   retval = -1;
    int   sp[2] = {-1, -1};
    pid_t child;
    int   status = 0;
    char  nspath[MAXPATHLEN]; /* Path to namespace file */
    struct stat st;

    clicon_debug(1, "%s %s", __FUNCTION__, netns);
    if (socketpair(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC, 0, sp) < 0){
	clicon_err(OE_UNIX, errno, "socketpair");
	goto done;
    }
    /* Check namespace exists */
    sprintf(nspath,"/var/run/netns/%s", netns);	
    if (stat(nspath, &st) < 0){
 	clicon_err(OE_UNIX, errno, ": stat(%s)", nspath);
	goto done;
    }
    if ((child = fork()) < 0) {
	clicon_err(OE_UNIX, errno, "fork");
	goto done;
    }
    if (child == 0) {	/* Child */
	int  fd;
	int  s = -1;

	close(sp[0]);
	/* Switch to namespace */
	if ((fd=open(nspath, O_RDONLY)) < 0) {
	    clicon_err(OE_UNIX, errno, "open(%s)", nspath);
	    return -1;
	}
#ifdef HAVE_SETNS
	if (setns(fd, CLONE_NEWNET) < 0){
	    clicon_err(OE_UNIX, errno, "setns(%s)", netns);
	    return -1;
	}
#endif
	close(fd);
	/* Create socket in this namespace */
	if (create_socket(sa, sin_len, backlog, &s) < 0)
	    return -1;
	/* Send socket to parent */
	if (send_sock(sp[1], s) < 0)
	    return -1;
	close(s);
	close(sp[1]);
	exit(0);
    }
    /* Parent */
    close(sp[1]);
    if (get_sock(sp[0], sock) < 0)
	goto done;
    close(sp[0]);
    if(waitpid(child, &status, 0) == child)
	; // retval = WEXITSTATUS(status); /* Dont know what to do with status */
    retval = 0;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    return retval;
}

/*! Create and bind stream socket in network namespace
 * @param[in]  netns    Network namespace
 * @param[in]  sa       Socketaddress
 * @param[in]  sa_len   Length of sa. Tecynicaliyu to be independent of sockaddr sa_len
 * @param[in]  backlog  Listen backlog, queie of pending connections
 * @param[out] sock     Server socket (bound for accept)
 */
int
clixon_netns_socket(const char      *netns,
		    struct sockaddr *sa,
		    size_t           sin_len,		    
		    int              backlog,
		    int             *sock)
{
    int    retval = -1;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (netns == NULL){
	if (create_socket(sa, sin_len, backlog, sock) < 0)
	    goto done;
	goto ok;
    }
    else {
#ifdef HAVE_SETNS
	if (fork_netns_socket(netns, sa, sin_len, backlog, sock) < 0)
	    goto done;
#else
	clicon_err(OE_UNIX, errno, "No namespace support on platform: %s", netns);
	return -1;	
#endif
    }
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    return retval;
}
