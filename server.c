#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define PORT "3490"
#define MAX_EVENTS 10
#define BACKLOG 10
#define BUFFER_SIZE 65536

// makes a non-blocking socket
int set_nonblocking(int sockfd) {

	int flags = fcntl(sockfd, F_GETFL, 0);
	if (flags == -1) return -1;
	if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;
	return 0;
}

// get content-type
const char *get_mime_type(const char *filename) {

	const char *dot = strrchr(filename, '.');
	if (!dot) return "text/plain";
	if (strcmp(dot, ".html") == 0) return "text/html";
	if (strcmp(dot, ".css") == 0) return "text/css";
	if (strcmp(dot, ".js") == 0) return "application/javascript";
	if (strcmp(dot, ".jpg") == 0) return "image/jpeg";
	if (strcmp(dot, ".png") == 0) return "image/png";
	if (strcmp(dot, ".gif") == 0) return "image/gif";
	return "text/plain";
}

// 404 response
void send_404(int client_fd) {

	const char *response = "HTTP/1.1 404 NOT FOUND\r\n"
						   "Content-Type: text/plain\r\n"
						   "Content-Length: 13\r\n"
						   "\r\n"
						   "404 Not Found";

	send(client_fd, response, strlen(response), 0);
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

void handle_client(int client_fd, int epoll_fd) {

	char buffer[BUFFER_SIZE];
	ssize_t bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

	if (bytes_received < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			perror("recv");
			epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
			close(client_fd);
		}
		return;
	}

	if (bytes_received == 0) {
		epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
		close(client_fd);
		return;
	}

	buffer[bytes_received] = '\0';

	char method[16], uri[256], protocol[16];

	sscanf(buffer, "%15s %255s %15s", method, uri, protocol);
	printf("Request: %s %s\n", method, uri);

	// security check for directory traversal
	if (strstr(uri, "..")) {
		send_404(client_fd);
		epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
		close(client_fd);
		return;
	}

	char filepath[512];
	if (strcmp(uri, "/") == 0) {
		sprintf(filepath, "www/index.html");
	} else {
		sprintf(filepath, "www%s", uri);
	}

	int file_fd = open(filepath, O_RDONLY);
	if (file_fd < 0) {
		send_404(client_fd);
		epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
		close(client_fd);
		return;
	}

	struct stat file_stat;
	if (fstat(file_fd, &file_stat) < 0) {
		perror("fstat");
		close(file_fd);
		epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
		close(client_fd);
		return;
	}
	off_t file_size = file_stat.st_size;

	const char *mime_type = get_mime_type(filepath);
	char header[1024];
	int header_len = snprintf(header, sizeof(header),
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: %s\r\n"
			"Content-Length: %ld\r\n"
			"\r\n",
			mime_type, file_size);
	send(client_fd, header, header_len, 0);
	
	off_t offset = 0;
	ssize_t sent_bytes = 0;

	while (offset < file_size) {

		ssize_t res = sendfile(client_fd, file_fd, &offset, file_size - offset);

		if (res < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
			perror("sendfile");
			break;
		}
		sent_bytes += res;
	}
	
	close(file_fd);
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
	close(client_fd);
}

int main(void) {

	int listen_fd, new_fd;
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_storage their_addr;
	socklen_t sin_size;
	int yes=1;
	int rv;

	signal(SIGPIPE, SIG_IGN);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((listen_fd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("server: socket");
			continue;
		}

		if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes,
				sizeof(int)) == -1) {
			perror("setsockopt");
			exit(1);
		}

		if (set_nonblocking(listen_fd) == -1) {
			perror("set_nonblocking");
			exit(1);
		}

		if (bind(listen_fd, p->ai_addr, p->ai_addrlen) == -1) {
			close(listen_fd);
			perror("server: bind");
			continue;
		}

		break;
	}

	freeaddrinfo(servinfo);

	if (p == NULL)  {
		fprintf(stderr, "server: failed to bind\n");
		exit(1);
	}

	if (listen(listen_fd, BACKLOG) == -1) {
		perror("listen");
		exit(1);
	}

	int epoll_fd = epoll_create1(0);
	if (epoll_fd == -1) {
		perror("epoll_create1");
		exit(1);
	}

	struct epoll_event event;
	struct epoll_event events[MAX_EVENTS];

	event.data.fd = listen_fd;
	event.events = EPOLLIN;

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) == -1) {
		perror("epoll_ctl: listen_fd");
		exit(1);
	}

	printf("server: waiting for connections (epoll mode)...\n");

	while(1) {  // main accept() loop
		
		int n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

		if (n_events == -1) {
			perror("epoll_wait");
			exit(1);
		}

		for (int i = 0; i < n_events; i++) {
		
			if (events[i].data.fd == listen_fd) {
				sin_size = sizeof their_addr;
				new_fd = accept(listen_fd, (struct sockaddr *)&their_addr, &sin_size);
				if (new_fd == -1) {
					perror("accept");
					continue;
				}

				if (set_nonblocking(new_fd) == -1) {
					perror("set_nonblocking");
					exit(1);
				}

				event.data.fd = new_fd;
				event.events = EPOLLIN;
				if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_fd, &event) == -1) {
                    perror("epoll_ctl: new_fd");
                    exit(1);
                }
			} else {
				handle_client(events[i].data.fd, epoll_fd);
			}
		} 
	}

	close(listen_fd);
	return 0;
}
