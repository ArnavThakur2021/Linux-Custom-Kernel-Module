#!/usr/bin/env python3
import subprocess
import time
import re
import sys
import os

def send_control(cmd):
    try:
        with open('/proc/netspeed_ctl', 'w') as f:
            f.write(cmd)
    except Exception as e:
        print(f"[-] Failed to send command '{cmd}': {e}")

def measure_ping(count=10):
    """Measures average latency (RTT) and packet loss to 8.8.8.8"""
    try:
        cmd = ["ping", "-c", str(count), "-i", "0.2", "8.8.8.8"]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        out = res.stdout

        # Extract RTT avg
        rtt_match = re.search(r'rtt min/avg/max/mdev = [\d\.]+/([\d\.]+)/', out)
        avg_rtt = float(rtt_match.group(1)) if rtt_match else 0.0

        # Extract Loss %
        loss_match = re.search(r'(\d+)% packet loss', out)
        loss = float(loss_match.group(1)) if loss_match else 0.0

        return avg_rtt, loss
    except Exception:
        return 0.0, 100.0

def run_iperf_background(duration=10):
    """Starts iperf3 background heavy load on localhost"""
    # Requires iperf3 server running: 'iperf3 -s -D'
    return subprocess.Popen(
        ["iperf3", "-c", "127.0.0.1", "-t", str(duration), "-b", "100M"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )

def run_single_test(mode_name, control_cmd):
    print(f"\n[+] Running Test Phase: {mode_name}...")
    send_control(control_cmd)
    time.sleep(1)  # Allow mode setting to settle

    # Start Heavy Outbound Traffic
    iperf_proc = run_iperf_background(duration=8)

    # Measure Interactive Latency under congestion
    time.sleep(1)  # Wait for link to saturate
    avg_rtt, loss = measure_ping(count=15)

    # Collect iperf3 output metrics
    stdout, stderr = iperf_proc.communicate()
    
    # Parse iperf throughput (bits/sec -> Mbits/sec)
    tp_match = re.search(r'(\d+\.?\d*)\s+Mbits/sec\s+receiver', stdout)
    if not tp_match:
        tp_match = re.search(r'(\d+\.?\d*)\s+Mbits/sec\s+sender', stdout)
    
    heavy_tp = float(tp_match.group(1)) if tp_match else 0.0

    return {
        "avg_rtt": avg_rtt,
        "loss": loss,
        "heavy_tp": heavy_tp
    }

def main():
    if os.geteuid() != 0:
        print("[-] Please run this script with sudo.")
        sys.exit(1)

    print("==================================================")
    print("      NetMLFQ Throughput & Latency Benchmark     ")
    print("==================================================")

    # Ensure local iperf3 server is active
    subprocess.run(["pkill", "iperf3"], stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    subprocess.Popen(["iperf3", "-s"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)

    # Test 1: BEFORE (Module Off / Standard Linux Stack)
    before_stats = run_single_test("BEFORE (Module OFF)", "mode off")

    # Test 2: AFTER (Module ON / MLFQ Active)
    after_stats = run_single_test("AFTER (NetMLFQ Active)", "mode mlfq")

    # Clean up iperf3 background process
    subprocess.run(["pkill", "iperf3"], stderr=subprocess.DEVNULL)
    send_control("mode off")

    # Display Side-by-Side Comparison Terminal Summary
    print("\n" + "="*65)
    print("                     BENCHMARK RESULTS REPORT                   ")
    print("="*65)
    print(f"{'METRIC':<30} | {'BEFORE (Standard)':<15} | {'AFTER (NetMLFQ)':<15}")
    print("-" * 65)
    print(f"{'Interactive Latency (Ping RTT)':<30} | {before_stats['avg_rtt']:<12.2f} ms | {after_stats['avg_rtt']:<12.2f} ms")
    print(f"{'Interactive Packet Loss':<30} | {before_stats['loss']:<12.1f} %  | {after_stats['loss']:<12.1f} %")
    print(f"{'Heavy Background Throughput':<30} | {before_stats['heavy_tp']:<12.2f} Mbps| {after_stats['heavy_tp']:<12.2f} Mbps")
    print("-" * 65)

    # Calculate Improvement
    if before_stats['avg_rtt'] > 0:
        rtt_impr = ((before_stats['avg_rtt'] - after_stats['avg_rtt']) / before_stats['avg_rtt']) * 100
        print(f"[*] Interactive Latency Improvement: {rtt_impr:.1f}% reduction in delay.")

if __name__ == '__main__':
    main()

#to run in terminal: sudo python3 benchmark.py and this is the sample output:

# ==================================================
#       NetMLFQ Throughput & Latency Benchmark     
# ==================================================

# [+] Running Test Phase: BEFORE (Module OFF)...
# [+] Running Test Phase: AFTER (NetMLFQ Active)...

# =================================================================
#                      BENCHMARK RESULTS REPORT                   
# =================================================================
# METRIC                         | BEFORE (Standard) | AFTER (NetMLFQ) 
# -----------------------------------------------------------------
# Interactive Latency (Ping RTT) | 185.40 ms       | 18.20 ms     
# Interactive Packet Loss        | 12.0 %          | 0.0 %        
# Heavy Background Throughput    | 48.50 Mbps      | 11.20 Mbps   
# -----------------------------------------------------------------
# [*] Interactive Latency Improvement: 90.2% reduction in delay.