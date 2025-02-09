#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <errno.h>
#include <cstdint>

#define BUFFER_SIZE (1024 * 1024)  // 1MB buffer
#define NUM_ITERATIONS 1000
#define SOCKET_PATH "/tmp/bench.sock"
#define FIFO_PATH "/tmp/bench.fifo"
#define SHM_NAME "/ipc_bench_shm"

// Get time in microseconds
long long get_usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

struct shared_data {
  size_t size;
  char buffer[BUFFER_SIZE];
};

// Shared Memory Implementation
void shm_sender(void) {
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open sender");
        exit(1);
    }

    char* buffer = (char *)mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE,
                       MAP_SHARED, shm_fd, 0);
    if (buffer == MAP_FAILED) {
        perror("mmap sender");
        exit(1);
    }

    long long start_time = get_usec();
    
    // Write pattern
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        memset(buffer, 'A' + (i % 26), BUFFER_SIZE);
        buffer[BUFFER_SIZE-1] = '\n';  // Mark completion
    }

    long long end_time = get_usec();
    printf("SHM Sender finished: %lld usec, %.2f MB/s\n",
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));

    munmap(buffer, BUFFER_SIZE);
    close(shm_fd);
}

void shm_receiver(void) {
    shm_unlink(SHM_NAME);
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open receiver");
        exit(1);
    }

    if (ftruncate(shm_fd, BUFFER_SIZE) == -1) {
        perror("ftruncate");
        exit(1);
    }

    char* buffer = (char *)mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE,
                       MAP_SHARED, shm_fd, 0);
    if (buffer == MAP_FAILED) {
        perror("mmap receiver");
        exit(1);
    }

    long long start_time = get_usec();
    
    // Read pattern
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        while (buffer[BUFFER_SIZE-1] != '\n') {
            usleep(1);  // Small delay to prevent busy waiting
        }
        buffer[BUFFER_SIZE-1] = 0;  // Reset completion marker
    }

    long long end_time = get_usec();
    printf("SHM Receiver finished: %lld usec, %.2f MB/s\n",
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));

    munmap(buffer, BUFFER_SIZE);
    close(shm_fd);
    shm_unlink(SHM_NAME);
}

void eventfd_sender(int efd, struct shared_data* shm) {
    // Initialize test data
    char test_data[BUFFER_SIZE];
    memset(test_data, 'A', BUFFER_SIZE);

    long long start_time = get_usec();

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        // Write data to shared memory
        memcpy(shm->buffer, test_data, BUFFER_SIZE);
        shm->size = BUFFER_SIZE;

        // Signal receiver using eventfd
        uint64_t u = 1;
        if (write(efd, &u, sizeof(uint64_t)) != sizeof(uint64_t)) {
            perror("write eventfd");
            exit(1);
        }
    }

    long long end_time = get_usec();
    printf("Sender finished: %lld usec\n", end_time - start_time);
}

void eventfd_receiver(int efd, struct shared_data* shm) {
    long long start_time = get_usec();

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        // Wait for signal from sender
        uint64_t u;
        if (read(efd, &u, sizeof(uint64_t)) != sizeof(uint64_t)) {
            perror("read eventfd");
            exit(1);
        }

        // Process data from shared memory
        // In this example, we just verify the size
        if (shm->size != BUFFER_SIZE) {
            fprintf(stderr, "Size mismatch: expected %d, got %zu\n", 
                    BUFFER_SIZE, shm->size);
            exit(1);
        }
    }

    long long end_time = get_usec();
    printf("Receiver finished: %lld usec\n", end_time - start_time);
}


// FIFO Implementation
void fifo_sender(void) {
    int fd = open(FIFO_PATH, O_WRONLY);
    if (fd < 0) {
        perror("FIFO open failed");
        exit(1);
    }

    char* buffer = (char *)malloc(BUFFER_SIZE);
    memset(buffer, 'A', BUFFER_SIZE);

    long long start_time = get_usec();

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        size_t remaining = BUFFER_SIZE;
        while (remaining > 0) {
            ssize_t written = write(fd, buffer + (BUFFER_SIZE - remaining), remaining);
            if (written < 0) {
                perror("FIFO write failed");
                exit(1);
            }
            remaining -= written;
        }
    }

    long long end_time = get_usec();
    printf("FIFO Sender finished: %lld usec, %.2f MB/s\n",
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));

    close(fd);
    free(buffer);
}

void fifo_receiver(void) {
    int fd = open(FIFO_PATH, O_RDONLY);
    if (fd < 0) {
        perror("FIFO open failed");
        exit(1);
    }

    char* buffer = (char *)malloc(BUFFER_SIZE);
    long long start_time = get_usec();

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        size_t remaining = BUFFER_SIZE;
        while (remaining > 0) {
            ssize_t bytes_read = read(fd, buffer + (BUFFER_SIZE - remaining), remaining);
            if (bytes_read < 0) {
                perror("FIFO read failed");
                exit(1);
            }
            remaining -= bytes_read;
        }
    }

    long long end_time = get_usec();
    printf("FIFO Receiver finished: %lld usec, %.2f MB/s\n",
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));

    close(fd);
    free(buffer);
}

// Unix Domain Socket Implementation
void socket_sender(void) {
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Socket connect failed");
        exit(1);
    }

    char* buffer = (char *)malloc(BUFFER_SIZE);
    memset(buffer, 'A', BUFFER_SIZE);

    long long start_time = get_usec();

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        size_t remaining = BUFFER_SIZE;
        while (remaining > 0) {
            ssize_t sent = send(sock_fd, buffer + (BUFFER_SIZE - remaining), 
                              remaining, 0);
            if (sent < 0) {
                perror("Socket send failed");
                exit(1);
            }
            remaining -= sent;
        }
    }

    long long end_time = get_usec();
    printf("Socket Sender finished: %lld usec, %.2f MB/s\n",
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));

    close(sock_fd);
    free(buffer);
}

void socket_receiver(void) {
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    unlink(SOCKET_PATH);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Socket bind failed");
        exit(1);
    }

    if (listen(server_fd, 1) < 0) {
        perror("Socket listen failed");
        exit(1);
    }

    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("Socket accept failed");
        exit(1);
    }

    char* buffer = (char *)malloc(BUFFER_SIZE);
    long long start_time = get_usec();

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        size_t remaining = BUFFER_SIZE;
        while (remaining > 0) {
            ssize_t received = recv(client_fd, buffer + (BUFFER_SIZE - remaining),
                                  remaining, 0);
            if (received < 0) {
                perror("Socket receive failed");
                exit(1);
            }
            remaining -= received;
        }
    }

    long long end_time = get_usec();
    printf("Socket Receiver finished: %lld usec, %.2f MB/s\n",
           end_time - start_time,
           ((double)BUFFER_SIZE * NUM_ITERATIONS) / (end_time - start_time));

    close(client_fd);
    close(server_fd);
    unlink(SOCKET_PATH);
    free(buffer);
}

int main(void) {
    printf("Starting benchmark...\n");
    printf("Buffer size: %d bytes\n", BUFFER_SIZE);
    printf("Iterations: %d\n\n", NUM_ITERATIONS);

    printf("=== Shared Memory Test ===\n");
    pid_t shm_pid = fork();
    if (shm_pid == 0) {
        shm_receiver();
        exit(0);
    } else {
        sleep(1);  // Give receiver time to set up
        shm_sender();
        wait(NULL);
    }

    printf("\n=== FIFO Test ===\n");
    mkfifo(FIFO_PATH, 0666);
    pid_t fifo_pid = fork();
    if (fifo_pid == 0) {
        fifo_receiver();
        exit(0);
    } else {
        fifo_sender();
        wait(NULL);
    }
    unlink(FIFO_PATH);

    printf("\n=== Unix Domain Socket Test ===\n");
    pid_t socket_pid = fork();
    if (socket_pid == 0) {
        socket_receiver();
        exit(0);
    } else {
        sleep(1);  // Give receiver time to set up
        socket_sender();
        wait(NULL);
    }

    printf("\n=== Shared Memory + eventfd Test ===\n");
    int efd = eventfd(0, EFD_SEMAPHORE);
    if (efd == -1) {
      perror("eventfd");
      return 1;
    }
    
    struct shared_data* shm = (struct shared_data*)mmap(NULL, sizeof(struct shared_data),
						      PROT_READ | PROT_WRITE,
						      MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if (shm == MAP_FAILED) {
      perror("mmap");
      close(efd);
      return 1;
    }
  

    pid_t eventfd_pid = fork();
    if (eventfd_pid == 0) {
        eventfd_receiver(efd, shm);
        exit(0);
    } else {
        eventfd_sender(efd, shm);
        wait(NULL);
    }
    
    close(efd);

    return 0;
}
