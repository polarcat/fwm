/* deskd.c: desktop daemon
 *
 * Provides http interface to desktop and system settings.
 *
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "misc.h"

#define HTTP_REQ_MAXLINES 20
#define HTTP_GET_MAXLEN sizeof("GET / HTTP/1.1")
#define HTTP_URL_MAXLEN 1024

#ifndef DESKD_BACKLOG
#define DESKD_BACKLOG 1
#endif

#ifndef CGI_TIMEOUT
#define CGI_TIMEOUT 30000 /* ms */
#endif

static int timeout;

#define MAXBUF 64

struct context {
	const char *ip;
	int sd;
	uint8_t buf[MAXBUF + 1];
	char *path;
	pid_t pid;
};

struct response {
	char *str;
	int len;
};

enum httphdr {
	HTTP_HDR_RESP = 0,
	HTTP_HDR_CONN,
	HTTP_HDR_SIZE,
	HTTP_HDR_TYPE,
	HTTP_HDR_DATA,
	HTTP_HDR_MAX,
};

static const struct response rfmt[HTTP_HDR_MAX] = {
	{ "HTTP/1.1 %3d\n", 0, },
	{ "Connection: %s%c%c", 0, },
	{ "Content-Length: %d%c", 0, }, /* byte overhead */
	{ "Content-Type: %s%s%c%c%ctext/plain;charset=utf-8", 0, },
	{ "<!DOCTYPE html><html><body>Error %3d</body></html>", 0, },
};

static struct response resp[HTTP_HDR_MAX];
static int client = -1;

static int match(const char *buf, const char *str, int len)
{
	int ret;

	memcmp(buf, str, len) == 0 ? (ret = 1) : (ret = 0);
	return ret;
}

static const char *gettype(const char *buf)
{
	unsigned char png[] = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, };
	unsigned char gif[] = { 0x47, 0x49, 0x46, 0x38, 0x61, 0x37, 0x39, };
	unsigned char jpg[] = { 0xff, 0xd8, 0xff, 0xe0, 0xe1, 0xdb, };

	if (match(buf, png, sizeof(png))) {
		return "image/png";
	} else if (match(buf, gif, sizeof(gif))) {
		return "image/gif";
	} else if (match(buf, jpg, sizeof(jpg))) {
		return "image/jpg";
	} else if (match(buf, "<!DOCTYPE html>", sizeof("<!DOCTYPE html>") - 1)) {
		return "text/html";
	} else {
		return "text/plain"; /* ... let's have it this way */
	}
}

static void setcode(struct iovec *iov, int code)
{
	int len;
	char *str = resp[HTTP_HDR_RESP].str;
	char *fmt = rfmt[HTTP_HDR_RESP].str;

	len = sprintf(str, fmt, code);
	iov->iov_base = str;
	iov->iov_len = len;
}

static void setconn(struct iovec *iov, const char *conn)
{
	int len = resp[HTTP_HDR_CONN].len;
	char *str = resp[HTTP_HDR_CONN].str;
	char *fmt = rfmt[HTTP_HDR_CONN].str;

	snprintf(str, len, fmt, conn, '\n', '\0');
	iov->iov_base = str;
	iov->iov_len = strlen(str);
}

static void setsize(struct iovec *iov, int size)
{
	int len;
	char *str = resp[HTTP_HDR_SIZE].str;
	char *fmt = rfmt[HTTP_HDR_SIZE].str;

	len = sprintf(str, fmt, size, '\n');
	iov->iov_base = str;
	iov->iov_len = len;
}

static void settype(struct iovec *iov, const char *type)
{
	char *str = resp[HTTP_HDR_TYPE].str;
	char *fmt = rfmt[HTTP_HDR_TYPE].str;

	sprintf(str, fmt, type, "; charset=utf-8", '\n', '\n', '\0');
	iov->iov_base = str;
	iov->iov_len = strlen(str);
}

static void setfail(struct iovec *iov, int err)
{
	int len;
	char *str = resp[HTTP_HDR_DATA].str;
	char *fmt = rfmt[HTTP_HDR_DATA].str;

	len = sprintf(str, fmt, err);
	iov->iov_base = str;
	iov->iov_len = len;
}

static void seterror(struct iovec *iov, int err)
{
	setcode(&iov[HTTP_HDR_RESP], err);
	setconn(&iov[HTTP_HDR_CONN], "close");
	setsize(&iov[HTTP_HDR_SIZE], strlen(rfmt[HTTP_HDR_DATA].str));
	settype(&iov[HTTP_HDR_TYPE], "text/html");
	setfail(&iov[HTTP_HDR_DATA], err);
}

static void setok(struct iovec *iov, const char *type, int size)
{
	setcode(&iov[HTTP_HDR_RESP], 200);
	setconn(&iov[HTTP_HDR_CONN], "close");
	setsize(&iov[HTTP_HDR_SIZE], size);
	settype(&iov[HTTP_HDR_TYPE], type);
}

static void pusherror(int sd, int err, struct iovec *iov, int cnt)
{
	if (client < 0)
		return; /* error has already been pushed */

	client = -1;
	seterror(iov, err);
	pushv(sd, iov, cnt);
}

static inline int umin32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static void waitcgi(int signum)
{
	int status = 0;

	while (waitpid(-1, &status, WNOHANG) < 0) {
		if (errno != EINTR)
			break;
	}

	if (status) { /* push internal server error */
		struct iovec iov[HTTP_HDR_MAX] = { 0 };
		pusherror(client, 500, iov, ARRAY_SIZE(iov));
	}
}

static void forkcgi(char *cmd, char *arg, int *srv, int *cli)
{
	if (fork() != 0)
		return;

	if (dup2(srv[1], STDOUT_FILENO) < 0) {
		ee("dup2(%d, %d) failed\n", srv[1], STDOUT_FILENO);
		exit(1);
	}
	close(srv[0]);

	if (dup2(cli[0], STDIN_FILENO) < 0) {
		ee("dup2(%d, %d) failed\n", cli[0], STDIN_FILENO);
		exit(1);
	}
	close(cli[1]);

	execl(cmd, cmd, arg, NULL);
	exit(0);
}

static int runcgi(struct context *ctx, char *req, int16_t reqlen)
{
	int srv[2];
	int cli[2];
	char uuid[sizeof("4294967295")];
	char *rep = ctx->buf;
	int16_t replen = MAXBUF;

	if (pipe(srv) < 0 || pipe(cli) < 0) {
		ee("pipe() failed\n");
		return 500; /* internal server error */
	}

	dd("pipe srv[%d,%d], cli[%d,%d]\n", srv[0], srv[1], cli[0], cli[1]);

	snprintf(uuid, sizeof(uuid), "%d", getpid());
	forkcgi("./cgi", uuid, srv, cli);

	/* handle cgi handshake */

	dd("pull handshake ..\n");
	if (pull(srv[0], rep, replen, CGI_TIMEOUT) < 0) {
		ee("pull(%d, %d) failed\n", srv[1], replen);
		return 503; /* service unavailable */
	}

	if (!(*rep == 'O' && *(rep + 1) == 'K')) {
		errno = EBADE;
		*(rep + 2) = '\0';
		ee("cgi not ready, rep '%s'\n", rep);
		return 502; /* bad gateway */
	}
	dd("cgi ready\n");

	/* handle cgi payload */

	dd("push '%s' len %d\n", req, reqlen);
	if (push(cli[1], req, reqlen) < 0) {
		ee("push(%s, %d) failed\n", req, reqlen);
		return 503; /* service unavailable */
	}
	close(cli[1]);

	/* handle cgi reply */

	dd("pull data ..\n");
	*rep = '\0';
	if (pull(srv[0], rep, replen, CGI_TIMEOUT) < 0) {
		ee("pull() %d bytes failed\n", replen);
		return 503; /* service unavailable */
	}

	char *tmp = memchr(rep, '\n', replen);
	if (tmp)
		*tmp = '\0';
	dd("got rep '%s', len %d\n", rep, replen);
	ctx->path = rep;
	return 0;
}

#define iscgi(c) (c[1] == 'c' && c[2] == 'g' && c[3] == 'i' && c[4] == '?')

static int parseget(struct context *ctx, char *buf, int16_t buflen)
{
	int16_t reqlen;
	char *req, *tmp;

	req = memchr(buf, '/', buflen);
	if (!req || !(reqlen = buflen - (req - buf)))
		return 400; /* bad request */

	tmp = memchr(req, ' ', reqlen);
	if (!tmp)
		return 400; /* bad request */
	else if (!(reqlen = tmp - req))
		return 400; /* bad request */
	*tmp = '\0'; /* not interested in the rest */

	if (match(req, "/files/", sizeof("/files/") - 1)) {
		ctx->path = req + 1; /* skip leading '/' */
		dd("direct link %s\n", ctx->path);
		return 0;
	} else if ((*req == '/' && *(req + 1) == '\0') || iscgi(req)) {
		return runcgi(ctx, req, reqlen);
	}
}

#define isrun(c) (c[0] == 'p' && c[1] == 'o' && c[2] == 's' && c[3] == 't')

static int parsepost(struct context *ctx, struct request *reqptr,
		     struct request *reqend)
{
	char *str;
	uint16_t len;

	while (reqptr <= reqend) {
		str = reqptr->str;
		len = umin32(reqptr->len, sizeof("Content-Type:") - 1);
		if (!match(str, "Content-Type:", len)) {
			reqptr++;
			continue;
		} /* don't care about Content-Length for now */

		/* last line supposed to be request body */
		str = (reqend - 1)->str;
		len = (reqend - 1)->len;
		if (isrun(str))
			return runcgi(ctx, str, len);
		return 400; /* bad request */
	}

	return 415; /* unsupported media type */
}

#ifdef HAVE_SENDFILE
#include <sys/sendfile.h>
static int pushfile(struct context *ctx, struct iovec *iov)
{
	unsigned char buf[8];
	struct stat st;
	int fd, ret;

	if (ctx->path[0] == '\0' ||
	    (ctx->path[0] == '.' && ctx->path[1] == '.'))
		return -1;

	fd = open(ctx->path, O_RDONLY);
	if (fd < 0) {
		ee("open(%s) failed\n", ctx->path);
		return -1;
	}
	fstat(fd, &st);

	eval(read(fd, buf, sizeof(buf)) != sizeof(buf), ret = -1; goto out);

	setok(iov, gettype(buf), st.st_size);
	eval(pushv(ctx->sd, iov, HTTP_HDR_MAX - 1) < 0, ret = -1; goto out);

	lseek(fd, 0, SEEK_SET);
	ret = sendfile(ctx->sd, fd, 0, st.st_size);
	tt("sd %d, fd %d, size %d, ret %d\n", ctx->sd, fd, st.st_size, ret);
	if (ret != st.st_size)
		ret = -1;
	else
		ret = 0;
out:
	close(fd);
	return ret;
}
#else /* ! HAVE_SENDFILE */

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

static int pushfile(struct context *ctx, struct iovec *iov)
{
	struct stat st;
	int fd, chunks, rem, offs, ret;

	if (ctx->path[0] == '\0' ||
	    (ctx->path[0] == '.' && ctx->path[1] == '.'))
		return -1;

	fd = open(ctx->path, O_RDONLY);
	if (fd < 0) {
		ee("open(%s) failed\n", ctx->path);
		return -1;
	}
	fstat(fd, &st);

	chunks = st.st_size / PAGE_SIZE;
	rem = st.st_size % PAGE_SIZE;
	if (rem > 0)
		chunks++;

	tt("need to send %d bytes in %d chunks, rem %d bytes\n",
	   st.st_size, chunks, rem);

#define iobase iov[HTTP_HDR_DATA].iov_base
#define iosize iov[HTTP_HDR_DATA].iov_len

	if (chunks == 1 && rem < PAGE_SIZE)
		iosize = rem;
	else
		iosize = PAGE_SIZE;

	iobase = mmap(NULL, PAGE_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
	eval(!iobase, ret = -1; goto out);

	dd("file: %s\n", ctx->path);
	char *ptr = ctx->path + strlen(ctx->path);
	if (*(ptr - 4) == '.' &&
	    *(ptr - 3) == 'c' && *(ptr - 2) == 's' && *(ptr - 1) =='s')
		setok(iov, "text/css", st.st_size);
	else
		setok(iov, gettype(iobase), st.st_size);

	ret = pushv(ctx->sd, iov, HTTP_HDR_MAX);
	munmap(iobase, iosize);
	eval(ret < 0, ret = -1; goto out);

	if (iosize != rem) {
		offs = PAGE_SIZE;
		while (--chunks) {
			if (chunks == 1 && rem < PAGE_SIZE)
				iosize = rem;
			else
				iosize = PAGE_SIZE;

			iobase = mmap(NULL, PAGE_SIZE, PROT_READ, MAP_PRIVATE,
				      fd, offs);
			ret = push(ctx->sd, iobase, iosize);
			munmap(iobase, iosize);
			eval(ret != iosize, ret = -1; goto out);

			offs += PAGE_SIZE;
		}
	}

	ret = 0;
out:
	close(fd);
	return ret;
}
#endif /* HAVE_SENDFILE */

static int parsereq(struct context *ctx)
{
	struct request req[HTTP_REQ_MAXLINES];
	struct request *reqptr, *reqend;
	char buf[HTTP_URL_MAXLEN] = { 0 };
	char *str;
	int cnt, len;

	cnt = pullstr(ctx->sd, buf, sizeof(buf) - 1, req, ARRAY_SIZE(req));
	if (cnt < 1)
		return -1; /* no data */
	reqptr = req;
	reqend = reqptr + cnt;
	while (reqptr < reqend) {
		str = reqptr->str;
		len = reqptr->len;
		ii("req %s, len %d\n", str, len);
		if (match(str, "GET", sizeof("GET") - 1)) {
			return parseget(ctx, str, len);
		} else if (match(str, "POST", sizeof("POST") - 1)) {
			return parsepost(ctx, reqptr, reqend);
		}
		reqptr++;
	}
	return 405; /* method not allowed */
}

static void work(struct context *ctx)
{
	int err;
	struct iovec iov[HTTP_HDR_MAX];

	ii("handle cli %d, ip %s\n", ctx->sd, ctx->ip);

	eval(signal(SIGCHLD, waitcgi) == SIG_ERR, return);

	memset(iov, 0, sizeof(iov));
	memset(ctx->buf, 0, sizeof(ctx->buf));

	if (!(err = parsereq(ctx))) {
		if (pushfile(ctx, iov) < 0)
			pusherror(ctx->sd, 404, iov, ARRAY_SIZE(iov)); /* not found */
	} else if (err > 0) {
		pusherror(ctx->sd, err, iov, ARRAY_SIZE(iov));
	}

	tt("done\n");
}

static int initresponse(void)
{
	int i;

	for (i = 0; i < HTTP_HDR_MAX; i++) {
		int len = strlen(rfmt[i].str);
		resp[i].str = calloc(1, len);
		eval(!resp[i].str, goto clean);
		strcpy(resp[i].str, rfmt[i].str);
		resp[i].len = len;
	}

	return 0;
clean:
	for (;i >= 0;i--) {
		free(resp[i].str);
		resp[i].str = NULL;
		resp[i].len = 0;
	}
	return -1;
}

#define reuseaddr(sd, opt) \
	setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (char *) &opt, sizeof(opt));

int main(int argc, char *argv[])
{
	const char *basedir, *portstr;
	const char *timeoutstr;
	int n, srv, tmp, port;
	pid_t pid;
	struct sockaddr_in addr;
	struct context ctx;

	if ((timeoutstr = getenv("DESKD_TIMEOUT")))
		timeout = atoi(timeoutstr) * 1000;
	if (!timeout)
		timeout = CGI_TIMEOUT;

	eval(!(portstr = getenv("DESKD_PORT")), return 1);
	port = atoi(portstr);
	eval(!(basedir = getenv("DESKD_BASE")), return 1);

	if (chdir(basedir) < 0)
		return 1;
	else if (initresponse() < 0)
		return 1;

	signal(SIGCHLD, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	eval((srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0, return 1);

	tmp = 1;
	reuseaddr(srv, tmp);

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	eval(bind(srv, (struct sockaddr *) &addr, sizeof(addr)) < 0, return 1);
	eval(listen(srv, DESKD_BACKLOG) < 0, return 1);

	ii("%d: waiting for clients on port %d ...\n", getpid(), port);
	while (1) {
		tmp = sizeof(addr);
		ctx.sd = accept(srv, (struct sockaddr *) &addr, (socklen_t *) &tmp);
		if (ctx.sd < 0) {
			ee("accept(%d) failed\n", srv);
			return 1;
		}

		reuseaddr(ctx.sd, tmp);
		ctx.ip = inet_ntoa(addr.sin_addr);

		pid = fork();
		if (pid < 0) {
			ee("fork() failed\n");
		} else if (pid == 0) {
			client = ctx.sd;
			ctx.pid = getpid();
			ii("%d: connection from %s\n", ctx.pid, ctx.ip);
			close(srv);
			work(&ctx);
			close(ctx.sd);
			return 0;
		}

		tt("close cli %d\n", ctx.sd);
		close(ctx.sd);
	}

	return 0;
}
