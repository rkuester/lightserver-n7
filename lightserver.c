#include <systemd/sd-daemon.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/fcntl.h>
#include <time.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

static int socket_from_systemd()
{
	/* Return a fd > 0 if given a socket, 0 if not, and -1 on error. */

	int nr = sd_listen_fds(1);
	if (nr == 1)
		return SD_LISTEN_FDS_START + 0;
	else if (nr == 0)
		return 0;
	else {
		fprintf(stderr, "Too many descriptors passed by systemd.\n");
		return -1;
	}
}

static int create_own_socket()
{
	/* Return a fd or -1 on error. */

	struct sockaddr_un address;
	char *ADDRESS = "/run/lightserver.socket";
	int rc;

	rc = unlink(ADDRESS);
	if (rc == -1 && errno != ENOENT) {
		perror("unlink()");
		return -1;
	}

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		perror("socket()");
		return -1;
	}

	memset(&address, 0, sizeof address);
	address.sun_family = AF_UNIX;
	strncpy(address.sun_path, ADDRESS, sizeof address.sun_path - 1);

	rc = bind(fd, (struct sockaddr *) &address, sizeof address);
	if (rc == -1) {
		perror("bind()");
		return -1;
	}

	rc = listen(fd, 1);
	if (rc == -1) {
		perror("listen()");
		return -1;
	}

	return fd;
}

static int timer_start(long long period_ns)
{
	int timer, rc;
	struct itimerspec period;

	timer = timerfd_create(CLOCK_MONOTONIC, 0);
	if (timer == -1) {
		perror("timerfd_create()");
		return -1;
	}

	memset(&period, 0, sizeof period);
	period.it_value.tv_nsec = period_ns;
	period.it_interval.tv_nsec = period_ns;
	rc = timerfd_settime(timer, 0, &period, 0);
	if (rc == -1) {
		perror("timerfd_settime()");
		return -1;
	}

	return timer;
}

static void timer_stop(int timer)
{
	close(timer);
}

static int timer_wait(int timer)
{
	uint64_t expirations;
	int rc = read(timer, &expirations, sizeof expirations);
	if (rc == -1) {
		perror("timer read()");
		return -1;
	}

	return 0;
}

static int read_lux(char *s, int capacity)
{
	const char *SYSFS_ATTRIBUTE = "/sys/bus/i2c/drivers/al3010/2-001c/show_revise_lux";
	int fd;
	int rc;
	
	fd = open(SYSFS_ATTRIBUTE, O_RDONLY, O_NONBLOCK);
	if (fd == -1) {
		perror("sysfs open()");
		return -1;
	}

	rc = read(fd, s, capacity - 1);
	if (rc == -1) {
		perror("sysfs read()");
		return -1;
	}

	return 0;
}

static int serve(int fd)
{
	const long long DATA_RATE_NS = 100LL * 1000LL * 1000LL; /* 100 ms */
	int timer;
	int rc;

	rc = fcntl(fd, F_SETFL, O_NONBLOCK);
	if (rc == -1) {
		perror("fcntl()");
		return -1;
	}

	timer = timer_start(DATA_RATE_NS); /* Stop before returning. */
	if (timer == -1)
		return -1;

	for (;;) {
		const int CAPACITY = 80;
		char message[CAPACITY];
		char *cursor;
		struct timeval now;
		long long ms;
		memset(message, 0, CAPACITY);

		rc = timer_wait(timer);
		if (rc == -1)
			break;

		rc = gettimeofday(&now, 0);
		if (rc == -1)
			break;

		rc = read_lux(message, CAPACITY);
		if (rc == -1)
			break;

		ms = now.tv_sec * 1000LL + now.tv_usec / 1000LL;
		cursor = strchr(message, '\n');
		snprintf(cursor, CAPACITY - (cursor - message), " %lli\n", ms);

		rc = send(fd, message, strnlen(message, CAPACITY), MSG_NOSIGNAL);
		if (rc == -1) {
			if (errno == EAGAIN) {
				fprintf(stderr, "write() blocked, closing");
				rc = 0;
			} else if (errno == EPIPE) {
				rc = 0;
			} else
				perror("send()");

			break;
		}
	}

	timer_stop(timer);
	return rc;
}

int main(int argc, char** argv)
{
	int server_fd;

	server_fd = socket_from_systemd();
	if (server_fd == 0)
		server_fd = create_own_socket();
	if (server_fd < 0)
		exit(EXIT_FAILURE);

	for (;;) {
		int client_fd, rc;

		client_fd = accept(server_fd, 0, 0);
		if (client_fd == -1) {
			perror("accept()");
			exit(EXIT_FAILURE);
		}

		rc = serve(client_fd);
		close(client_fd);
		if (rc == -1)
			exit(EXIT_FAILURE);
	}
}
