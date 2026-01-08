# Pure C Server

This is a pure C server using no external dependencies(other than standard C libraries). Shoutout to Beej's Guide to Network Programming for getting me started and teaching me. This is currently only set
to run on Linux. Originally it was for Windows but while exploring ways to improve the code I decided to make use of epoll and a few other Linux specific system calls(fork(), sendfile(), ...). I also now see
why Linux is used for most servers, replicating this on Windows does not look enjoyable.

---

## Implementation

### Architecture
* **Pre-forked Worker Model**: Upon startup, the server forks multiple worker processes based on the number of available CPU cores (`_SC_NPROCESSORS_ONLN`) to distribute incoming load.
* **Event-Driven I/O**: Each worker manages a dedicated `epoll` instance to monitor up to 1024 events simultaneously in non-blocking mode.
* **Kernel-Level Load Balancing**: Utilizes `SO_REUSEPORT` and `EPOLLEXCLUSIVE` to allow the Linux kernel to efficiently distribute new connections across the worker pool.



### Performance Optimizations
* **Zero-Copy Transfers**: Employs the `sendfile()` system call to stream data directly from the kernel's page cache to the network socket, bypassing expensive user-space memory copies.
* **Connection Pooling**: Uses a pre-allocated static pool of 10,000 connections per worker to eliminate the overhead of frequent memory allocations during high traffic.
* **Socket Tuning**: Leverages `TCP_CORK` to coalesce headers and file data into fewer network packets and `TCP_NODELAY` to reduce latency for small writes.

### Request Lifecycle (State Machine)
The server uses a non-blocking state machine to manage the lifecycle of each request without stalling worker processes:
1. **Read**: Buffers incoming HTTP request data until headers (`\r\n\r\n`) are complete.
2. **Parse**: Decodes URLs and validates file paths using `realpath` to prevent directory traversal attacks.
3. **Send Header**: Transmits HTTP response headers with appropriate MIME types.
4. **Send File**: Streams the requested content via `sendfile()`.
5. **Reset**: Handles `Keep-Alive` logic to reset the connection state for subsequent requests.



### Reliability & Security
* **Path Validation**: Strictly validates requested files against the `www_root_path` to ensure they reside within the intended directory.
* **Automatic Timeouts**: Monitors an active connection list to automatically close idle connections that exceed the 5-second timeout.
* **Signal Handling**: Ignores `SIGPIPE` to prevent process crashes if a client abruptly disconnects during a transfer.

---

## Running it yourself

### Prerequisites
* **Operating System**: Linux (Required for `epoll` and `sendfile`).
* **Compiler**: GCC or Clang.
* **Directory Structure**: A `www` directory must exist in the same folder as the executable to serve files.

### Compilation
Compile the server using `gcc` with optimization flags:
```bash
gcc -O3 server.c -o server
