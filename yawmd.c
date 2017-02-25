/* yawmd.c: yawm daemon
 *
 * Track windows lifecycle to facilitate desktop layout recovery upon yawm
 * restart. The layout information is only valid for active display server
 * session.
 *
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 *
 */

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <sys/stat.h>

#include <xcb/xcb.h>

#include "misc.h"
#include "list.h"
#include "yawm.h"

static const char *home;
static xcb_connection_t *dpy;
static const char *disp;

struct client {
	struct list_head head;
	struct clientinfo info;
};

#define list2client(item) list_entry(item, struct client, head)

struct list_head clients; /* keep track of all clients */

static int check_window(xcb_window_t win)
{
	xcb_get_window_attributes_cookie_t c;
	xcb_get_window_attributes_reply_t *a;

	c = xcb_get_window_attributes(dpy, win);
	a = xcb_get_window_attributes_reply(dpy, c, NULL);
	if (!a)
		return -1;

	free(a);
	return 0;
}

static void client_add(struct clientinfo *info)
{
	struct client *cli = malloc(sizeof(struct client));

	if (!cli) {
		ee("malloc(%lu) failed\n", sizeof(struct client));
		return;
	}

	cli->info.win = info->win;
	cli->info.scr = info->scr;
	cli->info.tag = info->tag;
	list_add(&clients, &cli->head);
}

static void save_list(uint8_t clean)
{
	int fd;
	struct list_head *cur, *tmp;
	int flags = O_CREAT | O_WRONLY | O_TRUNC | O_NONBLOCK;
	mode_t mode = S_IRUSR | S_IWUSR;

	if ((fd = open(YAWM_LIST, flags, mode)) < 0) {
		ee("open(%s/"YAWM_BASE"/"YAWM_LIST") failed\n", home);
		return;
	}

	ii("save winlist %s/"YAWM_BASE"/"YAWM_LIST"\n", home);

	list_walk_safe(cur, tmp, &clients) {
		struct client *cli = list2client(cur);
		off_t pos = write(fd, &cli->info, sizeof(cli->info));
		if (pos > 0 && pos != sizeof(cli->info)) { /* rewind back */
			lseek(fd, -pos, SEEK_CUR);
			ww("failed to store win 0x%x\n", cli->info.win);
		} else if (pos < 0) {
			ee("failed to store win 0x%x\n", cli->info.win);
		} else {
			ii("> scr %d, tag %d, win 0x%x\n", cli->info.scr,
			   cli->info.tag, cli->info.win);
		}

		if (clean) {
			list_del(&cli->head);
			free(cli);
		}
	}

	close(fd);
}

static void update_list(struct clientreq *req)
{
	const char *action[] = { "clean", "update", "store", };
	uint8_t i = 2;
	struct list_head *cur, *tmp;

	if (req->type != REQTYPE_CLEAN && check_window(req->info.win) < 0) {
		ww("ignore invalid window 0x%x\n", req->info.win);
		return;
	}

	list_walk_safe(cur, tmp, &clients) {
		struct client *cli = list2client(cur);
		if (cli->info.win == req->info.win) {
			if (req->type == REQTYPE_CLEAN) {
				i = 0;
				list_del(&cli->head);
				free(cli);
			} else {
				i = 1;
				cli->info.win = req->info.win;
				cli->info.scr = req->info.scr;
				cli->info.tag = req->info.tag;
			}
			goto out;
		}
	}

	client_add(&req->info);
out:
	ii("%s info: scr %d, tag %d, win 0x%x\n", action[i], req->info.scr,
	   req->info.tag, req->info.win);
}

static int open_fifo(const char *path, int fd)
{
	mode_t mode = S_IRUSR | S_IWUSR;

	if (fd >= 0) /* re-open fifo */
		close(fd);

remake:
	if (mkfifo(path, mode) < 0) {
		if (errno == EEXIST) {
			unlink(path);
			goto remake;
		}

		mode = errno;
		ee("open(%s) failed: %s\n", path, strerror(errno));
		errno = mode;
		return -1;
	}

	return open(path, O_RDONLY | O_NONBLOCK, S_IRUSR);
}

static void handle_signal(int signum)
{
	if (signum == SIGUSR1)
		ii("SIGUSR1\n");
}

static void yawmd(void)
{
	struct clientreq req;
	struct pollfd fds;
	struct sigaction sa;

	if (!(disp = getenv("DISPLAY"))) {
		ee("DISPLAY is not set\n");
		return;
	}

	if (!(home = getenv("HOME"))) {
		ee("HOME directory is not set\n");
		return;
	}

	if (chdir(home) < 0) {
		ee("chdir(%s) failed\n", home);
		return;
	}

	if (chdir(YAWM_BASE) < 0) {
		ee("chdir(%s"YAWM_BASE") failed\n", home);
		return;
	}

	list_init(&clients);

	fds.fd = open_fifo(YAWM_FIFO, -1);
	if (fds.fd < 0)
		return;
	fds.events = POLLIN;

	sa.sa_handler = &handle_signal;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &sa, NULL); /* interrupt poll */

	ii("%s, %s/"YAWM_BASE"/"YAWM_FIFO": waiting for data\n", disp, home);

	while (1) {
		memset(&req, 0, sizeof(req));
		fds.revents = 0;
		errno = 0;
		poll(&fds, 1, -1);
		if (fds.revents & POLLERR || errno == EINTR) {
			fds.fd = open_fifo(YAWM_FIFO, fds.fd);
			continue;
		} else if (!(fds.revents & POLLIN)) {
			if (fds.revents & POLLHUP)
				fds.fd = open_fifo(YAWM_FIFO, fds.fd);
			continue;
		}

		if (read(fds.fd, &req, sizeof(req)) != sizeof(req)) {
			errno = EMSGSIZE;
			ee("message size does not match (%lu)\n", sizeof(req));
			fds.fd = open_fifo(YAWM_FIFO, fds.fd);
			continue;
		}

		if (req.type == REQTYPE_RESET)
			save_list(1); /* save and clean */
		else if (req.type == REQTYPE_FLUSH)
			save_list(0); /* save don't clean */
		else
			update_list(&req);
	}

	close(fds.fd);
}

int main(int argc, char *argv[])
{
	int rc;

	dpy = xcb_connect(NULL, NULL);
	if (!dpy) {
		ee("xcb_connect() failed\n");
		return 1;
	}

	if (argc < 2) {
		yawmd();
	} else if (argv[1] && strcmp(argv[1], "-d") == 0) {
		if ((rc = fork()) < 0) {
			rc = errno;
			ee("fork() failed\n");
		} else if (rc > 0) {
			ii("forked with pid %d\n", rc);
			rc = 0;
		} else {
			fclose(stderr);
			fclose(stdout);
			setsid();
			yawmd();
			rc = errno;
		}
	}

	return rc;
}
