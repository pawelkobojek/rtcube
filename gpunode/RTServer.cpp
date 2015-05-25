#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <iostream>

#include "../proto/proto.h"
#include "../util/HostPort.h"

#include "../cudacore/RTCubeApi.h"
#include "RTServer.h"

using namespace std;

#define ERR(source) (perror(source),\
		     fprintf(stderr, "%s:%d\n", __FILE__, __LINE__),\
		     exit(EXIT_FAILURE))
#define BACKLOG 3
#define DEBUG_INFO false

volatile sig_atomic_t do_work = 1;

void sigint_handler(int sig)
{
	do_work = 0;
}

int sethandler(void (*f)(int), int sigNo)
{
	struct sigaction act;
	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = f;
	if (-1 == sigaction(sigNo, &act, NULL))
		return -1;
	return 0;
}

int make_socket(int domain, int type)
{
	auto sock = socket(domain, type, 0);
	if (sock < 0) ERR("socket");
	return sock;
}

int bind_inet_socket(char *hostport, int type)
{
	auto dest = HostPort{hostport};
	int t = 1;
	sockaddr_in6 addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	memcpy(&addr.sin6_addr, dest.ip, 16);
	addr.sin6_port = dest.port;
	auto socketfd = make_socket(PF_INET6, type);
	if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &t, sizeof(t))) ERR("setsockopt");
	if (bind(socketfd, (sockaddr*) &addr, sizeof(addr)) < 0)  ERR("bind");
	if (SOCK_STREAM == type)
		if (listen(socketfd, BACKLOG) < 0) ERR("listen");
	return socketfd;
}

ssize_t bulk_read(int fd, char *buf, size_t count)
{
	int c;
	size_t len = 0;
	do {
		c = TEMP_FAILURE_RETRY(read(fd, buf, count));
		if (c < 0) return c;
		if (0 == c) return len;
		buf += c;
		len += c;
		count -= c;
	} while (count > 0);
	return len;
}

ssize_t bulk_write(int fd, char *buf, size_t count)
{
	int c;
	size_t len = 0;
	do {
		c = TEMP_FAILURE_RETRY(write(fd, buf, count));
		if (c < 0) return c;
		buf += c;
		len += c;
		count -= c;
	} while (count > 0);
	return len;
}

int accept_client(int sfd)
{
	int nfd;
	if ((nfd = TEMP_FAILURE_RETRY(accept(sfd, NULL, NULL))) < 0)
	{
		if (EAGAIN == errno || EWOULDBLOCK == errno) return -1;
		ERR("accept");
	}
	return nfd;
}

void communicateStream(int cfd)
{
	ssize_t size;
	int32_t data[5];
	if ((size = bulk_read(cfd, (char *)data, sizeof(int32_t[5]))) < 0) ERR("read:");
	if (size == (int)sizeof(int32_t[5]))
	{
		//calculate(data);
		if (bulk_write(cfd, (char *)data, sizeof(int32_t[5])) < 0 && errno != EPIPE) ERR("write:");
	}
	if (TEMP_FAILURE_RETRY(close(cfd)) < 0) ERR("close");
}

void communicateDgram(int fd)
{
	sockaddr addr;
	socklen_t addr_len;
	char buffer[2048];
	int len = 0;
	if ((len = TEMP_FAILURE_RETRY(recvfrom(fd, buffer, 2048, 0, (::sockaddr*) &addr, &addr_len))) < 0) ERR("read:");

	auto msg = string{buffer, string::size_type(len)};
	auto V = proto::unserialize(msg);

	if (V[0] == "hello")
	{
		std::cout << "Generator connected" << endl;
	}
	else if (V[0] == "status")
	{
		cubeStatus();
	}
	else if (V[0] == "query")
	{
		cubeQuery();
	}
	else
	{
		if (DEBUG_INFO)
		{
			std::cout << "Insert: ";
			for (auto v : V)
			{
				try
				{
					std::cout << int(v) << " "; // cout is required, so compiler won't optimize int() away.
				}
				catch (std::domain_error&) {}
			}
			std::cout << std::endl;
		}

		int size = V.size();
		int *values = (int*)malloc(size * sizeof(int));
		for (auto i = 0; i < size; ++i)
			values[i] = V[i];
		cubeInsert(values, size);
		free(values);
	}
}

void doServer(int fd_tcp, int fd_udp)
{
	sigset_t mask, oldmask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, &oldmask);
	int fdmax = (fd_tcp > fd_udp ? fd_tcp : fd_udp);
	while (do_work)
	{
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd_tcp, &rfds);
		FD_SET(fd_udp, &rfds);
		int cfd = -1;
		if (pselect(fdmax + 1, &rfds, NULL, NULL, NULL, &oldmask) > 0)
		{
			if (FD_ISSET(fd_tcp, &rfds))
			{
				cfd = accept_client(fd_tcp);
				if (cfd >= 0) communicateStream(cfd);
			}
			if (FD_ISSET(fd_udp, &rfds)) {
				communicateDgram(fd_udp);
			}
		}
		else
		{
			if (EINTR == errno) continue;
			ERR("pselect");
		}
	}
	sigprocmask (SIG_UNBLOCK, &mask, NULL);
}

int RunServers(char *hostaddr_tcp, char *hostaddr_udp)
{
	int fd_tcp,fd_udp;
	int new_flags;

	if (sethandler(SIG_IGN, SIGPIPE)) ERR("Seting SIGPIPE:");
	if (sethandler(sigint_handler, SIGINT)) ERR("Seting SIGINT:");

	fd_tcp = bind_inet_socket(hostaddr_tcp, SOCK_STREAM);
	new_flags = fcntl(fd_tcp, F_GETFL) | O_NONBLOCK;
	fcntl(fd_tcp, F_SETFL, new_flags);

	fd_udp = bind_inet_socket(hostaddr_udp, SOCK_DGRAM);
	doServer(fd_tcp, fd_udp);

	if (TEMP_FAILURE_RETRY(close(fd_tcp)) < 0) ERR("close");
	if (TEMP_FAILURE_RETRY(close(fd_udp)) < 0) ERR("close");

	fprintf(stderr, "Server has terminated.\n");

	return EXIT_SUCCESS;
}