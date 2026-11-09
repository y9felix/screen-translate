#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void
sig_handler(int sig)
{
	(void)sig;
	running = 0;
}

static int
is_page_turn_key(uint16_t code)
{
	switch (code) {
	case KEY_W:
	case KEY_A:
	case KEY_S:
	case KEY_D:
	case KEY_UP:
	case KEY_DOWN:
	case KEY_LEFT:
	case KEY_RIGHT:
	case KEY_PAGEUP:
	case KEY_PAGEDOWN:
	case KEY_HOME:
	case KEY_END:
	case KEY_ENTER:
	case KEY_BACKSPACE:
	case KEY_J:
	case KEY_K:
	case KEY_H:
	case KEY_L:
	case KEY_SPACE:
	case KEY_TAB:
		return 1;
	default:
		return 0;
	}
}

static int
overlay_is_active(void)
{
	char path[128];
	FILE *f;
	pid_t pid;

	snprintf(path, sizeof(path), "/run/user/%u/screen-translate-overlay.pid", (unsigned int)getuid());
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fscanf(f, "%d", &pid) != 1) {
		fclose(f);
		return 0;
	}
	fclose(f);
	return kill(pid, 0) == 0;
}

static void
arm_timer(int tfd, int ms)
{
	struct itimerspec its;

	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = ms / 1000;
	its.it_value.tv_nsec = (long)(ms % 1000) * 1000000L;
	timerfd_settime(tfd, 0, &its, NULL);
}

static void
disarm_timer(int tfd)
{
	struct itimerspec its;

	memset(&its, 0, sizeof(its));
	timerfd_settime(tfd, 0, &its, NULL);
}

int
main(void)
{
	struct sigaction sa;
	struct pollfd pfds[2];
	struct input_event ev;
	uint64_t expirations;
	int kbd_fd;
	int tfd;
	ssize_t n;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGCHLD, SIG_IGN);

	kbd_fd = open("/dev/input/by-path/platform-i8042-serio-0-event-kbd", O_RDONLY | O_NONBLOCK);
	if (kbd_fd < 0) {
		fprintf(stderr, "failed to open keyboard device: %s\n", strerror(errno));
		return 1;
	}

	tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (tfd < 0) {
		fprintf(stderr, "failed to create timerfd: %s\n", strerror(errno));
		close(kbd_fd);
		return 1;
	}

	pfds[0].fd = kbd_fd;
	pfds[0].events = POLLIN;
	pfds[1].fd = tfd;
	pfds[1].events = POLLIN;

	while (running) {
		if (poll(pfds, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (pfds[0].revents & POLLIN) {
			while ((n = read(kbd_fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
				if (ev.type != EV_KEY || ev.value != 1)
					continue;

				if (ev.code == KEY_ESC) {
					disarm_timer(tfd);
					continue;
				}

				if (!is_page_turn_key(ev.code))
					continue;

				if (ev.code == KEY_SPACE || ev.code == KEY_TAB) {
					if (overlay_is_active())
						continue;
					arm_timer(tfd, 100);
					continue;
				}

				arm_timer(tfd, 100);
			}
		}

		if (pfds[1].revents & POLLIN) {
			if (read(tfd, &expirations, sizeof(expirations)) > 0) {
				if (fork() == 0) {
					int devnull;
					devnull = open("/dev/null", O_RDWR);
					if (devnull >= 0) {
						dup2(devnull, STDIN_FILENO);
						dup2(devnull, STDOUT_FILENO);
						dup2(devnull, STDERR_FILENO);
						close(devnull);
					}
					execl("/usr/local/bin/screen-translate", "screen-translate", (char *)NULL);
					_exit(1);
				}
			}
		}
	}

	close(tfd);
	close(kbd_fd);
	return 0;
}
