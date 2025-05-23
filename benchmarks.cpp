#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cassert>
#include <unistd.h>
#include <fcntl.h>
#include <linux/errqueue.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <mqueue.h>
#include <errno.h>
#include <cstdint>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>

// Aeron-related headers
#include <Aeron.h>
#include <util/CommandOptionParser.h>
#include <concurrent/NoOpIdleStrategy.h>
#include <FragmentAssembler.h>

// Common definitions for all tests
#define SOCKET_PATH "/tmp/bench.sock"
#define ZC_SOCKET_PATH "/tmp/benchzc.sock"
#define FIFO_PATH "/tmp/bench.fifo"
#define QUEUE_NAME "/bench_queue"
#define SHM_NAME "/my_shared_mem"
#define BUFFER_SIZE (2 * 1024)
#define NUM_ITERATIONS 1000
#define SHM_KEY 9876
#define SEM_KEY 9877

// Define on linux these are defined in <linux/splice.h>
#ifndef SPLICE_F_MOVE
#define SPLICE_F_MOVE     (0x01)  // Move pages instead of copying
#endif
#ifndef SPLICE_F_NONBLOCK
#define SPLICE_F_NONBLOCK (0x02)  // Non-blocking operation
#endif
#ifndef SPLICE_F_MORE
#define SPLICE_F_MORE     (0x04)  // More data will be coming
#endif
#ifndef SPLICE_F_GIFT
#define SPLICE_F_GIFT     (0x08)  // Pages passed in are a gift
#endif

// Define constants if they are not in the system headers
#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif

#ifndef SO_EE_ORIGIN_ZEROCOPY
#define SO_EE_ORIGIN_ZEROCOPY 5
#endif

#ifndef TCP_ZEROCOPY_RECEIVE
#define TCP_ZEROCOPY_RECEIVE 35
#endif

using namespace aeron::util;
using namespace aeron;

// Sys V shared memory segment
struct sysv_shared_data {
  int ready_flag;
  int done_flag;
  int iteration;
  char buffer[BUFFER_SIZE];
};

// Shared structures
struct shared_data {
    size_t size;
    char buffer[BUFFER_SIZE];
};

// Aeron-related
static const std::chrono::duration<long, std::milli> IDLE_SLEEP_MS(1);
static const std::chrono::duration<long, std::micro> IDLE_SLEEP_MU(1);
static const int FRAGMENTS_LIMIT = 100;
std::atomic<bool> running(true);

void sigIntHandler(int) {
  std::cerr << "Setting running to false\n";
  running = false;
}

void sigUsr1Handler(int) {
  // This signal is used to indicate that the source is finished
  printf("Received SIGUSR1 - source is finished\n");
  // Wait a bit for any more messages
  // std::this_thread::sleep_for(std::chrono::seconds(1));
  std::this_thread::sleep_for(std::chrono::microseconds(5));
  running = false;
}

namespace aeron { namespace defaults {

  const static std::string DEFAULT_CHANNEL = "aeron:udp?endpoint=localhost:20121";
  const static std::int32_t DEFAULT_STREAM_ID = 1001;
  const static long long DEFAULT_NUMBER_OF_MESSAGES = NUM_ITERATIONS;
  const static int DEFAULT_LINGER_TIMEOUT_MS = 0;

  struct Settings {
    std::string dirPrefix;
    std::string channel = aeron::defaults::DEFAULT_CHANNEL;
    std::int32_t streamId = aeron::defaults::DEFAULT_STREAM_ID;
    std::int64_t numberOfMessages = aeron::defaults::DEFAULT_NUMBER_OF_MESSAGES;
    int lingerTimeoutMs = aeron::defaults::DEFAULT_LINGER_TIMEOUT_MS;
  };

}}

template<typename T>
class SleepingIdleTStrategy
{
public:
  explicit SleepingIdleTStrategy(const std::chrono::duration<long, T> duration) :
    m_duration(duration)
  {}

  inline void idle(int workCount) {
    if (0 == workCount) {
      std::this_thread::sleep_for(m_duration);
    }
  }

  inline void reset()
  {}

  inline void idle()
  {
    std::this_thread::sleep_for(m_duration);
  }

private:
  std::chrono::duration<long, T> m_duration;
};

fragment_handler_t accumulateBytes(std::size_t& bytes_read, std::atomic<int>& messages_received) {
  return [&bytes_read, &messages_received](const AtomicBuffer &buffer, util::index_t offset, util::index_t length, const Header &header){
    std::size_t length_t = static_cast<std::size_t>(length);
    bytes_read += length;
    /*
      messages_received++;
      std::cerr << "bytes_read: " << bytes_read 
      << " messages_received: " << messages_received
      << std::endl;
    */
  };
}

void print_bytes(char *buffer, ssize_t sz) {
  printf("First 10 bytes: ");
  for (int i=0; i<10; ++i) {
    printf("0x%02x ", (unsigned char)buffer[i]);
  }
  printf("\n");
  printf("Last 10 bytes: ");
  for (int i=10; i>0; --i) {
    printf("0x%02x ", (unsigned char)buffer[sz-i]);
  }
  printf("\n");
}

void print_maps(const char* who) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", getpid());
    printf("\n=== Memory maps for %s (PID: %d) ===\n", who, getpid());
    FILE* f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            printf("%s", line);
        }
        fclose(f);
    }
    printf("===================================\n\n");
}

// Utility functions
void random_bits(char* buffer, size_t num_bytes) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        perror("open urandom");
        exit(1);
    }
    ssize_t bytes = read(fd, buffer, num_bytes);
    if (bytes != num_bytes) {
        perror("read urandom");
        exit(1);
    }
    close(fd);
}

void random_bits_unsigned(unsigned char* buffer, size_t num_bytes) {
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    perror("open urandom");
    exit(1);
  }
  ssize_t bytes = read(fd, buffer, num_bytes);
  if (bytes != num_bytes) {
    perror("read urandom");
    exit(1);
  }
}

long long get_usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}


void drain_message_queue(const char* queue_name) {

    mqd_t mq = mq_open(queue_name, O_RDONLY | O_NONBLOCK);
    if (mq == (mqd_t)-1) {
        // If we can't open it, it might not exist, which is fine
        return;
    }

    struct mq_attr attr;
    if (mq_getattr(mq, &attr) == -1) {
        mq_close(mq);
        return;
    }

    // Allocate a buffer for reading messages
    char* buffer = new char[attr.mq_msgsize];
    unsigned int prio;

    // Read messages until queue is empty
    while (true) {
        ssize_t bytes = mq_receive(mq, buffer, attr.mq_msgsize, &prio);
        if (bytes == -1) {
            if (errno == EAGAIN) {
                break;
            }
            break;
        }
    }

    // Clean up
    delete[] buffer;
    mq_close(mq);
    mq_unlink(queue_name);
}


//				//
// Start: FIFO Implementation	//
//				//
void fifo_source(void) {
    char buffer[BUFFER_SIZE];
    random_bits(buffer, BUFFER_SIZE);
    
    int fd = open(FIFO_PATH, O_WRONLY);
    if (fd < 0) {
        perror("FIFO open failed");
        exit(1);
    }
    
    long long start_time = get_usec();
    
    for (int i=0; i<NUM_ITERATIONS; ++i) {
      size_t bytes_sent = 0;
      while (bytes_sent < BUFFER_SIZE) {
	ssize_t result = write(fd, buffer + bytes_sent,
			       BUFFER_SIZE - bytes_sent);

	if (result < 0) {
	  perror("FIFO write failed");
	  exit(EXIT_FAILURE);
	} else if (result == 0) {
	  fprintf(stderr, "FIFO write returned 0\n");
	  exit(EXIT_FAILURE);
	}
	
	bytes_sent += result;
      }
    }

    long long end_time = get_usec();

    // printf("FIFO SOURCE:\n============\n");
    printf("FIFO source finished: [bytes sent: %d] %lld usec (%.2f MB/s)\n", 
	   BUFFER_SIZE,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    // print_bytes(buffer, BUFFER_SIZE);

    close(fd);
}

void fifo_target(void) {
    char buffer[BUFFER_SIZE];

    int fd = open(FIFO_PATH, O_RDONLY);
    if (fd < 0) {
        perror("FIFO open failed");
        exit(1);
    }

    long long start_time = get_usec();

    for (int i=0; i<NUM_ITERATIONS; ++i) {
      size_t bytes_received = 0;
      while (bytes_received < BUFFER_SIZE) {
	ssize_t result = read(fd, buffer + bytes_received,
			      BUFFER_SIZE - bytes_received);

	if (result < 0) {
	  perror("FIFO read failed");
	  exit(EXIT_FAILURE);
	} else if (result == 0) {
	  fprintf(stderr, "FIFO writer closed connection\n");
	  exit(EXIT_FAILURE);
	}

	bytes_received += result;
      }
    }

    
    long long end_time = get_usec();

    // printf("FIFO TARGET:\n============\n");
    printf("FIFO target finished [bytes rec: %d]: %lld usec (%.2f MB/s)\n", 
	   BUFFER_SIZE,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    // print_bytes(buffer, BUFFER_SIZE);

    close(fd);
}

//				//
// End: FIFO Implementation	//
//				//

//							  //
// Start: TCP/IP domain Socket Implementation (zero-copy) //
//							  //
// Modification to the TCP socket implementation to handle small messages correctly
// Simplified Zero-Copy TCP Socket Implementation
void tcp_zc_socket_source(int port) {
    char buffer[BUFFER_SIZE];
    random_bits(buffer, BUFFER_SIZE);
    
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("TCP ZC Socket creation failed");
        exit(1);
    }

    // Enable address reuse
    int optval = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("TCP ZC Socket reuse failed");
        exit(1);
    }

    // Only use zero-copy for large messages ( >= 20Mb)
    const int MIN_ZC_SIZE = 20 * 1024;
    int zerocopy_enabled = 0;
    if (BUFFER_SIZE >= MIN_ZC_SIZE) {
        // Try to enable zero-copy
        int val = 1;
        if (setsockopt(sock_fd, SOL_SOCKET, SO_ZEROCOPY, &val, sizeof(val)) == 0) {
            zerocopy_enabled = 1;
            // printf("Using zero-copy for message size %d bytes\n", BUFFER_SIZE);
        } else {
	  ;
	  // printf("Zero-copy not supported (errno=%d: %s)\n", errno, strerror(errno));
        }
    } else {
      ;
      //printf("Message size too small for zero-copy: %d bytes\n", BUFFER_SIZE);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    // Wait for server to be ready
    sleep(1);
    
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("TCP ZC Socket connect failed");
        exit(1);
    }

    long long start_time = get_usec();
    
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        size_t bytes_sent = 0;
        while (bytes_sent < BUFFER_SIZE) {
            ssize_t result;
            
            if (zerocopy_enabled) {
                result = send(sock_fd, buffer + bytes_sent, 
                             BUFFER_SIZE - bytes_sent, MSG_ZEROCOPY);
                
                if (result < 0 && (errno == EINVAL || errno == ENOSYS)) {
                    // Fall back if zero-copy fails
                    zerocopy_enabled = 0;
                    printf("MSG_ZEROCOPY failed, falling back to regular send\n");
                    result = send(sock_fd, buffer + bytes_sent,
                                BUFFER_SIZE - bytes_sent, 0);
                }
            } else {
                result = send(sock_fd, buffer + bytes_sent,
                            BUFFER_SIZE - bytes_sent, 0);
            }
            
            if (result < 0) {
                if (errno == EINTR) continue;  // Interrupted, retry
                perror("TCP ZC Socket send failed");
                exit(EXIT_FAILURE);
            } else if (result == 0) {
                fprintf(stderr, "TCP ZC Connection closed by peer\n");
                exit(EXIT_FAILURE);
            }
            bytes_sent += result;
        }
    }

    long long end_time = get_usec();
    printf("TCP ZC Socket source finished: [bytes sent: %d] %lld usec (%.2f MB/s)\n", 
           BUFFER_SIZE,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    
    close(sock_fd);
}

void tcp_zc_socket_target(int port) {
    // Zero-copy is only relevant on the sender side
  // This should be identical to vanilla tcp_socket_target
    char buffer[BUFFER_SIZE];
    
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("TCP ZC Socket creation failed");
        exit(1);
    }
    
    // Enable address reuse
    int optval = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("TCP ZC Socket reuse failed");
        exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("TCP ZC Socket bind failed");
        exit(1);
    }

    if (listen(sock_fd, 1) < 0) {
        perror("TCP ZC Socket listen failed");
        exit(1);
    }
    
    int client_fd = accept(sock_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("TCP ZC Socket accept failed");
        exit(1);
    }

    long long start_time = get_usec();
 
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        size_t bytes_received = 0;
        while (bytes_received < BUFFER_SIZE) {
            ssize_t result = recv(client_fd, buffer + bytes_received,
                                  BUFFER_SIZE - bytes_received, 0);

            if (result < 0) {
                if (errno == EINTR) continue;  // Interrupted, retry
                perror("TCP ZC Socket recv failed");
                exit(EXIT_FAILURE);
            } else if (result == 0) {
                fprintf(stderr, "TCP ZC Connection closed by peer\n");
                exit(EXIT_FAILURE);
            }
            
            bytes_received += result;
        }
    }
    
    long long end_time = get_usec();
    printf("TCP ZC Socket target finished [bytes rec: %d]: %lld usec (%.2f MB/s)\n", 
           BUFFER_SIZE,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    
    close(client_fd);
    close(sock_fd);
    
    // Cleanup
    char socket_path[256];
    snprintf(socket_path, sizeof(socket_path), "/tmp/tcp_zc_bench_%d.sock", port);
    unlink(socket_path);
}
//							//
// End: TCP/IP domain Socket Implementation (zero-copy) //
//							//


//							//
// Start: SPLICE/VMSPLICE Zero-Copy Implementation	//
//							//

void splice_source(bool aligned) {

  char *buffer = NULL;
  char desc[50];
  if (aligned) {
    strcpy(desc, "aligned");
  } else {
    strcpy(desc, "unaligned");
  }

  if (aligned) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (posix_memalign((void **)&buffer, page_size, BUFFER_SIZE) != 0) {
      perror("posix_memalign failed");
      exit(EXIT_FAILURE);
    }
  } else {
    buffer = (char*)malloc(BUFFER_SIZE);
    if (!buffer) {
      perror("malloc failed");
      exit(EXIT_FAILURE);
    }
  }
  
    random_bits(buffer, BUFFER_SIZE);

    
    // Socket used for synchronization only - probably a more
    // efficient way to do this; we use a UNIX domain socket
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    // Give target time to start
    sleep(1);
    
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Socket connect failed");
        exit(EXIT_FAILURE);
    }
    
    // Create a pipe for vmsplice/splice operations
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("Pipe creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Try to enable pipe buffer sizing for better performance
    // although in our case pipe_size should be 64Kb and 
    // resizing should not take place
    long pipe_size = fcntl(pipefd[1], F_GETPIPE_SZ);
    if (pipe_size > 0 && pipe_size < BUFFER_SIZE) {
        if (fcntl(pipefd[1], F_SETPIPE_SZ, BUFFER_SIZE) < 0) {
            fprintf(stderr, "Warning: Could not increase pipe size (errno=%d: %s)\n", 
                    errno, strerror(errno));
        }
    }
    
    // Non-blocking mode
    int flags = fcntl(pipefd[1], F_GETFL);
    if (flags >= 0) {
        fcntl(pipefd[1], F_SETFL, flags | O_NONBLOCK);
    }

    struct iovec iov;
    iov.iov_base = buffer;
    iov.iov_len = BUFFER_SIZE;
    
    long long start_time = get_usec();
    
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        
        // Step 1: Move data from user space to pipe (vmsplice)
        ssize_t bytes_spliced = vmsplice(pipefd[1], &iov, 1, SPLICE_F_GIFT);
        if (bytes_spliced < 0) {
            if (errno == EINVAL || errno == ENOSYS) {
                fprintf(stderr, "vmsplice not supported on this system (errno=%d: %s)\n", 
                        errno, strerror(errno));
                exit(EXIT_FAILURE);
            }
            perror("vmsplice failed");
            exit(EXIT_FAILURE);
        }
        
        // If we couldn't send everything at once, handle it
        size_t total_spliced = bytes_spliced;
        while (total_spliced < BUFFER_SIZE) {
            bytes_spliced = vmsplice(pipefd[1], &iov, 1, SPLICE_F_GIFT);
            if (bytes_spliced < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Pipe is full, need to move data out first
                    break;
                }
                perror("vmsplice continuation failed");
                exit(EXIT_FAILURE);
            }
            total_spliced += bytes_spliced;
        }
        
        // Step 2: Move data from pipe to socket (splice)
        size_t total_sent = 0;
        while (total_sent < BUFFER_SIZE) {
            ssize_t sent = splice(pipefd[0], NULL, sock_fd, NULL, 
                                  BUFFER_SIZE - total_sent, 
                                  SPLICE_F_MOVE | SPLICE_F_MORE);
            if (sent < 0) {
                if (errno == EINVAL || errno == ENOSYS) {
                    fprintf(stderr, "splice not supported on this system (errno=%d: %s)\n", 
                            errno, strerror(errno));
                    exit(EXIT_FAILURE);
                }
                perror("splice failed");
                exit(EXIT_FAILURE);
            }
            total_sent += sent;
        }
    }
    
    // Wait for acknowledgment before stopping
    char ack;
    if (recv(sock_fd, &ack, 1, 0) != 1) {
        perror("Failed to receive final acknowledgment");
    }
    
    long long end_time = get_usec();
    printf("SPLICE source finished: [bytes sent: %d] %lld usec (%.2f MB/s)\n", 
           BUFFER_SIZE,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    
    // Clean up
    close(pipefd[0]);
    close(pipefd[1]);
    close(sock_fd);
    free(buffer);
}

void splice_target(bool aligned) {

  char desc[50];
  if (aligned) {
    strcpy(desc, "aligned");
  } else {
    strcpy(desc, "unalignged");
  }

    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    unlink(SOCKET_PATH);
    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Socket bind failed");
        exit(EXIT_FAILURE);
    }
    
    if (listen(sock_fd, 1) < 0) {
        perror("Socket listen failed");
        exit(EXIT_FAILURE);
    }
    
    int client_fd = accept(sock_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("Socket accept failed");
        exit(EXIT_FAILURE);
    }
    
    // Create a pipe for receiving data
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("Pipe creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Try to optimize pipe size; again, should not be triggered
    long pipe_size = fcntl(pipefd[0], F_GETPIPE_SZ);
    if (pipe_size > 0 && pipe_size < BUFFER_SIZE) {
        if (fcntl(pipefd[0], F_SETPIPE_SZ, BUFFER_SIZE) < 0) {
            fprintf(stderr, "Warning: Could not increase pipe size (errno=%d: %s)\n", 
                    errno, strerror(errno));
        }
    }

    /*
    
    // Create a temporary file to store/process data
    char tempfile[] = "/tmp/splice_target_XXXXXX";
    int temp_fd = mkstemp(tempfile);
    if (temp_fd < 0) {
        perror("Failed to create temporary file");
        exit(EXIT_FAILURE);
    }
    unlink(tempfile);  // Delete on close
    
    */

    long page_size = sysconf(_SC_PAGESIZE);

    char *buffer = NULL;

    if (aligned) {
      if (posix_memalign((void **)&buffer, page_size, BUFFER_SIZE) != 0) {
	perror("posix_memalign failed");
	exit(EXIT_FAILURE);
      }
    } else {
      buffer = (char *)malloc(BUFFER_SIZE);
      if (!buffer) {
	perror("Malloc failed");
	exit(EXIT_FAILURE);
      }
    }
    
    struct iovec iov;
    iov.iov_base = buffer;

    long long start_time = get_usec();
    
    size_t total_received = 0;
    while (total_received < BUFFER_SIZE * NUM_ITERATIONS) {
        // Step 1: Use splice to move data from socket to pipe
        ssize_t bytes_received = splice(client_fd, NULL, pipefd[1], NULL, 
                                        BUFFER_SIZE, 
                                        SPLICE_F_MOVE | SPLICE_F_MORE);
        if (bytes_received < 0) {
            if (errno == EINVAL || errno == ENOSYS) {
                fprintf(stderr, "splice not supported on this system (errno=%d: %s)\n", 
                        errno, strerror(errno));
                exit(EXIT_FAILURE);
            }
            perror("splice from socket to pipe failed");
            exit(EXIT_FAILURE);
        } else if (bytes_received == 0) {
            fprintf(stderr, "Connection closed by peer\n");
            break;
        }
        

        // Step 2: Use splice to move data from pipe to file (if needed)
        // This simulates processing the data without copying to user space
	/*
        ssize_t bytes_written = splice(pipefd[0], NULL, temp_fd, NULL, 
                                      bytes_received, 
                                      SPLICE_F_MOVE);
	*/

	/*
	ssize_t bytes_written = read(pipefd[0], buffer, bytes_received);
        if (bytes_written < 0) {
            perror("read from pipe to file failed");
            exit(EXIT_FAILURE);
        }
	*/

	iov.iov_len = bytes_received;

	ssize_t bytes_written = vmsplice(pipefd[0], &iov, 1, 0);
	if (bytes_written < 0) {
	  perror("vmsplice from pipe to buffer failed");
	  exit(EXIT_FAILURE);
	}

        total_received += bytes_received;
    }
    
    // Send ack back over communication channel
    char ack = 'A';
    if (send(client_fd, &ack, 1, 0) != 1) {
        perror("Failed to send final acknowledgment");
    }
    
    long long end_time = get_usec();
    printf("SPLICE target finished [bytes rec: %zu]: %lld usec (%.2f MB/s)\n", 
           total_received / 1000,
           end_time - start_time,
           ((double)total_received) / 1000 / (end_time - start_time));
    
    // Clean up
    close(pipefd[0]);
    close(pipefd[1]);
    // close(temp_fd);
    close(client_fd);
    close(sock_fd);
    free(buffer);
    unlink(SOCKET_PATH);
}

//							//
// End: SPLICE/VMSPLICE Zero-Copy Implementation	//
//							//

//							//
// Start: UNIX Domain Socket Implementation (zero-copy) //
//							//
void socket_zc_source(void) {
  char buffer[BUFFER_SIZE];
  random_bits(buffer, BUFFER_SIZE);

  int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    perror("ZC Socket creation failed");
    exit(1);
  }

  // Set socket send buffer to handle data queuing
  int sndbuf = BUFFER_SIZE * 2; 
  if (setsockopt(sock_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
    perror("zc setsocketopt SO_SNDBUF failed");
    exit(1);
  }

  // Enable zero copy notifications on this socket
  int val = 1;
  if (setsockopt(sock_fd, SOL_SOCKET, SO_ZEROCOPY, &val, sizeof(val)) < 0) {
    fprintf(stderr, "Warning: SO_ZEROCOPY not supported, falling back to regular send\n");
  }


  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, ZC_SOCKET_PATH, sizeof(addr.sun_path) - 1);

  sleep(1);

  if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("ZC Socket connect failed");
    exit(1);
  }

  long long start_time = get_usec();

  for (int i=0; i< NUM_ITERATIONS; ++i) {
    size_t bytes_sent = 0;
    while (bytes_sent < BUFFER_SIZE) {
      ssize_t result = send(sock_fd, buffer + bytes_sent,
			    BUFFER_SIZE - bytes_sent,
			    MSG_ZEROCOPY);

      if (result < 0 && errno == EINVAL) {
	result = send(sock_fd, buffer + bytes_sent,
		      BUFFER_SIZE - bytes_sent,
		      0);
      }
      if (result < 0) {
	perror("ZC Socket send failed");
	exit(EXIT_FAILURE);
      }  else if (result == 0) {
	fprintf(stderr, "Connection closed by peer\n");
	exit(EXIT_FAILURE);
      }

      bytes_sent += result;
    }

          struct msghdr msg = {0};
      struct sockaddr_in addr;
      char control[100];
      struct iovec iov[1];
      
      msg.msg_name = &addr;
      msg.msg_namelen = sizeof(addr);
      msg.msg_control = control;
      msg.msg_controllen = sizeof(control);
      msg.msg_iov = iov;
      msg.msg_iovlen = 1;
      
      int flags = MSG_ERRQUEUE | MSG_DONTWAIT;
      if (recvmsg(sock_fd, &msg, flags) < 0) {
          if (errno != EAGAIN && errno != EWOULDBLOCK) {
              perror("Failed to receive notification");
          }
      }
  }

  long long end_time = get_usec();
    printf("ZC UNIX Domain Socket source finished: [bytes sent: %d] %lld usec (%.2f MB/s)\n", 
           BUFFER_SIZE,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    // print_bytes(buffer, BUFFER_SIZE);
    close(sock_fd);  
}

void socket_zc_target(void) {
  char buffer[BUFFER_SIZE];

  int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    perror("Socket creation failed");
    exit(1);
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, ZC_SOCKET_PATH, sizeof(addr.sun_path) - 1);

  unlink(ZC_SOCKET_PATH);
  if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("ZC Socket bind failed");
    exit(1);
  }

  if (listen(sock_fd, 1) < 0) {
    perror("Socket listen failed");
    exit(1);
  }

  int client_fd = accept(sock_fd, NULL, NULL);
  if (client_fd < 0) {
    perror("ZC Socket accept failed");
    exit(1);
  }

  // Set socket receive buffer for the client connection
  int rcvbuf = BUFFER_SIZE * 2;
  if (setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
    perror("setsockopt SO_RCVBUF failed");
    exit(1);
  }

  // Try to enable zero copy if supported
  int val = 1;
  if (setsockopt(client_fd, SOL_SOCKET, SO_ZEROCOPY, &val, sizeof(val)) < 0) {
    fprintf(stderr, "Zero copy receiving not supported, using standard recv\n");
  }

  // Verify the buffer we set (it might be rounded up by the system)
  int actual_rcvbuf;
  socklen_t len = sizeof(actual_rcvbuf);
  if (getsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &actual_rcvbuf, &len) < 0) {
    perror("getsockopt SO_FCVBUF failed");
    exit(1);
  }
  assert(actual_rcvbuf >= rcvbuf && "Receive buffer not set up correctly");

  long long start_time = get_usec();

  for (int i=0; i < NUM_ITERATIONS; ++i) {

    size_t bytes_received = 0;
    while (bytes_received < BUFFER_SIZE) {

      size_t result = recv(client_fd, buffer + bytes_received,
			   BUFFER_SIZE - bytes_received,
			   0);
	if (result < 0) {
	  perror("Socket recv failed");
	  exit(EXIT_FAILURE);
	} else if (result == 0) {
	  fprintf(stderr, "Connection closed by peer\n");
	  exit(EXIT_FAILURE);
	}
	
	bytes_received += result;      
    }
  }

    long long end_time = get_usec();
    // printf("UNIX Domain Socket TARGET:\n============\n");
    printf("ZC UNIX Domain Socket target finished [bytes rec: %d]: %lld usec (%.2f MB/s)\n", 
           BUFFER_SIZE,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    // print_bytes(buffer, BUFFER_SIZE);
    close(client_fd);
    close(sock_fd);
    unlink(ZC_SOCKET_PATH);
 
}
//							//
// End: UNIX Domain Socket Implementation (zero-copy) //
//							//

//						//
// Start: UNIX Domain Socket Implementation	//
//						//
void socket_source(void) {
    char buffer[BUFFER_SIZE];
    random_bits(buffer, BUFFER_SIZE);
    
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    // Set socket send buffer to handle data queuing
    int sndbuf = BUFFER_SIZE * 2;  // Double the size for queuing
    if (setsockopt(sock_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
        perror("setsockopt SO_SNDBUF failed");
        exit(1);
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    sleep(1);  // Give receiver time to start
    
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Socket connect failed");
        exit(1);
    }

    long long start_time = get_usec();
    
    for (int i=0; i<NUM_ITERATIONS; ++i) {
      size_t bytes_sent = 0;
      while (bytes_sent < BUFFER_SIZE) {
	ssize_t result = send(sock_fd, buffer + bytes_sent,
			      BUFFER_SIZE - bytes_sent,
			      0);
	if (result < 0) {
	  perror("Socket send failed");
	  exit(EXIT_FAILURE);
	} else if (result == 0) {
	  fprintf(stderr, "Connection closed by peer\n");
	  exit(EXIT_FAILURE);
	}
	
	bytes_sent += result;
      }
    }

    long long end_time = get_usec();
    // printf("UNIX Domain Socket SOURCE:\n============\n");
    printf("UNIX Domain Socket source finished: [bytes sent: %d] %lld usec (%.2f MB/s)\n", 
           BUFFER_SIZE,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    // print_bytes(buffer, BUFFER_SIZE);
    close(sock_fd);
}

void socket_target(void) {
    char buffer[BUFFER_SIZE];  // Use regular BUFFER_SIZE for application buffer
    
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Socket creation failed");
        exit(1);
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    unlink(SOCKET_PATH);
    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Socket bind failed");
        exit(1);
    }
    
    if (listen(sock_fd, 1) < 0) {
        perror("Socket listen failed");
        exit(1);
    }
    
    int client_fd = accept(sock_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("Socket accept failed");
        exit(1);
    }

    // Set socket receive buffer for the client connection
    int rcvbuf = BUFFER_SIZE * 2;  // Double the size for queuing
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        perror("setsockopt SO_RCVBUF failed");
        exit(1);
    }

    // Verify the buffer was set (it might be rounded up by the system)
    int actual_rcvbuf;
    socklen_t len = sizeof(actual_rcvbuf);
    if (getsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &actual_rcvbuf, &len) < 0) {
        perror("getsockopt SO_RCVBUF failed");
        exit(1);
    }
    assert(actual_rcvbuf >= rcvbuf && "Receive buffer not set up correctly");

    long long start_time = get_usec();
 
    for (int i=0; i<NUM_ITERATIONS; ++i) {

      size_t bytes_received = 0;
      while (bytes_received < BUFFER_SIZE) {
	ssize_t result = recv(client_fd, buffer + bytes_received,
			      BUFFER_SIZE - bytes_received,
			      0);

	if (result < 0) {
	  perror("Socket recv failed");
	  exit(EXIT_FAILURE);
	} else if (result == 0) {
	  fprintf(stderr, "Connection closed by peer\n");
	  exit(EXIT_FAILURE);
	}
	
	bytes_received += result;
      }
    }
    
    
    long long end_time = get_usec();
    // printf("UNIX Domain Socket TARGET:\n============\n");
    printf("UNIX Domain Socket target finished [bytes rec: %d]: %lld usec (%.2f MB/s)\n", 
           BUFFER_SIZE,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    // print_bytes(buffer, BUFFER_SIZE);
    close(client_fd);
    close(sock_fd);
    unlink(SOCKET_PATH);
}
//						//
// End: UNIX Domain Socket Implementation	//
//						//

//					//
// Start: TCP/IP Socket Implementation	//
//					//
void tcp_source(int port) {
    char buffer[BUFFER_SIZE];
    random_bits(buffer, BUFFER_SIZE);
    
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("TCP Socket creation failed");
        exit(1);
    }

    // Enable address reuse
    int optval = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("TCP Socket reuse failed");
        exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    // Wait for server to be ready
    sleep(1);
    
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("TCP Socket connect failed");
        exit(1);
    }

    long long start_time = get_usec();
    
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        size_t bytes_sent = 0;
        while (bytes_sent < BUFFER_SIZE) {
            ssize_t result = send(sock_fd, buffer + bytes_sent,
                                  BUFFER_SIZE - bytes_sent, 0);
            if (result < 0) {
                if (errno == EINTR) continue;  // Interrupted, retry
                perror("TCP Socket send failed");
                exit(EXIT_FAILURE);
            } else if (result == 0) {
                fprintf(stderr, "TCP Connection closed by peer\n");
                exit(EXIT_FAILURE);
            }
            bytes_sent += result;
        }
    }

    long long end_time = get_usec();
    printf("TCP Socket source finished: [bytes sent: %d] %lld usec (%.2f MB/s)\n", 
           BUFFER_SIZE,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    
    close(sock_fd);
}

void tcp_target(int port) {
    char buffer[BUFFER_SIZE];
    
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("TCP Socket creation failed");
        exit(1);
    }
    
    // Enable address reuse
    int optval = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("TCP Socket reuse failed");
        exit(1);
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    // Unlink any existing binding
    unlink(SOCKET_PATH);  // If you're using a socket path
    
    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("TCP Socket bind failed");
        exit(1);
    }
    
    if (listen(sock_fd, 1) < 0) {
        perror("TCP Socket listen failed");
        exit(1);
    }
    
    int client_fd = accept(sock_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("TCP Socket accept failed");
        exit(1);
    }

    long long start_time = get_usec();
 
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        size_t bytes_received = 0;
        while (bytes_received < BUFFER_SIZE) {
            ssize_t result = recv(client_fd, buffer + bytes_received,
                                  BUFFER_SIZE - bytes_received, 0);

            if (result < 0) {
                if (errno == EINTR) continue;  // Interrupted, retry
                perror("TCP Socket recv failed");
                exit(EXIT_FAILURE);
            } else if (result == 0) {
                fprintf(stderr, "TCP Connection closed by peer\n");
                exit(EXIT_FAILURE);
            }
            
            bytes_received += result;
        }
    }
    
    long long end_time = get_usec();
    printf("TCP Socket target finished [bytes rec: %d]: %lld usec (%.2f MB/s)\n", 
           BUFFER_SIZE,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    
    close(client_fd);
    close(sock_fd);
    
    // Cleanup
    char socket_path[256];
    snprintf(socket_path, sizeof(socket_path), "/tmp/tcp_bench_%d.sock", port);
    unlink(socket_path);
}
//					//
// End: TCP/IP Socket Implementation	//
//					//

//					//
// Start: UDP Implementation		//
//					//
void udp_target(int port) {
    char buffer[BUFFER_SIZE];
    
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("UDP Socket creation failed");
        exit(1);
    }
    
    // Enable address reuse
    int optval = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("UDP Socket reuse failed");
        exit(1);
    }
    
    // Increase receive buffer size
    int rcvbuf = BUFFER_SIZE * 2;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        perror("UDP Socket receive buffer size failed");
        exit(1);
    }
    
    // Set a very long timeout
    struct timeval tv;
    tv.tv_sec = 5;  // 5 seconds timeout
    tv.tv_usec = 0;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("Error setting socket timeout");
        exit(1);
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("UDP Socket bind failed");
        exit(1);
    }

    long long start_time = get_usec();
    int iterations_completed = 0;
    int termination_received = 0;
 
    while (!termination_received) {
        struct sockaddr_in sender_addr;
        socklen_t sender_len = sizeof(sender_addr);
        
        ssize_t result = recvfrom(sock_fd, buffer, BUFFER_SIZE, 0, 
                                  (struct sockaddr*)&sender_addr, &sender_len);

        if (result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout occurred
                printf("Timeout after %d iterations\n", iterations_completed);
                break;
            }
            perror("UDP Socket recvfrom failed");
            exit(EXIT_FAILURE);
        }

        // Check for termination message
        if (result == sizeof(int) && *(int*)buffer == -1) {
            termination_received = 1;
            break;
        }


        if (result != BUFFER_SIZE) {
            fprintf(stderr, "Incomplete UDP message: expected %d, got %zd\n", 
                    BUFFER_SIZE, result);
            exit(EXIT_FAILURE);
        }
    }
    
    long long end_time = get_usec();
    printf("UDP Socket target finished [bytes rec: %d, iterations: %d]: %lld usec (%.2f MB/s)\n", 
           BUFFER_SIZE,
           iterations_completed,
           end_time - start_time - 200000,
           ((double)BUFFER_SIZE * iterations_completed) / (end_time - start_time - 200000));
    
    close(sock_fd);
}

void udp_source(int port) {
    char buffer[BUFFER_SIZE];
    random_bits(buffer, BUFFER_SIZE);
    
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("UDP Socket creation failed");
        exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    // Small delay to ensure receiver is ready
    usleep(200000);  // 200ms delay
    
    long long start_time = get_usec();
    
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        ssize_t result = sendto(sock_fd, buffer, BUFFER_SIZE, 0,
                                (struct sockaddr*)&addr, sizeof(addr));
        
        if (result < 0) {
            perror("UDP Socket sendto failed");
            exit(EXIT_FAILURE);
        }

        if (result != BUFFER_SIZE) {
            fprintf(stderr, "Incomplete send: expected %d, sent %zd\n", 
                    BUFFER_SIZE, result);
            exit(EXIT_FAILURE);
        }
    }

    // Send termination message
    int termination_msg = -1;
    ssize_t result = sendto(sock_fd, &termination_msg, sizeof(termination_msg), 0,
                            (struct sockaddr*)&addr, sizeof(addr));
    if (result < 0) {
        perror("UDP Socket termination sendto failed");
        exit(EXIT_FAILURE);
    }

    long long end_time = get_usec();
    printf("UDP Socket source finished: [bytes sent: %d] %lld usec (%.2f MB/s)\n", 
           BUFFER_SIZE,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    
    // Ensure all messages are sent
    usleep(500000);  // 500ms delay
    
    close(sock_fd);
}
//					//
// End: UDP Implementation		//
//					//

//					//
// Start: POSIX MQ Implementation	//
//					//
void mq_source(void) {
    char buffer[BUFFER_SIZE];
    random_bits(buffer, BUFFER_SIZE);
    
    struct mq_attr attr = {
        .mq_flags = 0,
        .mq_maxmsg = 10,
        .mq_msgsize = BUFFER_SIZE,
        .mq_curmsgs = 0
    };
    
    mqd_t mq = mq_open(QUEUE_NAME, O_WRONLY | O_CREAT, 0644, &attr);
    if (mq == (mqd_t)-1) {
      // Try again?
      usleep(100);
      mq = mq_open(QUEUE_NAME, O_WRONLY | O_CREAT, 0644, &attr);
      if (mq == (mqd_t)-1) {	
	perror("Message queue open failed");
	exit(1);
      }
    }
    
    int num_iterations = NUM_ITERATIONS;

    long long start_time = get_usec();
    
    while (num_iterations) {
      mq_send(mq, buffer, BUFFER_SIZE, 0);
      num_iterations--;
    }
    
    long long end_time = get_usec();

    // printf("POSIX MQ SOURCE:\n============\n");
    printf("POSIX MQ source finished: [bytes sent: %d] %lld usec (%.2f MB/s)\n", 
	   BUFFER_SIZE,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    // print_bytes(buffer, BUFFER_SIZE);

    mq_close(mq);
}

void mq_target(void) {
    char buffer[BUFFER_SIZE];
    
    mqd_t mq = mq_open(QUEUE_NAME, O_RDONLY);
    if (mq == (mqd_t)-1) {
      // Try again?
      usleep(100);
      mq = mq_open(QUEUE_NAME, O_RDONLY);
      if (mq == (mqd_t)-1) {	
	perror("Message queue open failed");
	exit(1);
      }
    }


    int num_iterations = NUM_ITERATIONS;
    ssize_t bytes;
    
    long long start_time = get_usec();
    
    unsigned int prio;

    while (num_iterations) {
      bytes = mq_receive(mq, buffer, BUFFER_SIZE, &prio);
      num_iterations--;
    }

    long long end_time = get_usec();

    // printf("POSIX MQ TARGET:\n============\n");
    printf("POSIX MQ target finished [bytes rec: %zd]: %lld usec (%.2f MB/s)\n", 
	   bytes,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    // print_bytes(buffer, BUFFER_SIZE);

    mq_close(mq);
    mq_unlink(QUEUE_NAME);
}
//					//
// End: POSIX MQ Implementation		//
//					//

//					//
// Start: Shmem+Eventfd Implementation	//
//					//
void eventfd_sender(int efd, struct shared_data* shm) {
  // Initialize test data
  char test_data[BUFFER_SIZE];
  random_bits(test_data, BUFFER_SIZE);

  int num_iterations = NUM_ITERATIONS;
  
  long long start_time = get_usec();

  while (num_iterations) {
    memcpy(shm->buffer, test_data, BUFFER_SIZE);
    shm->size = BUFFER_SIZE;

    // Signal to receiver
    uint64_t u = 1;
    ssize_t result = write(efd, &u, sizeof(uint64_t));

    num_iterations--;
  }

  long long end_time = get_usec();

  // printf("Shmem+Eventfd SOURCE:\n============\n");
  printf("Shmem+Eventfd source finished: [bytes sent: %d] %lld usec (%.2f MB/s)\n", 
	 BUFFER_SIZE,
	 end_time - start_time,
	 ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
  // print_bytes(shm->buffer, BUFFER_SIZE);
  
}


void eventfd_receiver(int efd, struct shared_data* shm) {
  
  int num_iterations = NUM_ITERATIONS;

  long long start_time = get_usec();

  while (num_iterations) {
    uint64_t u;
    ssize_t result = read(efd, &u, sizeof(uint64_t));
    num_iterations--;
  }

  long long end_time = get_usec();

  // printf("Shmem+Eventfd TARGET:\n============\n");
  printf("Shmem+Eventfd target finished [bytes rec: %d]: %lld usec (%.2f MB/s)\n", 
	 BUFFER_SIZE,
	 end_time - start_time,
	 ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
  // print_bytes(shm->buffer, BUFFER_SIZE);

}
//					//
// End: Shmem+Eventfd Implementation	//
//					//

//					//
// Begin: Cross Memory Attach		//
//					//
// The address information structure to share memory location
typedef struct {
    void *addr;     // Memory address in the source process
    size_t size;    // Size of the memory region
    pid_t pid;      // PID of the source process
} addr_info_t;

// Target-side: Function to read memory from another process
ssize_t process_vm_readv_wrapper(pid_t pid, void *local_addr, void *remote_addr, size_t size) {
    struct iovec local[1];
    struct iovec remote[1];
    
    local[0].iov_base = local_addr;
    local[0].iov_len = size;
    remote[0].iov_base = remote_addr;
    remote[0].iov_len = size;
    
    return process_vm_readv(pid, local, 1, remote, 1, 0);
}

// Source-side: Function to write memory to another process
ssize_t process_vm_writev_wrapper(pid_t pid, void *local_addr, void *remote_addr, size_t size) {
    struct iovec local[1];
    struct iovec remote[1];
    
    local[0].iov_base = local_addr;
    local[0].iov_len = size;
    remote[0].iov_base = remote_addr;
    remote[0].iov_len = size;
    
    return process_vm_writev(pid, local, 1, remote, 1, 0);
}

void cma_source(void) {
  char *buffer = (char *)malloc(BUFFER_SIZE);
    if (!buffer) {
        perror("malloc failed");
        exit(EXIT_FAILURE);
    }
    
    random_bits(buffer, BUFFER_SIZE);

    // Socket creation used for synchronization
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
   
    // Give the target time to set up
    sleep(1);
    
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Socket connect failed");
        exit(EXIT_FAILURE);
    }
    
    addr_info_t info;
    info.addr = buffer;
    info.size = BUFFER_SIZE;
    info.pid = getpid();
    
    if (send(sock_fd, &info, sizeof(info), 0) != sizeof(info)) {
        perror("Failed to send address info");
        exit(EXIT_FAILURE);
    }
    
    // Synchronize with target before starting benchmark
    char sync_char;
    if (recv(sock_fd, &sync_char, 1, 0) != 1) {
        perror("Failed to receive synchronization");
        exit(EXIT_FAILURE);
    }
    
    long long start_time = get_usec();
    
    // For CMA, we don't actually send anything - the target reads directly from our memory
    // We just need to keep the buffer available and wait for notifications
    
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        // Wait for target to signal it's ready to read
        if (recv(sock_fd, &sync_char, 1, 0) != 1) {
            perror("Failed to receive ready signal");
            exit(EXIT_FAILURE);
        }
        
        // We could potentially update the buffer here if this were a real application
        
        sync_char = 'R';
        if (send(sock_fd, &sync_char, 1, 0) != 1) {
            perror("Failed to send ready signal");
            exit(EXIT_FAILURE);
        }
        
        if (recv(sock_fd, &sync_char, 1, 0) != 1) {
            perror("Failed to receive completion signal");
            exit(EXIT_FAILURE);
        }
    }
    
    long long end_time = get_usec();
    printf("CMA source finished: [bytes accessed: %d] %lld usec (%.2f MB/s)\n", 
           BUFFER_SIZE,
           end_time - start_time - 10000,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time - 10000));
    
    // Wait a bit before freeing the buffer to ensure target has completed
    sleep(1);
    
    // Clean up
    close(sock_fd);
    free(buffer);
}

void cma_target(void) {
  char *buffer = (char *)malloc(BUFFER_SIZE);
    if (!buffer) {
        perror("malloc failed");
        exit(EXIT_FAILURE);
    }
    
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    unlink(SOCKET_PATH);
    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Socket bind failed");
        exit(EXIT_FAILURE);
    }
    
    if (listen(sock_fd, 1) < 0) {
        perror("Socket listen failed");
        exit(EXIT_FAILURE);
    }
    
    int client_fd = accept(sock_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("Socket accept failed");
        exit(EXIT_FAILURE);
    }
    
    addr_info_t info;
    if (recv(client_fd, &info, sizeof(info), 0) != sizeof(info)) {
        perror("Failed to receive address info");
        exit(EXIT_FAILURE);
    }
    
    // Synchronize with source before starting benchmark
    char sync_char = 'S';
    if (send(client_fd, &sync_char, 1, 0) != 1) {
        perror("Failed to send synchronization");
        exit(EXIT_FAILURE);
    }
    
    long long start_time = get_usec();
    
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        sync_char = 'T';
        if (send(client_fd, &sync_char, 1, 0) != 1) {
            perror("Failed to send ready signal");
            exit(EXIT_FAILURE);
        }
        
        if (recv(client_fd, &sync_char, 1, 0) != 1) {
            perror("Failed to receive ready signal");
            exit(EXIT_FAILURE);
        }
        
        // Read directly from source process memory using CMA
        ssize_t bytes_read = process_vm_readv_wrapper(info.pid, buffer, info.addr, BUFFER_SIZE);
        if (bytes_read < 0) {
            perror("process_vm_readv failed");
            if (errno == EPERM) {
                fprintf(stderr, "Permission denied: Check process permissions or CAP_SYS_PTRACE capability\n");
            }
            exit(EXIT_FAILURE);
        } else if (bytes_read != BUFFER_SIZE) {
	  // fprintf(stderr, "Partial read: %zd/%d bytes\n", bytes_read, BUFFER_SIZE);
	  // Continue anyway
	  ;
        }
        
        sync_char = 'C';
        if (send(client_fd, &sync_char, 1, 0) != 1) {
            perror("Failed to send completion signal");
            exit(EXIT_FAILURE);
        }
    }
    
    long long end_time = get_usec();
    printf("CMA target finished [bytes read: %d]: %lld usec (%.2f MB/s)\n", 
           BUFFER_SIZE,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    
    // Clean up
    close(client_fd);
    close(sock_fd);
    unlink(SOCKET_PATH);
    free(buffer);
}
//					//
// End: Cross Memory Attach		//
//					//

//					//
// Begin: Aeron MQ			//
//					//

void aeron_target(void) {
  // std::cout << "Target: Starting Aeron target process" << std::endl;

    // Setup Aeron
    aeron::defaults::Settings settings;
    aeron::Context context;

    std::atomic<int> messages_received{0};

    context.newSubscriptionHandler([](const std::string &channel,
  				    std::int32_t streamId,
  				    std::int64_t correlationId) {
				     //       std::cout << "Target: New subscription on channel: " << channel
				     //                << " stream: " << streamId << std::endl;
				     ;
    });

    // std::cout << "Target: Connecting to Aeron..." << std::endl;
    std::shared_ptr<Aeron> aeron = Aeron::connect(context);
    signal(SIGINT, sigIntHandler);
    signal(SIGUSR1, sigUsr1Handler);

    // std::cout << "Target: Adding subscription on " << settings.channel
    //           << " stream " << settings.streamId << std::endl;
    std::int64_t id = aeron->addSubscription(settings.channel, settings.streamId);

    // Wait for subscription to be valid
    std::shared_ptr<Subscription> subscription = aeron->findSubscription(id);
    while (!subscription && running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      subscription = aeron->findSubscription(id);
    }

    if (!running) {
      std::cout << "Target: Aborted before subscription was valid" << std::endl;
      return;
    }

    // std::cout << "Target: Subscription ready with channel status: "
    //           << subscription->channelStatus() << std::endl;

    // Setup variables for receiving
    std::size_t bytes_received = 0;
    std::size_t total_bytes_received = 0;
    std::atomic<int> total_messages_received{0};
    int consecutiveEmptyPolls = 0;
    const int MAX_EMPTY_POLLS = 10000; // Early termination after enough empty polls

    // Create handler for accumulating bytes
    FragmentAssembler fragmentAssembler(
      [&bytes_received, &total_messages_received](const AtomicBuffer &buffer, 
                                              util::index_t offset, 
                                              util::index_t length, 
                                              const Header &header) {
        bytes_received += length;
        total_messages_received++;
      });

    fragment_handler_t handler = fragmentAssembler.handler();

    // Use a sleeping idle strategy to avoid high CPU usage
    // SleepingIdleStrategy idleStrategy(std::chrono::milliseconds(1);)
    SleepingIdleTStrategy<std::micro> idleStrategy(std::chrono::microseconds(10));
  

    // std::cout << "Target: Starting to receive messages..." << std::endl;

    int iterationsCompleted = 0;

    long long start_time = get_usec();

    while (iterationsCompleted < NUM_ITERATIONS && running) {
      // Poll for fragments
      int fragments_read = subscription->poll(handler, FRAGMENTS_LIMIT);

      // Handle empty polls
      if (fragments_read == 0) {
        consecutiveEmptyPolls++;

        // Print debug info periodically during empty polls
        if (consecutiveEmptyPolls % 1000 == 0) {
          // std::cout << "Target: " << consecutiveEmptyPolls << " empty polls, received "
	  //          << total_messages_received.load() << " of " << NUM_ITERATIONS
	  //          << " messages, bytes: " << bytes_received << std::endl;
        }

        // Early termination if we stop receiving messages but have received some
        if (consecutiveEmptyPolls > MAX_EMPTY_POLLS && total_messages_received.load() > 0) {
          // std::cout << "Target: No messages received for too long, assuming completion" << std::endl;
          break;
        }
      } else {
        consecutiveEmptyPolls = 0;

        // Update iterations based on messages received
        if (total_messages_received > iterationsCompleted) {
          iterationsCompleted = total_messages_received;

          // Progress feedback
          if (iterationsCompleted % 100 == 0) {
            // std::cout << "Target: Received " << iterationsCompleted << " messages" << std::endl;
	    ;
          }
        }
      }
      
      // Use idle strategy based on fragment count
      idleStrategy.idle(fragments_read);

      // Add to total bytes received
      total_bytes_received += bytes_received;
      bytes_received = 0; // Reset for next iteration
    }

    long long end_time = get_usec();
    double elapsed_seconds = (end_time - start_time) / 1000000.0;
    int final_messages_received = total_messages_received.load();
    double throughput = final_messages_received > 0 ?
                        ((double)BUFFER_SIZE * final_messages_received) / (end_time - start_time - 2000000) : 0;

    // std::cout << "Target: Completed receiving " << final_messages_received << "/"
    //           << NUM_ITERATIONS << " messages" << std::endl;

    printf("Aeron Queue target finished [bytes rec: %zu]: %lld usec (%.2f MB/s)\n",
           total_bytes_received / NUM_ITERATIONS,
           end_time - start_time - 2000000,
           throughput);
  }


void aeron_target_old(void) {

  aeron::defaults::Settings settings;
  aeron::Context context;

  std::atomic<int> messages_received{0};
  
  context.newSubscriptionHandler([](const std::string &channel, 
				    std::int32_t streamId, 
				    std::int64_t correlationId)
				 {
				   ;
				 });
				    
 
  std::shared_ptr<Aeron> aeron = Aeron::connect(context);
  signal(SIGINT, sigIntHandler);
  signal(SIGUSR1, sigUsr1Handler);
  std::int64_t id = aeron->addSubscription(settings.channel, settings.streamId);

  std::shared_ptr<Subscription> subscription = aeron->findSubscription(id);
  // Wait for the subscription to be valid
  while (!subscription && running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    subscription = aeron->findSubscription(id);
  }

  std::size_t bytes_received = 0;
  std::size_t total_bytes_received = 0;
  int consecutiveEmptyPolls = 0;
  std::int64_t channelStatus = subscription->channelStatus();

  FragmentAssembler fragmentAssembler(accumulateBytes(bytes_received, messages_received));
  fragment_handler_t handler = fragmentAssembler.handler();
  NoOpIdleStrategy idleStrategy;

  long long start_time = get_usec();
  
  for (int i=0; i<NUM_ITERATIONS && running; ++i) {
    bytes_received = 0;
    while (bytes_received < BUFFER_SIZE) {
      int fragments_read = subscription->poll(handler, FRAGMENTS_LIMIT);
      
      if (fragments_read == 0) {
	consecutiveEmptyPolls++;
	if (consecutiveEmptyPolls % 1000 == 0) {
	  break;
	}

      } else {
	consecutiveEmptyPolls = 0;
      }

      // std::cerr << "i: " << i << std::endl;
      /*
      std::cerr << "(BUFFER_SIZE, bytes_read): ("
		<< BUFFER_SIZE << ", "
		<< bytes_read << ", "
		<< ")"
		<< std::endl;
      */

      idleStrategy.idle(fragments_read);
      

      total_bytes_received += bytes_received;
    }
    
  }

  long long end_time = get_usec();
  
  // printf("Aeron QueueTARGET:\n============\n");
  printf("Aeron Queue target finished [bytes rec: %d]: %lld usec (%.2f MB/s)\n", 
	 total_bytes_received,
	 end_time - start_time - 1000000,
	 ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time - 1000000));
  // print_bytes(shm->buffer, BUFFER_SIZE);

  
}

void aeron_source(void) {
  // std::cout << "Source: Starting Aeron source process" << std::endl;

      // Create buffer for data
      unsigned char *buffer = (unsigned char*)malloc(BUFFER_SIZE);
      if (!buffer) {
        perror("Malloc failed");
        exit(EXIT_FAILURE);
      }

      // Fill buffer with random data
      random_bits_unsigned(buffer, BUFFER_SIZE);
      // std::cout << "Source: Filled buffer with random data" << std::endl;

      // Setup Aeron
      aeron::defaults::Settings settings;
      aeron::Context context;

      // std::cout << "Source: Connecting to Aeron..." << std::endl;
      std::shared_ptr<Aeron> aeron = Aeron::connect(context);
      signal(SIGINT, sigIntHandler);

      // std::cout << "Source: Adding publication on " << settings.channel
      //           << " stream " << settings.streamId << std::endl;
      std::int64_t id = aeron->addPublication(settings.channel, settings.streamId);

      // Wait for publication to be valid
      std::shared_ptr<Publication> publication = aeron->findPublication(id);
      while (!publication && running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        publication = aeron->findPublication(id);
      }

      if (!running) {
        // std::cout << "Source: Aborted before publication was valid" << std::endl;
        free(buffer);
        return;
      }

      // std::cout << "Source: Publication ready with channel status: "
      //           << publication->channelStatus() << std::endl;

      // Setup AtomicBuffer for the entire message at once
      concurrent::AtomicBuffer srcBuffer(buffer, BUFFER_SIZE);
      int messages_sent = 0;

      // Give target a chance to set up
      // std::cout << "Source: Waiting for target to initialize..." << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(2));

      long long start_time = get_usec();

      for (int i=0; i < NUM_ITERATIONS && running; ++i) {
        // Refresh buffer with new random data for each iteration
        srcBuffer.putBytes(0, buffer, BUFFER_SIZE);

        // Try to offer the buffer until successful or error
        std::int64_t result;
        bool sent = false;
        int retries = 0;

        while (!sent && running && retries < 1000) {
          result = publication->offer(srcBuffer);

          if (result > 0) {
            // Successfully sent
            messages_sent++;
            sent = true;

            // Progress feedback every 100 messages
            if (messages_sent % 100 == 0) {
              // std::cout << "Source: Sent " << messages_sent << " messages" << std::endl;
	      ;
            }

          } else if (result == -2) {
            // No subscribers, wait longer
            if (retries % 100 == 0) {
              // std::cout << "Source: Publication not connected. Waiting..." << std::endl;
	      ;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            retries++;

          } else if (result == -1) {
            // Back pressure, try again after a short delay
            // std::this_thread::sleep_for(std::chrono::microseconds(10));
            retries++;

          } else if (result == -3 || result == -4) {
            // std::cerr << "Source: Publication error: " << result << std::endl;
            running = false;
            break;
          }
        }

        if (retries >= 1000) {
          std::cerr << "Source: Failed to send message after 1000 retries" << std::endl;
        }

        if (!running) break;
      }

      long long end_time = get_usec();
      double elapsed_seconds = (end_time - start_time) / 1000000.0;
      double throughput = messages_sent > 0 ?
                         ((double)BUFFER_SIZE * messages_sent) / (end_time - start_time) : 0;

      // std::cout << "Source: Completed sending " << messages_sent << "/"
      //           << NUM_ITERATIONS << " messages" << std::endl;

      printf("Aeron Queue source finished: [bytes sent: %d x %d messages] %lld usec (%.2f MB/s)\n",
             BUFFER_SIZE, messages_sent,
             end_time - start_time,
             throughput);

      // Allow time for any in-flight messages to be processed
      std::this_thread::sleep_for(std::chrono::seconds(1));
      free(buffer);
  }


void aeron_source_old(void) {
  
    unsigned char *buffer = NULL;
    buffer = (unsigned char*)malloc(BUFFER_SIZE);
    if (!buffer) {
      perror("Malloc failed");
      exit(EXIT_FAILURE);
    }

    random_bits_unsigned(buffer, BUFFER_SIZE);
    
    aeron::defaults::Settings settings;
    aeron::Context context;
    
    std::shared_ptr<Aeron> aeron = Aeron::connect(context);
    signal(SIGINT, sigIntHandler);
    
    std::int64_t id = aeron->addPublication(settings.channel, settings.streamId);
    
    std::shared_ptr<Publication> publication = aeron->findPublication(id);
    while (!publication && running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      publication = aeron->findPublication(id);
    }

    if (!running) {
      std::cout << "Source: Aborted before publication was valid" << std::endl;
      delete buffer;
      return;
    }

    const std::int64_t channelStatus = publication->channelStatus();

    concurrent::AtomicBuffer srcBuffer(buffer, BUFFER_SIZE);
    int messages_sent = 0;

    // Give target a chance to listen
    std::this_thread::sleep_for(std::chrono::seconds(1));

    size_t total_bytes_sent = 0;

    long long start_time = get_usec();

    for (int i=0; i<NUM_ITERATIONS && running; ++i) {
      size_t bytes_sent = 0;
      while (bytes_sent < BUFFER_SIZE) {
	/*
	  srcBuffer.putBytes(0, 
	  reinterpret_cast<std::uint8_t *>(buffer + bytes_sent), 
	  BUFFER_SIZE - bytes_sent);
	*/
	srcBuffer.putBytes(0, 
			   buffer + bytes_sent,
			   BUFFER_SIZE - bytes_sent);
			   
	std::int64_t result = publication->offer(srcBuffer, 0, BUFFER_SIZE - bytes_sent);

	if (result > 0) {
	  // std::cerr << "Bytes sent: " << result << std::endl;
	bytes_sent += result;
	}
	else if (result == -1) {
	  // Back pressure
	  std::this_thread::sleep_for(std::chrono::microseconds(100));
	} else if (result == -2) {
	  // No subscribers, wait longer
	  std::this_thread::sleep_for(std::chrono::milliseconds(10));
	} else if (result == -3 || result == -4) {
	  // Publiscation error
	  running = false;
	  break;
	}
	
	
	if (!running) break;
	
	// std::this_thread::sleep_for(std::chrono::milliseconds(50));

	/*
	std::cerr << "(NUM_ITERATION, i): (" << NUM_ITERATIONS
		  << ", " << i << ") "
		  << "(BUFFER_SIZE, bytes_sent): ("
		  << BUFFER_SIZE << ", "
		  << bytes_sent << ")"
		  << std::endl;
	*/
      }

      total_bytes_sent += bytes_sent;
      
      // std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    long long end_time = get_usec();
    // printf("Aeron Queue SOURCE:\n============\n");
    printf("Aeron Queue source finished: [bytes sent: %d] %lld usec (%.2f MB/s)\n", 
           total_bytes_sent,
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));
    // print_bytes(buffer, BUFFER_SIZE);

    free(buffer);

}


//					//
// End: Aeron MQ			//
//					//

int main(int argc, char **argv) {

  if (argc > 1 && strcmp(argv[1], "--cleanup") == 0) {
    drain_message_queue(QUEUE_NAME);
    exit(0);
  }

  struct shared_data* shm = (struct shared_data*)mmap(NULL, sizeof(struct shared_data),
						      PROT_READ | PROT_WRITE,
						      MAP_SHARED | MAP_ANONYMOUS, -1, 0);

  if (shm == MAP_FAILED) {
    perror("mmap");
    exit(EXIT_FAILURE);
  }

  /*
  int efd = eventfd(0, EFD_SEMAPHORE);
  if (efd == -1) {
    perror("eventfd");
    exit(EXIT_FAILURE);
  }

    printf("Buffer size: %d bytes\n", BUFFER_SIZE);
    printf("Number of iterations: %d\n\n", NUM_ITERATIONS);

    printf("=== FIFO Benchmark ===\n");
    unlink(FIFO_PATH);
    
    if (mkfifo(FIFO_PATH, 0666) < 0) {
      if (errno != EEXIST) {
	perror("mkfifo failed");
	exit(EXIT_FAILURE);
      }
    }

    pid_t fifo_pid = fork();
    if (fifo_pid == 0) {
        fifo_target();
        exit(0);
    } else if (fifo_pid > 0) {
        sleep(1);  // Give receiver time to start
        fifo_source();
        wait(NULL);
    } else {
      perror("fork_failed");
      unlink(FIFO_PATH);
      exit(EXIT_FAILURE);
    }
    unlink(FIFO_PATH);

    printf("\n=== Unix Domain Socket Benchmark ===\n");
    pid_t socket_pid = fork();
    if (socket_pid == 0) {
        socket_target();
        exit(0);
    } else {
        socket_source();
        wait(NULL);
    }

    printf("\n=== Message Queue Benchmark ===\n");
    pid_t mq_pid = fork();
    if (mq_pid == 0) {
        mq_target();
        exit(0);
    } else {
        mq_source();
        wait(NULL);
    }

    printf("\n=== Shared Memory + eventfd Benchmark ===\n");
    pid_t shm_pid = fork();
    if (shm_pid == 0) {
        eventfd_receiver(efd, shm);
        exit(0);
    } else {
        eventfd_sender(efd, shm);
        wait(NULL);
    }
    close(efd);
    munmap(shm, sizeof(struct shared_data));

  */
    printf("\n=== TCP/IP Socket Benchmark ===\n");
    pid_t tcp_pid = fork();
    if (tcp_pid == 0) {
      tcp_target(54321);
      exit(0);
    } else if (tcp_pid > 0) {
      tcp_source(54321);
      wait(NULL);
    } else {
      perror("fork failed");
      exit(EXIT_FAILURE);
    }

    printf("\n== TCP/IP ZC Socket Benchmark===\n");
    pid_t zc_socket_pid = fork();
    if (zc_socket_pid == 0) {
      tcp_zc_socket_target(54324);
      exit(0);
    } else {
      tcp_zc_socket_source(54324);
      wait(NULL);
    }
    
    printf("\n=== UDP Socket Benchmark ===\n");
    pid_t udp_pid = fork();
    if (udp_pid == 0) {
      udp_target(54323);
      exit(0);
    } else if (udp_pid > 0) {
      udp_source(54323);
      wait(NULL);
    } else {
      perror("fork failed");
      exit,(EXIT_FAILURE);
    }

    /*
    printf("\n=== Splice Benchmark ===\n");
    pid_t splice_pid = fork();
    if (splice_pid == 0) {
      splice_target(true);
      exit(0);
    } else if (splice_pid > 0) {
      splice_source(true);
      wait(NULL);
    } else {
      perror("fork failed");
      exit(EXIT_FAILURE);
    }

  printf("\n=== Cross Memory Attach Benchmark ===\n");
  pid_t cma_pid = fork();
  if (cma_pid == 0) {
    cma_target();
    exit(0);
  } else if (cma_pid > 0) {
    cma_source();
    wait(NULL);
  } else {
    perror("fork failed");
    exit(EXIT_FAILURE);
  }

*/

  printf("\n=== Aeron Queue Attach Benchmark ===\n");
  pid_t aeron_pid = fork();
  if (aeron_pid == 0) {
    aeron_target();
    exit(0);
  } else if (aeron_pid > 0) {
    aeron_source();
    
    // Signal to child that we're done sending
    kill(aeron_pid, SIGUSR1);
    wait(NULL);
  } else {
    perror("fork failed");
    exit(EXIT_FAILURE);
  }

    return 0;
}
