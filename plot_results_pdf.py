import matplotlib.pyplot as plt
import numpy as np
import csv
import os

def read_benchmark_data(filename):
    buffer_sizes = []
    send_times = []
    recv_times = []
    throughputs = []
    
    with open(filename, 'r') as f:
        if 'Buffer_Size' in f.readline():
            pass
        
        for line in f:
            parts = line.strip().split(',')
            if len(parts) >= 3:
                buffer_size = int(parts[0])
                send_time = float(parts[1])
                throughput = float(parts[2])
                buffer_sizes.append(buffer_size)
                send_times.append(send_time)
                throughputs.append(throughput)
    
    return np.array(buffer_sizes), np.array(send_times), np.array(throughputs)

# Define our IPC methods with labels and colors
ipc_methods = {
    'fifo_results.csv': ('FIFO', '#1f77b4'),
    'socket_results.csv': ('Unix Domain Socket', '#2ca02c'),
    'mq_results.csv': ('POSIX Message Queue', '#ff7f0e'),
    'shm_results.csv': ('Shared Memory + Eventfd', '#d62728'),
    'tcp_results.csv': ('TCP Socket', '#9467bd'),
    'tcp_zc_results.csv' : ('TCP ZC Socket', '#a50d8d'),
    'udp_results.csv': ('UDP Socket', '#8c564b'),
    'splice_results.csv' : ('SPLICE', '#4ffd0b'),
    'cma_results.csv' : ('CMA', '#6a44cc'),
}

def create_subplot(methods_to_include, output_filename):
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 12))
    
    # Throughput subplot
    for filename, (label, color) in ipc_methods.items():
        if label in methods_to_include:
            buffer_sizes, send_times, throughputs = read_benchmark_data(f'benchmark_results_optimized/{filename}')
            ax1.plot(buffer_sizes / 1024, throughputs, 'x-', label=label, color=color, 
                    linewidth=1, markersize=8)
    ax1.set_xlabel('Buffer Size (KB)', fontsize=12)
    ax1.set_ylabel('Throughput (MB/s)', fontsize=12)
    ax1.set_title('IPC Methods Throughput Comparison', fontsize=14)
    ax1.grid(True, linestyle='--', alpha=0.7)
    ax1.legend(loc='upper left', bbox_to_anchor=(1, 1), fontsize=10)
    
    # Total time subplot
    for filename, (label, color) in ipc_methods.items():
        if label in methods_to_include:
            buffer_sizes, send_times, throughputs = read_benchmark_data(f'benchmark_results_optimized/{filename}')
            ax2.plot(buffer_sizes / 1024, send_times, 'x-', label=label, color=color, 
                    linewidth=1, markersize=8)
    ax2.set_xlabel('Buffer Size (KB)', fontsize=12)
    ax2.set_ylabel('Send Time (microseconds)', fontsize=12)
    ax2.set_title('IPC Methods Send Time Comparison', fontsize=14)
    ax2.grid(True, linestyle='--', alpha=0.7)
    ax2.legend(loc='upper left', bbox_to_anchor=(1, 1), fontsize=10)
    
    plt.tight_layout()
    plt.savefig(f'benchmark_results_optimized/{output_filename}.pdf', 
                format='pdf', bbox_inches='tight')
    plt.savefig(f'benchmark_results_optimized/{output_filename}.jpg',
                format='jpg', bbox_inches='tight')
    plt.savefig(f'benchmark_results_optimized/{output_filename}.png',
                format='png', bbox_inches='tight')
    plt.close()

# Create plots with all methods
all_methods = [
    'FIFO', 
    'Unix Domain Socket', 
    'POSIX Message Queue', 
    'Shared Memory + Eventfd',
    'TCP Socket',
    'TCP ZC Socket',
    'UDP Socket',
    'SPLICE',
    'CMA'
]
create_subplot(all_methods, 'ipc_performance_all_with_time')

# Create plots without shared memory
methods_no_shm = [
    'FIFO', 
    'Unix Domain Socket', 
    'POSIX Message Queue',
    'TCP Socket',
    'TCP ZC Socket',
    'UDP Socket',
    'SPLICE',
    'CMA'
]
create_subplot(methods_no_shm, 'ipc_performance_no_shm_with_time')

methods_tcp = [
    'TCP Socket',
    'TCP ZC Socket'
]

create_subplot(methods_tcp, 'ipc_performance_tcp_v_tcp_zc_with_time')

methods_mq_splice = [
    'POSIX Message Queue',
    'SPLICE'
]

create_subplot(methods_mq_splice, 'ipc_performance_mq_splice_with_time')


