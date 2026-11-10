#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define KBD_DEV "/dev/input/by-path/platform-i8042-serio-0-event-kbd"
#define PID_FMT "/run/user/%u/screen-translate-overlay.pid"

static void eprintf(const char *fmt, ...);
static int is_page_turn_key(uint16_t code);
static int overlay_active(void);
static void set_timer(int tfd, int ms);
static void sig_handler(int sig);
static void spawn_translate(void);

static volatile sig_atomic_t running = 1;

static const uint16_t page_keys[] = {
	KEY_W, KEY_A, KEY_S, KEY_D,
	KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
	KEY_PAGEUP, KEY_PAGEDOWN, KEY_HOME, KEY_END,
	KEY_ENTER, KEY_BACKSPACE,
	KEY_J, KEY_K, KEY_H, KEY_L,
	KEY_SPACE, KEY_TAB
};

static void
eprintf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(1);
}

static int
is_page_turn_key(uint16_t code)
{
	size_t i;

	for (i = 0; i < sizeof(page_keys) / sizeof(page_keys[0]); i++)
		if (page_keys[i] == code)
			return 1;
	return 0;
}

static int
overlay_active(void)
{
	char path[64];
	FILE *fp;
	pid_t pid;
	int active = 0;

	snprintf(path, sizeof(path), PID_FMT, (unsigned int)getuid());
	if (!(fp = fopen(path, "r")))
		return 0;
	if (fscanf(fp, "%d", &pid) == 1)
		active = (kill(pid, 0) == 0);
	fclose(fp);
	return active;
}

static void
set_timer(int tfd, int ms)
{
	struct itimerspec its;

	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = ms / 1000;
	its.it_value.tv_nsec = (long)(ms % 1000) * 1000000L;
	timerfd_settime(tfd, 0, &its, NULL);
}

static void
sig_handler(int sig)
{
	(void)sig;
	running = 0;
}

static void
spawn_translate(void)
{
	int fd;

	if (fork() != 0)
		return;
	if ((fd = open("/dev/null", O_RDWR)) >= 0) {
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		close(fd);
	}
	execl("/usr/local/bin/screen-translate", "screen-translate", (char *)NULL);
	_exit(1);
}

int
main(void)
{
	struct sigaction sa;
	struct pollfd pfds[2];
	struct input_event ev;
	uint64_t exp;
	int kfd;
	int tfd;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGCHLD, SIG_IGN);

	if ((kfd = open(KBD_DEV, O_RDONLY | O_NONBLOCK)) < 0)
		eprintf("failed to open keyboard device: %s\n", strerror(errno));

	if ((tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) < 0) {
		close(kfd);
		eprintf("failed to create timerfd: %s\n", strerror(errno));
	}

	pfds[0].fd = kfd;
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
			while (read(kfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
				if (ev.type != EV_KEY || ev.value != 1)
					continue;

				if (ev.code == KEY_ESC) {
					set_timer(tfd, 0);
					continue;
				}

				if (!is_page_turn_key(ev.code))
					continue;

				if ((ev.code == KEY_SPACE || ev.code == KEY_TAB) && overlay_active())
					continue;

				set_timer(tfd, 100);
			}
		}

		if (pfds[1].revents & POLLIN) {
			if (read(tfd, &exp, sizeof(exp)) > 0)
				spawn_translate();
		}
	}

	close(tfd);
	close(kfd);
	return 0;
}
