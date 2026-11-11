#define _GNU_SOURCE

#include <err.h>
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

#define KBDDEV "/dev/input/by-path/platform-i8042-serio-0-event-kbd"
#define PIDFMT "/run/user/%u/screen-translate-overlay.pid"

static int ispageturnkey(uint16_t code);
static int overlayactive(void);
static void settimer(int tfd, int ms);
static void sighandler(int sig);
static void spawntranslate(void);

static volatile sig_atomic_t running = 1;

static int
ispageturnkey(uint16_t code)
{
	static const uint16_t keys[] = {
		KEY_W, KEY_A, KEY_S, KEY_D,
		KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
		KEY_PAGEUP, KEY_PAGEDOWN, KEY_HOME, KEY_END,
		KEY_ENTER, KEY_BACKSPACE,
		KEY_J, KEY_K, KEY_H, KEY_L,
		KEY_SPACE, KEY_TAB
	};
	size_t i;

	for (i = 0; i < sizeof(keys) / sizeof(*keys); i++)
		if (keys[i] == code)
			return 1;
	return 0;
}

static int
overlayactive(void)
{
	char path[64];
	FILE *fp;
	pid_t pid;
	int active;

	active = 0;
	snprintf(path, sizeof(path), PIDFMT, (unsigned int)getuid());
	if (!(fp = fopen(path, "r")))
		return 0;
	if (fscanf(fp, "%d", &pid) == 1 && pid > 0)
		active = !kill(pid, 0);
	fclose(fp);
	return active;
}

static void
settimer(int tfd, int ms)
{
	struct itimerspec its;

	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = ms / 1000;
	its.it_value.tv_nsec = (long)(ms % 1000) * 1000000L;
	timerfd_settime(tfd, 0, &its, NULL);
}

static void
sighandler(int sig)
{
	(void)sig;
	running = 0;
}

static void
spawntranslate(void)
{
	pid_t pid;
	int fd;

	if ((pid = fork()) != 0)
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
	int kfd, tfd;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sighandler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGCHLD, SIG_IGN);

	if ((kfd = open(KBDDEV, O_RDONLY | O_NONBLOCK)) < 0)
		err(1, "open %s", KBDDEV);

	if ((tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) < 0) {
		close(kfd);
		err(1, "timerfd_create");
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
					settimer(tfd, 0);
					continue;
				}

				if (!ispageturnkey(ev.code))
					continue;

				if ((ev.code == KEY_SPACE || ev.code == KEY_TAB) && overlayactive())
					continue;

				settimer(tfd, 100);
			}
		}

		if (pfds[1].revents & POLLIN) {
			if (read(tfd, &exp, sizeof(exp)) > 0)
				spawntranslate();
		}
	}

	close(tfd);
	close(kfd);
	return 0;
}
