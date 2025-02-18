#include <iostream>
#include <vector>
#include <string>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <array>

// Structure to hold results for each run
struct BenchmarkResult {
    size_t buffer_size;
    double send_time_usec;
    double recv_time_usec;
    double throughput_MBps;
};

// Structure to hold results for each IPC method
struct IPCResults {
    std::string method_name;
    std::vector<BenchmarkResult> results;
};

// Function to parse a line of benchmark output and extract timing information
bool parse_benchmark_line(const std::string& line, double& time_usec, double& throughput) {
    // Look for patterns like:
    // "... finished: [bytes sent/rec: XXXX] YYYY usec (ZZZZ MB/s)"
    size_t usec_pos = line.find("usec");
    size_t mbps_pos = line.find("MB/s");
    
    if (usec_pos == std::string::npos || mbps_pos == std::string::npos) {
        return false;
    }

    // Extract time (microseconds)
    size_t time_start = line.rfind(' ', usec_pos - 2) + 1;
    time_usec = std::stod(line.substr(time_start, usec_pos - time_start));

    // Extract throughput (MB/s)
    size_t tp_start = line.rfind('(') + 1;
    throughput = std::stod(line.substr(tp_start, mbps_pos - tp_start - 1));

    return true;
}

// Function to run benchmark with specific buffer size and collect results
std::vector<IPCResults> run_benchmarks(size_t buffer_size) {
    // Prepare command with buffer size
    std::string command = "./benchmarks " + std::to_string(buffer_size) + " > benchmark_output.txt 2>&1";
    
    // Run the benchmark
    int ret = system(command.c_str());
    if (ret != 0) {
        std::cerr << "Benchmark failed with buffer size: " << buffer_size << std::endl;
        return {};
    }

    // Read and parse results
    std::ifstream result_file("benchmark_output.txt");
    std::string line;
    std::vector<IPCResults> all_results;
    
    // Initialize results for each IPC method
    all_results.push_back({"FIFO", {}});
    all_results.push_back({"Unix Domain Socket", {}});
    all_results.push_back({"POSIX MQ", {}});
    all_results.push_back({"Shmem+Eventfd", {}});

    int current_method = -1;
    BenchmarkResult current_result = {buffer_size, 0.0, 0.0, 0.0};
    
    while (std::getline(result_file, line)) {
        if (line.find("=== FIFO") != std::string::npos) current_method = 0;
        else if (line.find("=== Unix Domain Socket") != std::string::npos) current_method = 1;
        else if (line.find("=== Message Queue") != std::string::npos) current_method = 2;
        else if (line.find("=== Shared Memory") != std::string::npos) current_method = 3;
        
        double time_usec, throughput;
        if (parse_benchmark_line(line, time_usec, throughput)) {
            if (line.find("source") != std::string::npos) {
                current_result.send_time_usec = time_usec;
                current_result.throughput_MBps = throughput;
            } else if (line.find("target") != std::string::npos) {
                current_result.recv_time_usec = time_usec;
                if (current_method >= 0) {
                    all_results[current_method].results.push_back(current_result);
                    current_result = {buffer_size, 0.0, 0.0, 0.0};
                }
            }
        }
    }

    return all_results;
}

int main() {
    std::vector<size_t> buffer_sizes;
    for (size_t i = 1; i <= 64; i++) {
        buffer_sizes.push_back(i * 1024);
    }

    // Vectors to store results for each IPC method
    std::vector<IPCResults> methods_results;
    
    // Run benchmarks for each buffer size
    for (size_t buffer_size : buffer_sizes) {
        std::cout << "Running benchmarks with buffer size: " << buffer_size << " bytes\n";
        auto results = run_benchmarks(buffer_size);
        
        // Store results
        if (methods_results.empty()) {
            methods_results = results;
        } else {
            for (size_t i = 0; i < results.size(); i++) {
                methods_results[i].results.insert(
                    methods_results[i].results.end(),
                    results[i].results.begin(),
                    results[i].results.end()
                );
            }
        }
    }

    // Print results in a formatted table
    std::cout << "\nBenchmark Results:\n";
    for (const auto& method : methods_results) {
        std::cout << "\n" << method.method_name << " Results:\n";
        std::cout << std::setw(12) << "Buffer Size" 
                  << std::setw(15) << "Send Time" 
                  << std::setw(15) << "Recv Time" 
                  << std::setw(15) << "Throughput\n";
        std::cout << std::string(60, '-') << "\n";
        
        for (const auto& result : method.results) {
            std::cout << std::setw(12) << result.buffer_size
                      << std::setw(15) << result.send_time_usec
                      << std::setw(15) << result.recv_time_usec
                      << std::setw(15) << result.throughput_MBps << "\n";
        }
    }

    return 0;
}
