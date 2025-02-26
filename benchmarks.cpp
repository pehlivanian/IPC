#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cassert>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
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

// Common definitions for all tests
#define SOCKET_PATH "/tmp/bench.sock"
#define FIFO_PATH "/tmp/bench.fifo"
#define QUEUE_NAME "/bench_queue"
#define SHM_NAME "/my_shared_mem"
#define BUFFER_SIZE (20 * 1024)
#define NUM_ITERATIONS 1000

// Shared structures
struct shared_data {
    size_t size;
    char buffer[BUFFER_SIZE];
};

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

long long get_usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

// This function reads and discards all messages from a queue until it's empty
void drain_message_queue(const char* queue_name) {
    // Open the queue for reading
    mqd_t mq = mq_open(queue_name, O_RDONLY | O_NONBLOCK);
    if (mq == (mqd_t)-1) {
        // If we can't open it, it might not exist, which is fine
        return;
    }

    // Get queue attributes to know message size
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
                // Queue is empty
                break;
            }
            // Some other error occurred
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
    printf("FIFO souce finished: [bytes sent: %d] %lld usec (%.2f MB/s)\n", 
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
        perror("Message queue open failed");
        exit(1);
    }
    
    int num_iterations = NUM_ITERATIONS;

    long long start_time = get_usec();
    
    while (num_iterations) {
      mq_send(mq, buffer, BUFFER_SIZE, 0);
      num_iterations--;
    }
    
    long long end_time = get_usec();

    // printf("POSIX MQ SOURCE:\n============\n");
    printf("POSIX MQ souce finished: [bytes sent: %d] %lld usec (%.2f MB/s)\n", 
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
        perror("Message queue open failed");
        exit(1);
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
    write(efd, &u, sizeof(uint64_t));

    num_iterations--;
  }

  long long end_time = get_usec();

  // printf("Shmem+Eventfd SOURCE:\n============\n");
  printf("Shmem+Eventfd souce finished: [bytes sent: %d] %lld usec (%.2f MB/s)\n", 
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
    read(efd, &u, sizeof(uint64_t));
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
      exit(EXIT_FAILURE);
    }

    return 0;
}
