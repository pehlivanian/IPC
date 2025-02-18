#!/bin/bash

# Create a directory for results if it doesn't exist
mkdir -p benchmark_results_unoptimized

adjust_system_limits() {
    echo 65536 > /proc/sys/fs/mqueue/msg_max
    echo 65536 > /proc/sys/fs/mqueue/msgsize_max
    echo 65536 > /proc/sys/fs/mqueue/queues_max
}


check_mq_status() {
    echo "Current message queues:"
    ls -l /dev/mqueue/
    echo "Message queue limits:"
    cat /proc/sys/fs/mqueue/queues_max
    cat /proc/sys/fs/mqueue/msg_max
    cat /proc/sys/fs/mqueue/msgsize_max
}

cleanup_resources() {

    rm -f /dev/mqueue/bench_queue
    
    # Remove all message queues owned by current user
    for mq in $(ls /dev/mqueue/); do
        rm -f "/dev/mqueue/$mq"
    done
    
    # Force cleanup of the specific queue we use
    mq_unlink() { 
        if [ -e "/dev/mqueue/bench_queue" ]; then
            rm -f "/dev/mqueue/bench_queue"
        fi
    }
    
    # Remove any leftover FIFO and socket files
    rm -f /tmp/bench.fifo
    rm -f /tmp/bench.sock
    
    # Wait for cleanup to complete
    sleep 1
    
    # Verify cleanup
    if [ -e "/dev/mqueue/bench_queue" ]; then
        echo "Warning: Message queue still exists after cleanup"
    fi
}

# Function to run benchmark and extract results
run_benchmark() {
    local size=$1
    local output_file="benchmark_results_unoptimized/size_${size}.txt"
    
    echo "Starting benchmark for size $size"
    echo "Before benchmark:"
    check_mq_status

    ./benchmarks --cleanup
    cleanup_resources

    echo "After cleanup:"
    check_mq_status

    # Create a temporary copy of the source file
    cp benchmarks.cpp benchmarks_temp.cpp
    
    # Replace the BUFFER_SIZE definition
    sed -i "s/#define BUFFER_SIZE.*/#define BUFFER_SIZE (${size})/" benchmarks_temp.cpp
    
    # Compile
    g++ -o benchmarks benchmarks_temp.cpp -lrt
    if [ $? -ne 0 ]; then
        echo "Compilation failed for buffer size ${size}"
        return 1
    fi
    
    # Run and capture output
    ./benchmarks > "${output_file}"

    # Check if the benchmark completed successfully
    if [ $? -ne 0 ]; then
	echo "Benchmark failed for buffer size ${size}"
	cat "${output_file}"
	return 1
    fi
    
    # Clean up
    rm benchmarks_temp.cpp

    echo "After benchmark:"
    check_mq_status
}

extract_metrics() {
    local output_file=$1
    local pattern=$2
    local size=$3
    
    case "$pattern" in
        "FIFO")
            # Get the last occurrence of the source timing
            local source_line=$(grep "FIFO souce finished:" "$output_file" | tail -n 1)
            local source_time=$(echo "$source_line" | awk '{for(i=1;i<=NF;i++) if($i=="usec") print $(i-1)}')
            local throughput=$(echo "$source_line" | awk -F'[()]' '{print $2}' | awk '{print $1}')
            echo "$size,$source_time,$throughput"
            ;;
            
        "UNIX Domain Socket")
            local source_line=$(grep "UNIX Domain Socket source finished:" "$output_file" | tail -n 1)
            local source_time=$(echo "$source_line" | awk '{for(i=1;i<=NF;i++) if($i=="usec") print $(i-1)}')
            local throughput=$(echo "$source_line" | awk -F'[()]' '{print $2}' | awk '{print $1}')
            echo "$size,$source_time,$throughput"
            ;;
            
        "POSIX MQ")
            local source_line=$(grep "POSIX MQ souce finished:" "$output_file" | tail -n 1)
            local source_time=$(echo "$source_line" | awk '{for(i=1;i<=NF;i++) if($i=="usec") print $(i-1)}')
            local throughput=$(echo "$source_line" | awk -F'[()]' '{print $2}' | awk '{print $1}')
            echo "$size,$source_time,$throughput"
            ;;
            
        "Shmem+Eventfd")
            local source_line=$(grep "Shmem+Eventfd souce finished:" "$output_file" | tail -n 1)
            local source_time=$(echo "$source_line" | awk '{for(i=1;i<=NF;i++) if($i=="usec") print $(i-1)}')
            local throughput=$(echo "$source_line" | awk -F'[()]' '{print $2}' | awk '{print $1}')
            echo "$size,$source_time,$throughput"
            ;;
    esac
}

# Initialize results files with headers
echo "Buffer_Size,Send_Time_usec,Recv_Time_usec,Throughput_MBps" > benchmark_results_unoptimized/all_results.csv

# Create summary file
echo "Benchmark Summary" > benchmark_results_unoptimized/summary.txt
echo "=================" >> benchmark_results_unoptimized/summary.txt
echo "" >> benchmark_results_unoptimized/summary.txt

# Generate array of buffer sizes from 1KB to 49KB
sizes=()
for ((i=1; i<=48; i++)); do
    sizes+=($((i * 1024)))
done

# Set system resources
adjust_system_limits

# Initial cleanup
./benchmarks --cleanup
cleanup_resources

# Create CSV files for results
# Initialize result files with headers
echo "Buffer_Size,Send_Time,Recv_Time,Throughput" > benchmark_results_unoptimized/fifo_results.csv
echo "Buffer_Size,Send_Time,Recv_Time,Throughput" > benchmark_results_unoptimized/socket_results.csv
echo "Buffer_Size,Send_Time,Recv_Time,Throughput" > benchmark_results_unoptimized/mq_results.csv
echo "Buffer_Size,Send_Time,Recv_Time,Throughput" > benchmark_results_unoptimized/shm_results.csv

# Run benchmarks for each size
# Run benchmarks for each size
for size in "${sizes[@]}"; do
    echo "Running benchmark with buffer size: $size bytes"
    
    # Run benchmark and capture its return status immediately
    run_benchmark $size
    benchmark_status=$?

    # Skip result processing if the benchmark failed
    if [ $benchmark_status -ne 0 ]; then
        echo "Skipping result processing for buffer size $size"
        continue
    fi

    output_file="benchmark_results_unoptimized/size_${size}.txt"
    
    # Check for message queue success
    if ! grep -q "POSIX MQ target finished" "$output_file"; then
        echo "Message queue benchmark failed for size $size"
        echo "Last few lines of output:"
        tail -n 5 "$output_file"
        ./benchmarks --cleanup
        cleanup_resources
        continue
    fi

    # Extract metrics for each IPC method
    fifo_metrics=$(extract_metrics "${output_file}" "FIFO" "${size}")
    socket_metrics=$(extract_metrics "${output_file}" "UNIX Domain Socket" "${size}")
    mq_metrics=$(extract_metrics "${output_file}" "POSIX MQ" "${size}")
    shm_metrics=$(extract_metrics "${output_file}" "Shmem+Eventfd" "${size}")
    
    # Write results to individual files
    [ -n "$fifo_metrics" ] && echo "$fifo_metrics" >> benchmark_results_unoptimized/fifo_results.csv
    [ -n "$socket_metrics" ] && echo "$socket_metrics" >> benchmark_results_unoptimized/socket_results.csv
    [ -n "$mq_metrics" ] && echo "$mq_metrics" >> benchmark_results_unoptimized/mq_results.csv
    [ -n "$shm_metrics" ] && echo "$shm_metrics" >> benchmark_results_unoptimized/shm_results.csv

    # Also write to the combined results file
    echo "${size},${fifo_metrics}" >> benchmark_results_unoptimized/all_results.csv
    echo "${size},${socket_metrics}" >> benchmark_results_unoptimized/all_results.csv
    echo "${size},${mq_metrics}" >> benchmark_results_unoptimized/all_results.csv
    echo "${size},${shm_metrics}" >> benchmark_results_unoptimized/all_results.csv
    
    # Add debug output to verify data is being written
    echo "Processed results for size ${size}:"
    echo "FIFO: ${size},${fifo_metrics}"
    echo "Socket: ${size},${socket_metrics}"
    echo "MQ: ${size},${mq_metrics}"
    echo "SHM: ${size},${shm_metrics}"
done

# Create a summary report
echo "Benchmark Summary" > benchmark_results_unoptimized/summary.txt
echo "=================" >> benchmark_results_unoptimized/summary.txt

for method in fifo socket mq shm; do
    echo "" >> benchmark_results_unoptimized/summary.txt
    echo "$(tr '[:lower:]' '[:upper:]' <<< ${method}) Results:" >> benchmark_results_unoptimized/summary.txt
    echo "Buffer Size (B), Send Time (-Fìs), Recv Time (ìs), Throughput (MB/s)" >> benchmark_results_unoptimized/summary.txt-A
    cat "benchmark_results_unoptimized/${method}_results.csv" | tail -n +2 >> benchmark_results_unoptimized/summary.txt
done

echo "Benchmarks complete. Results are in the benchmark_results_unoptimized directory."
