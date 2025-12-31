#define _GNU_SOURCE

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
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>

#define PORT "3490"
#define MAX_EVENTS 1024
#define BACKLOG 1024
#define REQUEST_BUFFER_SIZE 8192
#define HEADER_BUFFER_SIZE 1024
#define CONNECTION_POOL_SIZE 10000
#define CONNECTION_TIMEOUT 5

#ifndef EPOLLEXCLUSIVE
#define EPOLLEXCLUSIVE (1U << 28)
#endif

typedef enum {
    STATE_READ_REQUEST,
    STATE_PARSE_REQUEST,
    STATE_SEND_HEADER,
    STATE_SEND_FILE,
    STATE_DONE
} conn_state_t;

struct connection_t;

typedef struct connection_t {
    int fd;
    conn_state_t state;
    int in_use;
    time_t last_activity;

    struct connection_t *prev;
    struct connection_t *next;

    char request_buffer[REQUEST_BUFFER_SIZE];
    size_t request_len;

    char method[16];
    char uri[256];
    char filepath[PATH_MAX];

    char response_header[HEADER_BUFFER_SIZE];
    size_t header_len;
    size_t header_sent;

    int file_fd;
    off_t file_size;
    off_t file_offset;
} connection_t;

connection_t connection_pool[CONNECTION_POOL_SIZE];
connection_t *free_list[CONNECTION_POOL_SIZE];
int free_list_count = CONNECTION_POOL_SIZE;

connection_t *active_connections = NULL;
connection_t *active_connections_tail = NULL;

char www_root_path[PATH_MAX];

void init_connection_pool(void) {
    for (int i = 0; i < CONNECTION_POOL_SIZE; i++) {
        memset(&connection_pool[i], 0, sizeof(connection_t));
        connection_pool[i].in_use = 0;
        connection_pool[i].file_fd = -1;
        free_list[i] = &connection_pool[i];
    }
}

void list_add(connection_t *conn) {
    conn->next = active_connections;
    conn->prev = NULL;
    if (active_connections) {
        active_connections->prev = conn;
    } else {
        active_connections_tail = conn;
    }
    active_connections = conn;
}

void list_remove(connection_t *conn) {
    if (conn->prev) {
        conn->prev->next = conn->next;
    } else {
        active_connections = conn->next;
    }
    
    if (conn->next) {
        conn->next->prev = conn->prev;
    } else {
        active_connections_tail = conn->prev;
    }
    
    conn->next = NULL;
    conn->prev = NULL;
}

void list_move_to_tail(connection_t *conn) {
    if (conn == active_connections_tail) {
        return;
    }
    
    if (conn->prev) {
        conn->prev->next = conn->next;
    } else {
        active_connections = conn->next;
    }
    
    if (conn->next) {
        conn->next->prev = conn->prev;
    }
    
    conn->next = NULL;
    conn->prev = active_connections_tail;
    
    if (active_connections_tail) {
        active_connections_tail->next = conn;
    } else {
        active_connections = conn;
    }
    
    active_connections_tail = conn;
}

connection_t *pool_alloc_connection(void) {
    if (free_list_count <= 0) return NULL;
    connection_t *conn = free_list[--free_list_count];
    memset(conn, 0, sizeof(connection_t));
    conn->in_use = 1;
    conn->file_fd = -1;
    return conn;
}   

void pool_free_connection(connection_t *conn) {
    if (!conn->in_use) return;

    list_remove(conn);

    conn->in_use = 0;
    if (free_list_count < CONNECTION_POOL_SIZE) {
        free_list[free_list_count++] = conn;
    }
}

int url_decode(const char *src, char *dest, size_t dest_size) {
    const char *p = src;
    char *d = dest;
    char *dest_end = dest + dest_size - 1; 
    
    while (*p && d < dest_end) {
        if (*p == '%') {
            if (p[1] && p[2] && isxdigit(p[1]) && isxdigit(p[2])) {
                char code[3] = {p[1], p[2], 0};
                *d++ = (char)strtol(code, NULL, 16);
                p += 3;
            } else {
                return -1;
            }
        } else if (*p == '+') {
            *d++ = ' ';
            p++;
        } else {
            *d++ = *p++;
        }
    }
    
    if (*p != '\0') return -1;
    *d = '\0';
    return 0;
}

int set_nonblocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) return -1;
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;
    return 0;
}

const char *get_mime_type(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return "text/plain";
    
    switch(dot[1]) {
        case 'h':
            if (strcmp(dot, ".html") == 0) return "text/html";
            break;
        case 'c':
            if (strcmp(dot, ".css") == 0) return "text/css";
            break;
        case 'j':
            if (strcmp(dot, ".js") == 0) return "application/javascript";
            if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) 
                return "image/jpeg";
            break;
        case 'p':
            if (strcmp(dot, ".png") == 0) return "image/png";
            break;
        case 'g':
            if (strcmp(dot, ".gif") == 0) return "image/gif";
            break;
    }
    return "text/plain";
}

void send_404_and_close(connection_t *conn, int epoll_fd) {
    const char *response = 
        "HTTP/1.1 404 NOT FOUND\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "Connection: close\r\n"
        "\r\n"
        "404 Not Found";

    send(conn->fd, response, strlen(response), 0);
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    close(conn->fd);
    if (conn->file_fd >= 0) close(conn->file_fd);
    pool_free_connection(conn);
}

connection_t *create_connection(int client_fd) {
    connection_t *conn = pool_alloc_connection();
    if (!conn) return NULL;

    conn->fd = client_fd;
    conn->state = STATE_READ_REQUEST;
    conn->last_activity = time(NULL);

    list_add(conn);

    return conn;
}

void destroy_connection(connection_t *conn, int epoll_fd) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    close(conn->fd);
    if (conn->file_fd >= 0) close(conn->file_fd);
    
    pool_free_connection(conn);
}

int parse_and_prepare_response(connection_t *conn) {
    
    if (sscanf(conn->request_buffer, "%15s %255s", conn->method, conn->uri) != 2) return -1;
    
    if (strcmp(conn->method, "GET") != 0) return -1;

    char decoded_uri[256];
    if (url_decode(conn->uri, decoded_uri, sizeof(decoded_uri)) < 0) {
        return -1; 
    }

    char tentative_path[PATH_MAX];
    if (strcmp(decoded_uri, "/") == 0) {
        snprintf(tentative_path, sizeof(tentative_path), "www/index.html");
    } else {
        snprintf(tentative_path, sizeof(tentative_path), "www%s", decoded_uri);
    }

    if (realpath(tentative_path, conn->filepath) == NULL) {
        return -1;
    }

    if (strncmp(conn->filepath, www_root_path, strlen(www_root_path)) != 0) {
        return -1; 
    }

    conn->file_fd = open(conn->filepath, O_RDONLY);
    if (conn->file_fd < 0) return -1;

    struct stat file_stat;
    if (fstat(conn->file_fd, &file_stat) < 0) {
        close(conn->file_fd);
        conn->file_fd = -1;
        return -1;
    }
    conn->file_size = file_stat.st_size;

    const char *mime_type = get_mime_type(conn->filepath);
    conn->header_len = snprintf(conn->response_header, sizeof(conn->response_header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: keep-alive\r\n"
        "Keep-Alive: timeout=%d, max=100\r\n"
        "\r\n",
        mime_type, conn->file_size, CONNECTION_TIMEOUT);

    conn->header_sent = 0;
    conn->file_offset = 0;

    return 0;
}

int handle_read_request(connection_t *conn) {
    while (1) {
        size_t remain = REQUEST_BUFFER_SIZE - conn->request_len - 1;
        if (remain == 0) return -1;

        ssize_t n = recv(conn->fd, conn->request_buffer + conn->request_len, remain, 0);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (strstr(conn->request_buffer, "\r\n\r\n") != NULL) return 1;
                return 0;
            }
            return -1;
        }

        if (n == 0) return -1;

        conn->request_len += n;
        conn->request_buffer[conn->request_len] = '\0';
        conn->last_activity = time(NULL);
        list_move_to_tail(conn);

        if (strstr(conn->request_buffer, "\r\n\r\n") != NULL) return 1;
    }
}

int handle_send_header(connection_t *conn) {
    while (conn->header_sent < conn->header_len) {
        ssize_t n = send(conn->fd, conn->response_header + conn->header_sent, conn->header_len - conn->header_sent, 0);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }

        conn->header_sent += n;
        conn->last_activity = time(NULL);
        list_move_to_tail(conn);
    }
    return 1;
}

int handle_send_file(connection_t *conn) {
    while (conn->file_offset < conn->file_size) {
        ssize_t n = sendfile(conn->fd, conn->file_fd, &conn->file_offset, conn->file_size - conn->file_offset);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            return -1;
        }

        if (n == 0) {
            break;
        }
        conn->last_activity = time(NULL);
        list_move_to_tail(conn);
    }
    return 1;
}

void reset_for_next_request(connection_t *conn) {
    conn->last_activity = time(NULL);
    list_move_to_tail(conn); 

    char *end_of_header = strstr(conn->request_buffer, "\r\n\r\n");
    if (!end_of_header) {
        conn->request_len = 0;
        conn->state = STATE_READ_REQUEST;
        return;
    }

    size_t header_size = (end_of_header - conn->request_buffer) + 4;
    size_t leftover = conn->request_len - header_size;

    if (leftover > 0) {
        memmove(conn->request_buffer, conn->request_buffer + header_size, leftover);
        conn->request_len = leftover;
        conn->request_buffer[conn->request_len] = '\0';

        if (strstr(conn->request_buffer, "\r\n\r\n") != NULL) {
            conn->state = STATE_PARSE_REQUEST;
        } else {
            conn->state = STATE_READ_REQUEST;
        }
    } else {
        conn->request_len = 0;
        conn->state = STATE_READ_REQUEST;
    }
}

void handle_connection(connection_t *conn, int epoll_fd, uint32_t events) {
    struct epoll_event ev;
    int result;
    int process_next_request = 0;

    do {
        process_next_request = 0;

        if (events & EPOLLIN) {
            if (conn->state == STATE_READ_REQUEST) {
                result = handle_read_request(conn);
                if (result < 0) {
                    destroy_connection(conn, epoll_fd);
                    return;
                }
                if (result == 1) {
                    conn->state = STATE_PARSE_REQUEST;
                }
            }
        }

        if (conn->state == STATE_PARSE_REQUEST) {
            if (parse_and_prepare_response(conn) < 0) {
                send_404_and_close(conn, epoll_fd);
                return;
            }

            int flag = 1;
            setsockopt(conn->fd, IPPROTO_TCP, TCP_CORK, &flag, sizeof(flag));

            conn->state = STATE_SEND_HEADER;
            events |= EPOLLOUT;

            ev.events = EPOLLOUT | EPOLLET;
            ev.data.ptr = conn;
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
        }

        if (events & EPOLLOUT) {
            if (conn->state == STATE_SEND_HEADER) {
                result = handle_send_header(conn);
                if (result < 0) {
                    destroy_connection(conn, epoll_fd);
                    return;
                }
                if (result == 1) {
                    conn->state = STATE_SEND_FILE;
                }
            }

            if (conn->state == STATE_SEND_FILE) {
                result = handle_send_file(conn);
                if (result < 0) {
                    destroy_connection(conn, epoll_fd);
                    return;
                }
                if (result == 1) {
                    int flag = 0;
                    setsockopt(conn->fd, IPPROTO_TCP, TCP_CORK, &flag, sizeof(flag));

                    close(conn->file_fd);
                    conn->file_fd = -1;

                    reset_for_next_request(conn);

                    if (conn->state == STATE_PARSE_REQUEST) {
                        process_next_request = 1;
                        events = 0;
                    } else {
                        ev.events = EPOLLIN | EPOLLET;
                        ev.data.ptr = conn;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
                    }
                }
            }
        }

    } while (process_next_request);

    if (events & (EPOLLERR | EPOLLHUP)) {
        destroy_connection(conn, epoll_fd);
    }
}

void run_worker(int listen_fd, int worker_id) {
    time_t last_sweep = 0;

    printf("Worker %d (PID %d) starting on port %s...\n", worker_id, getpid(), PORT);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        exit(1);
    }

    struct epoll_event ev, events[MAX_EVENTS];
    
    ev.events = EPOLLIN | EPOLLEXCLUSIVE;
    ev.data.fd = listen_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror("epoll_ctl: listen_fd");
        exit(1);
    }

    while(1) {  
        int n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);

        if (n_events == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == listen_fd) {
                while (1) {
                    struct sockaddr_storage client_addr;
                    socklen_t addr_len = sizeof(client_addr);

                    int client_fd = accept4(listen_fd,
                                          (struct sockaddr *)&client_addr,
                                          &addr_len,
                                          SOCK_NONBLOCK | SOCK_CLOEXEC);

                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        perror("accept4");
                        break;
                    }

                    int flag = 1;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                              &flag, sizeof(flag));

                    connection_t *conn = create_connection(client_fd);
                    if (!conn) {
                        fprintf(stderr, "Connection pool exhausted!\n");
                        close(client_fd);
                        continue;
                    }

                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.ptr = conn;

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                        perror("epoll_ctl: client_fd");
                        pool_free_connection(conn);
                        close(client_fd);
                        continue;
                    }
                }
            }
            else {
                connection_t *conn = (connection_t *)events[i].data.ptr;
                handle_connection(conn, epoll_fd, events[i].events);
            }
        }

        time_t now = time(NULL);
        if (now - last_sweep >= 1) {
            connection_t *curr = active_connections;
            while (curr) {
                connection_t *next = curr->next;
                
                if ((now - curr->last_activity) <= CONNECTION_TIMEOUT) {
                    break; 
                }
                
                destroy_connection(curr, epoll_fd);
                curr = next;
            }
            last_sweep = now;
        }
    }

    close(epoll_fd);
    exit(0);
}

int main(void) {
    int listen_fd;
    struct addrinfo hints, *servinfo, *p;
    int yes=1;
    int rv;

    if (realpath("www", www_root_path) == NULL) {
        perror("Error resolving 'www' directory. Make sure it exists");
        return 1;
    }
    printf("Web root resolved to: %s\n", www_root_path);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

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
            perror("socket");
            continue;
        }

        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes,
                sizeof(int)) == -1) {
            perror("setsockopt");
            close(listen_fd);
            continue;
        }

        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &yes, 
                sizeof(int)) == -1) {
            perror("setsockopt SO_REUSEPORT");
            close(listen_fd);
            continue;
        }

        if (set_nonblocking(listen_fd) == -1) {
            perror("set_nonblocking");
            close(listen_fd);
            continue;
        }

        if (bind(listen_fd, p->ai_addr, p->ai_addrlen) == -1) {
            close(listen_fd);
            perror("bind");
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo);

    if (p == NULL)  {
        fprintf(stderr, "server: failed to bind\n");
        return 1;
    }

    if (listen(listen_fd, BACKLOG) == -1) {
        perror("listen");
        return 1;
    }

    int num_workers = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_workers <= 0) num_workers = 4;
    
    printf("Starting %d worker processes...\n", num_workers);

    init_connection_pool();
    printf("Initialized connection pool with %d connections per worker\n", CONNECTION_POOL_SIZE);

    for (int i = 0; i < num_workers; i++) {
        pid_t pid = fork();
        
        if (pid == -1) {
            perror("fork");
            continue;
        }
        
        if (pid == 0) {
            run_worker(listen_fd, i);
        }
    }

    printf("Server started with %d workers on port %s\n", num_workers, PORT);
    
    while(1) {
        wait(NULL);
    }

    close(listen_fd);
    return 0;
}
