#!/usr/bin/env python3
import time
import sys
import os

def get_stats():
    try:
        with open('/proc/netspeed', 'r') as f:
            lines = f.readlines()
        
        data = {}
        for line in lines:
            if ':' in line:
                key, val = line.split(':', 1)
                data[key.strip()] = val.strip()
        return data
    except FileNotFoundError:
        return None

def format_speed(b_sec):
    if b_sec > 1024 * 1024 * 1024:
        return f"{b_sec / (1024*1024*1024):.2f} GB/s"
    elif b_sec > 1024 * 1024:
        return f"{b_sec / (1024*1024):.2f} MB/s"
    elif b_sec > 1024:
        return f"{b_sec / 1024:.2f} KB/s"
    else:
        return f"{b_sec:.2f} B/s"

def main():
    if os.geteuid() != 0:
        print("[-] Please run with sudo.")
        sys.exit(1)

    initial = get_stats()
    if not initial:
        print("[-] Error: /proc/netspeed not found. Load kernel module first.")
        sys.exit(1)

    try:
        prev_rx = int(initial.get("TOTAL_RX", 0))
        prev_tx = int(initial.get("TOTAL_TX", 0))
    except Exception:
        prev_rx, prev_tx = 0, 0

    prev_time = time.time()

    try:
        while True:
            time.sleep(1.0)
            curr_time = time.time()
            elapsed = curr_time - prev_time
            if elapsed <= 0:
                elapsed = 1.0

            stats = get_stats()
            if not stats:
                continue

            try:
                curr_rx = int(stats.get("TOTAL_RX", 0))
                curr_tx = int(stats.get("TOTAL_TX", 0))
            except Exception:
                continue

            rx_speed = (curr_rx - prev_rx) / elapsed
            tx_speed = (curr_tx - prev_tx) / elapsed

            prev_rx = curr_rx
            prev_tx = curr_tx
            prev_time = curr_time

            rx_str = format_speed(rx_speed)
            tx_str = format_speed(tx_speed)

            rx_ips = stats.get("RX_IPS", "None")
            tx_ips = stats.get("TX_IPS", "None")

            # Truncate IP lists if they exceed available width to keep table neat
            if len(rx_ips) > 42:
                rx_ips = rx_ips[:39] + "..."
            if len(tx_ips) > 42:
                tx_ips = tx_ips[:39] + "..."

            os.system('cls' if os.name == 'nt' else 'clear')

            print("\033[1;36m┌──────────────────────────────────────────────────────────────────┐\033[0m")
            print("\033[1;36m│                   NETSPEED KERNEL HTOP MONITOR                   │\033[0m")
            print("\033[1;36m├──────────────────────────────────────────────────────────────────┤\033[0m")
            print(f"\033[1;32m│ Download Speed (RX) : {rx_str:<42} │\033[0m")
            print(f"\033[1;31m│ Upload Speed   (TX) : {tx_str:<42} │\033[0m")
            print("\033[1;36m├──────────────────────────────────────────────────────────────────┤\033[0m")
            print(f"│ Total RX Bytes      : {curr_rx:<42} │")
            print(f"│ Total TX Bytes      : {curr_tx:<42} │")
            print("\033[1;36m├──────────────────────────────────────────────────────────────────┤\033[0m")
            print(f"\033[1;33m│ Active Source IPs(RX): {rx_ips:<41} │\033[0m")
            print(f"\033[1;33m│ Active Dest IPs  (TX): {tx_ips:<41} │\033[0m")
            print("\033[1;36m└──────────────────────────────────────────────────────────────────┘\033[0m")
            print(" Press Ctrl+C to exit.")

    except KeyboardInterrupt:
        print("\nExiting dashboard.")

if __name__ == '__main__':
    main()