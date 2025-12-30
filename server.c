#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>

#define PORT "3490"  // the port users will be connecting to
#define BACKLOG 10   // how many pending connections queue will hold
#define BUFFER_SIZE 65536

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

void *handle_client(void *arg) {

	int client_fd = (int)(intptr_t)arg;
	char buffer[BUFFER_SIZE];
	
	ssize_t bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

	if (bytes_received <= 0) {
		perror("recv");
		close(client_fd);
		return NULL;
	}	
	buffer[bytes_received] = '\0';

	char method[16], uri[256], protocol[16];

	sscanf(buffer, "%15s %255s %15s", method, uri, protocol);
	printf("Request: %s %s\n", method, uri);

	char filepath[512];
	if (strcmp(uri, "/") == 0) {
		sprintf(filepath, "www/index.html");
	} else {
		sprintf(filepath, "www%s", uri);
	}

	FILE *file = fopen(filepath, "rb");
	if (!file) {
		perror("404 Not Found");
		send_404(client_fd);
		close(client_fd);
		return NULL;
	}

	fseek(file, 0, SEEK_END);
	long file_size = ftell(file);
	fseek(file, 0, SEEK_SET);

	const char *mime_type = get_mime_type(filepath);

	char header[1024];
	int header_len = snprintf(header, sizeof(header),
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: %s\r\n"
			"Content-Length: %ld\r\n"
			"\r\n",
			mime_type, file_size);
	send(client_fd, header, header_len, 0);
	
	size_t bytes_read;
	while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
		send(client_fd, buffer, bytes_read, 0);
	}
	
	fclose(file);
	close(client_fd);
	return NULL;
}

int main(void) {

	// listen on sock_fd, new connection on new_fd
	int sockfd, new_fd;
	struct addrinfo hints, *servinfo, *p;
	struct sockaddr_storage their_addr; // connector's address info
	socklen_t sin_size;
	int yes=1;
	char s[INET6_ADDRSTRLEN];
	int rv;

	signal(SIGPIPE, SIG_IGN);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE; // use my IP

	if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and bind to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("server: socket");
			continue;
		}

		if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
				sizeof(int)) == -1) {
			perror("setsockopt");
			exit(1);
		}

		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("server: bind");
			continue;
		}

		break;
	}

	freeaddrinfo(servinfo); // all done with this structure

	if (p == NULL)  {
		fprintf(stderr, "server: failed to bind\n");
		exit(1);
	}

	if (listen(sockfd, BACKLOG) == -1) {
		perror("listen");
		exit(1);
	}

	printf("server: waiting for connections...\n");

	while(1) {  // main accept() loop
		
		sin_size = sizeof their_addr;
		new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
		if (new_fd == -1) {
			perror("accept");
			continue;
		}

		inet_ntop(their_addr.ss_family,
			get_in_addr((struct sockaddr *)&their_addr), s, sizeof s);
		printf("server: got connection from %s\n", s);

		// create the thread
		pthread_t thread_id;
		if (pthread_create(&thread_id, NULL, handle_client, (void*)(intptr_t)new_fd) != 0) {
			perror("pthred_create");
			close(new_fd);
			continue;
		}

		pthread_detach(thread_id);
	}
	return 0;
}
