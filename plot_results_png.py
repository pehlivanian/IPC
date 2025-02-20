import matplotlib.pyplot as plt
import numpy as np
import csv

# First, let's create a function to read our CSV data
def read_benchmark_data(filename):
    """
    Reads benchmark data from a file and returns buffer sizes and throughput values.
    The file should have format: buffer_size,send_time,throughput
    """
    buffer_sizes = []
    throughputs = []
    
    with open(filename, 'r') as f:
        # Skip header if it exists
        if 'Buffer_Size' in f.readline():
            pass
        
        # Read the actual data
        for line in f:
            # Split the CSV line and convert to appropriate types
            parts = line.strip().split(',')
            if len(parts) >= 3:  # Ensure we have at least 3 values
                buffer_size = int(parts[0])
                throughput = float(parts[2])
                buffer_sizes.append(buffer_size)
                throughputs.append(throughput)
    
    return np.array(buffer_sizes), np.array(throughputs)

# Create the figure with a reasonable size
plt.figure(figsize=(12, 8))

# Read data for each IPC method
# Dictionary to store our data with nice labels and color scheme
ipc_methods = {
    'fifo_results.csv': ('FIFO', '#1f77b4'),  # Blue
    'socket_results.csv': ('Unix Domain Socket', '#2ca02c'),  # Green
    'mq_results.csv': ('POSIX Message Queue', '#ff7f0e'),  # Orange
    'shm_results.csv': ('Shared Memory + Eventfd', '#d62728')  # Red
}

# Plot data for each method
for filename, (label, color) in ipc_methods.items():
    buffer_sizes, throughputs = read_benchmark_data(f'benchmark_results_unoptimized/{filename}')
    
    # Create the plot with a distinctive style for each method
    plt.plot(buffer_sizes / 1024, throughputs, 'o-', label=label, color=color, linewidth=2, markersize=8)

# Customize the plot
plt.xlabel('Buffer Size (KB)', fontsize=12)
plt.ylabel('Throughput (MB/s)', fontsize=12)
plt.title('IPC Methods Performance Comparison', fontsize=14, pad=20)

# Add grid for better readability
plt.grid(True, linestyle='--', alpha=0.7)

# Customize the legend
plt.legend(loc='upper left', bbox_to_anchor=(1, 1), fontsize=10)

# Adjust layout to prevent label cutoff
plt.tight_layout()

# Add text box with key observations
plt.figtext(0.99, 0.02, 
            'Key Observations:\n' +
            '%G•%@ Shared Memory shows highest throughput\n' +
            '%G•%@ All methods improve with larger buffers\n' +
            '%G•%@ FIFO and Sockets show similar patterns\n' +
            '%G•%@ Message Queue performance is intermediate',
            fontsize=10, ha='right', va='bottom',
            bbox=dict(boxstyle='round', facecolor='white', alpha=0.8))

# Save the plot
plt.savefig('benchmark_results_unoptimized/ipc_performance_comparison.png', 
            dpi=300, bbox_inches='tight')
plt.close()

print("Plot has been saved as 'ipc_performance_comparison.png'")
