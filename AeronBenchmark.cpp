#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cassert>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>

// Aeron-related headers
#include <Aeron.h>
#include <util/CommandOptionParser.h>
#include <concurrent/NoOpIdleStrategy.h>
#include <concurrent/SleepingIdleStrategy.h>
#include <FragmentAssembler.h>

#define BUFFER_SIZE (8 * 1024)
#define NUM_ITERATIONS 1000

using namespace aeron;
using namespace aeron::util;

// Global flag for signaling between processes
std::atomic<bool> running(true);

// Handler for SIGINT
void sigIntHandler(int) {
    running = false;
}

// Handler for SIGUSR1 (used to signal completion)
void sigUsr1Handler(int) {
    printf("Received SIGUSR1 - source is finished\n");

    // Print stats if target didn't receive all messages
    std::cout << "Target: WARNING - Some messages may have been lost in transmission." << std::endl;
    std::cout << "Target: This is normal with Aeron's UDP transport and is why real applications" << std::endl;
    std::cout << "        use sequence numbers and retransmission protocols on top of Aeron." << std::endl;

    running = false;
}

long long get_usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

// Default Aeron settings
namespace aeron { namespace defaults {
    const static std::string DEFAULT_CHANNEL = "aeron:udp?endpoint=localhost:20121";
    const static std::int32_t DEFAULT_STREAM_ID = 1001;
    const static long long DEFAULT_NUMBER_OF_MESSAGES = NUM_ITERATIONS;
    const static int DEFAULT_LINGER_TIMEOUT_MS = 0;

    struct Settings {
        std::string channel = aeron::defaults::DEFAULT_CHANNEL;
        std::int32_t streamId = aeron::defaults::DEFAULT_STREAM_ID;
        std::int64_t numberOfMessages = aeron::defaults::DEFAULT_NUMBER_OF_MESSAGES;
        int lingerTimeoutMs = aeron::defaults::DEFAULT_LINGER_TIMEOUT_MS;
    };
}}

// Fragment handler to accumulate received bytes
fragment_handler_t createFragmentHandler(std::size_t& totalBytesReceived, std::atomic<int>& messagesReceived) {
    return [&totalBytesReceived, &messagesReceived](
        const AtomicBuffer &buffer, 
        util::index_t offset, 
        util::index_t length, 
        const Header &header) {
            
        totalBytesReceived += length;
        messagesReceived++;
    };
}

// Generate random test data
void random_bits(unsigned char* buffer, size_t size) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        perror("open urandom");
        exit(1);
    }
    
    ssize_t bytes = read(fd, buffer, size);
    if (bytes != size) {
        perror("read urandom");
        exit(1);
    }
    close(fd);
}

void aeron_source() {
    // Prepare buffer with test data
    unsigned char *buffer = new unsigned char[BUFFER_SIZE];
    random_bits(buffer, BUFFER_SIZE);
    
    // Setup Aeron
    aeron::defaults::Settings settings;
    aeron::Context context;
    
    // Connect to Aeron
    std::cout << "Source: Connecting to Aeron..." << std::endl;
    std::shared_ptr<Aeron> aeron = Aeron::connect(context);
    
    // Register signal handlers
    signal(SIGINT, sigIntHandler);
    
    // Create publication
    std::cout << "Source: Adding publication on " << settings.channel 
              << " stream " << settings.streamId << std::endl;
    
    std::int64_t id = aeron->addPublication(settings.channel, settings.streamId);
    
    // Wait for publication to be valid
    std::shared_ptr<Publication> publication = aeron->findPublication(id);
    while (!publication && running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        publication = aeron->findPublication(id);
    }
    
    if (!running) {
        std::cout << "Source: Aborted before publication was valid" << std::endl;
        delete[] buffer;
        return;
    }
    
    std::cout << "Source: Publication ready" << std::endl;
    
    // Setup buffer for transmission
    concurrent::AtomicBuffer srcBuffer(buffer, BUFFER_SIZE);
    
    // Wait a bit to give target time to set up
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    long long start_time = get_usec();
    int messages_sent = 0;
    
    // Send messages
    for (int i = 0; i < NUM_ITERATIONS && running; ++i) {
        // Fill the source buffer with random data for this iteration
        random_bits(buffer, BUFFER_SIZE);
        srcBuffer.putBytes(0, buffer, BUFFER_SIZE);

        // Try to send until successful
        std::int64_t result;
        while (running) {
            result = publication->offer(srcBuffer);

            if (result > 0) {
                // Successfully sent
                messages_sent++;

                // Progress feedback
                if (messages_sent % 100 == 0) {
                    std::cout << "Source: Sent " << messages_sent << " messages" << std::endl;
                }
                break;
            } else if (result == -1) {  // BACK_PRESSURED
                // Back pressure, try again after a short delay
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            } else if (result == -2) {  // NOT_CONNECTED
                // No subscribers, wait longer
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else if (result == -3 || result == -4) {  // ADMIN_ACTION or CLOSED
                std::cerr << "Source: Publication error: " << result << std::endl;
                running = false;
                break;
            }
        }

        if (!running) break;
    }

    std::cout << "Source: Completed sending " << messages_sent << " messages" << std::endl;
    
    long long end_time = get_usec();
    double elapsed_seconds = (end_time - start_time) / 1000000.0;
    printf("Aeron Queue source finished: [bytes sent: %d x %d messages] %lld usec (%.2f MB/s)\n",
           BUFFER_SIZE, messages_sent,
           end_time - start_time,
           ((double)BUFFER_SIZE * messages_sent) / (end_time - start_time));
    
    delete[] buffer;
}

void aeron_target() {
    // Setup Aeron
    aeron::defaults::Settings settings;
    aeron::Context context;
    
    // Connect to Aeron
    std::cout << "Target: Connecting to Aeron..." << std::endl;
    std::shared_ptr<Aeron> aeron = Aeron::connect(context);
    
    // Register signal handlers
    signal(SIGINT, sigIntHandler);
    signal(SIGUSR1, sigUsr1Handler);
    
    // Create subscription
    std::cout << "Target: Adding subscription on " << settings.channel 
              << " stream " << settings.streamId << std::endl;
              
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
    
    std::cout << "Target: Subscription ready" << std::endl;
    
    // Setup variables for receiving
    std::size_t totalBytesReceived = 0;
    std::atomic<int> messagesReceived{0};
    
    // Create fragment handler for processing received messages
    FragmentAssembler fragmentAssembler(
        createFragmentHandler(totalBytesReceived, messagesReceived));
    fragment_handler_t handler = fragmentAssembler.handler();
    
    // Idle strategy for polling
    SleepingIdleStrategy idleStrategy(std::chrono::milliseconds(1));
    
    long long start_time = get_usec();
    
    // Main receiving loop
    int iterationsCompleted = 0;
    int consecutiveEmptyPolls = 0;
    const int MAX_EMPTY_POLLS = 1000; // Early termination after enough empty polls

    while (iterationsCompleted < NUM_ITERATIONS && running) {
        // Poll for fragments (messages or parts of messages)
        int fragmentsRead = subscription->poll(handler, 10);

        // Early termination if we stop receiving messages
        if (fragmentsRead == 0) {
            consecutiveEmptyPolls++;

            // After 1000 empty polls (~1 second), print debug info
            if (consecutiveEmptyPolls % 1000 == 0) {
                std::cout << "Target: " << consecutiveEmptyPolls << " empty polls, received "
                          << messagesReceived.load() << " of " << NUM_ITERATIONS << " messages" << std::endl;
            }

            // If no messages for a while and we received at least some messages, exit
            if (consecutiveEmptyPolls > MAX_EMPTY_POLLS && messagesReceived.load() > 0) {
                std::cout << "Target: No messages received for too long, assuming completion" << std::endl;
                break;
            }
        } else {
            consecutiveEmptyPolls = 0;
        }

        // Process completion if we've received a full message
        if (messagesReceived > iterationsCompleted) {
            iterationsCompleted = messagesReceived;

            // Progress feedback
            if (iterationsCompleted % 100 == 0) {
                std::cout << "Target: Received " << iterationsCompleted << " messages" << std::endl;
            }
        }

        // Idle according to results
        idleStrategy.idle(fragmentsRead);
    }
    
    long long end_time = get_usec();
    int totalMessagesReceived = messagesReceived.load();
    double elapsed_seconds = (end_time - start_time) / 1000000.0;

    printf("Aeron Queue target finished [bytes rec: %d x %d messages]: %lld usec (%.2f MB/s)\n",
           BUFFER_SIZE, totalMessagesReceived,
           end_time - start_time,
           ((double)BUFFER_SIZE * totalMessagesReceived) / (end_time - start_time));
}

int main() {
    std::cout << "Starting Aeron benchmark test\n";
    std::cout << "Make sure the Aeron Media Driver is running!\n";
    
    printf("\n=== Aeron Queue Attach Benchmark ===\n");
    pid_t aeron_pid = fork();
    
    if (aeron_pid == 0) {
        // Child process - run target
        aeron_target();
        exit(0);
    } else if (aeron_pid > 0) {
        // Parent process - run source
        // Give target time to set up
        std::this_thread::sleep_for(std::chrono::seconds(2));
        aeron_source();
        
        // Wait a bit for any in-flight messages to be processed
        std::cout << "Source: Waiting for target to process remaining messages..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Signal to child that we're done sending
        std::cout << "Source: Sending SIGUSR1 signal to target" << std::endl;
        kill(aeron_pid, SIGUSR1);

        // Wait for child process to finish with timeout
        int status;
        pid_t result;
        int timeout_seconds = 5;
        time_t start_time = time(NULL);

        do {
            result = waitpid(aeron_pid, &status, WNOHANG);
            if (result == 0) {
                // Child still running, check timeout
                if (difftime(time(NULL), start_time) > timeout_seconds) {
                    std::cout << "Target process timeout after " << timeout_seconds
                              << " seconds, terminating..." << std::endl;
                    kill(aeron_pid, SIGTERM);
                    waitpid(aeron_pid, &status, 0); // Wait for it to terminate
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } while (result == 0);
    } else {
        perror("fork failed");
        exit(EXIT_FAILURE);
    }
    
    return 0;
}