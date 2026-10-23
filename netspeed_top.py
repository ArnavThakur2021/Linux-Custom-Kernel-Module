#!/usr/bin/env python3
import time
import sys
import os
import platform

def send_control(cmd):
    try:
        with open('/proc/netspeed_ctl', 'w') as f:
            f.write(cmd)
    except Exception:
        pass

def get_kernel_stats():
    try:
        with open('/proc/netspeed', 'r') as f:
            return f.readlines()
    except FileNotFoundError:
        return []

def get_global_net_bytes():
    rx_total, tx_total = 0, 0
    try:
        with open('/proc/net/dev', 'r') as f:
            lines = f.readlines()
        for line in lines[2:]:
            if ':' not in line:
                continue
            iface, data = line.split(':', 1)
            iface = iface.strip()
            if iface == 'lo':
                continue
            fields = data.split()
            if len(fields) >= 9:
                rx_total += int(fields[0])
                tx_total += int(fields[8])
    except Exception:
        pass
    return rx_total, tx_total

def get_system_info():
    hostname = platform.node()
    kernel = platform.release()
    try:
        with open('/proc/loadavg', 'r') as f:
            loadavg = " ".join(f.read().split()[:3])
    except Exception:
        loadavg = "0.0 0.0 0.0"
    return hostname, kernel, loadavg

def format_speed(b_sec):
    if b_sec > 1024 * 1024 * 1024:
        return f"{b_sec / (1024*1024*1024):.2f} GB/s"
    elif b_sec > 1024 * 1024:
        return f"{b_sec / (1024*1024):.2f} MB/s"
    elif b_sec > 1024:
        return f"{b_sec / 1024:.2f} KB/s"
    else:
        return f"{b_sec:.2f} B/s"

def format_bytes(b):
    try:
        b = int(b)
    except ValueError:
        return str(b)
    if b > 1024 * 1024 * 1024:
        return f"{b / (1024*1024*1024):.2f} GB"
    elif b > 1024 * 1024:
        return f"{b / (1024*1024):.2f} MB"
    elif b > 1024:
        return f"{b / 1024:.2f} KB"
    else:
        return f"{b} B"

def main():
    if os.geteuid() != 0:
        print("[-] Please run with sudo.")
        sys.exit(1)

    prev_rx, prev_tx = get_global_net_bytes()
    prev_time = time.time()

    try:
        while True:
            time.sleep(1.0)
            curr_time = time.time()
            elapsed = curr_time - prev_time
            if elapsed <= 0:
                elapsed = 1.0

            curr_rx, curr_tx = get_global_net_bytes()
            rx_speed = (curr_rx - prev_rx) / elapsed
            tx_speed = (curr_tx - prev_tx) / elapsed

            prev_rx, prev_tx = curr_rx, curr_tx
            prev_time = curr_time

            hostname, kernel, loadavg = get_system_info()
            lines = get_kernel_stats()

            congestion = "IDLE"
            mode = "OFF"
            link = "5 MB"
            processes = []

            for line in lines:
                line = line.strip()
                if line.startswith("CONGESTION:"):
                    congestion = line.split(":")[1]
                elif line.startswith("MLFQ_MODE:"):
                    mode = line.split(":")[1]
                elif line.startswith("LINK_CAPACITY:"):
                    link = f"{int(line.split(':')[1]) // (1024*1024)} MB/s"
                elif line.startswith("PROC "):
                    parts = line.split()
                    if len(parts) >= 7:
                        processes.append({
                            "pid": parts[1],
                            "comm": parts[2][:10],
                            "queue": parts[3],
                            "total_bytes": parts[4],
                            "tokens": parts[5],
                            "drops": parts[6],
                            "status": parts[7] if len(parts) >= 8 else "AUTO"
                        })

            os.system('cls' if os.name == 'nt' else 'clear')

            print("\033[1;36m┌──────────────────────────────────────────────────────────────────┐\033[0m")
            print(f"\033[1;36m│          NETMLFQ SCHEDULER & GREEN ROUTING MONITOR              │\033[0m")
            print("\033[1;36m├──────────────────────────────────────────────────────────────────┤\033[0m")
            print(f"│ Host   : {hostname:<55} │")
            print(f"│ Kernel : {kernel:<55} │")
            print(f"│ Load   : {loadavg:<55} │")
            print(f"│ Mode   : {mode:<10} | Link Cap: {link:<15} | Cong: {congestion:<8} │")
            print("\033[1;36m├──────────────────────────────────────────────────────────────────┤\033[0m")
            print(f"\033[1;32m│ Download (RX) Speed : {format_speed(rx_speed):<43} │\033[0m")
            print(f"\033[1;31m│ Upload   (TX) Speed : {format_speed(tx_speed):<43} │\033[0m")
            print(f"│ Total RX Transferred: {format_bytes(curr_rx):<43} │")
            print(f"│ Total TX Transferred: {format_bytes(curr_tx):<43} │")
            print("\033[1;36m├──────────────────────────────────────────────────────────────────┤\033[0m")
            print(f"│ \033[1mPID     COMM       Q    DATA        TOKENS          DROPS  PINNED\033[0m │")
            print("\033[1;36m├──────────────────────────────────────────────────────────────────┤\033[0m")

            if not processes:
                print("│                    No active processes tracked                   │")
            else:
                for p in processes:
                    q_color = "\033[32m" if p["queue"] == "Q0" else ("\033[33m" if p["queue"] == "Q1" else "\033[31m")
                    tok_str = "Unlimited" if "184467" in p["tokens"] else format_bytes(p["tokens"])
                    data_str = format_bytes(p["total_bytes"])
                    pin_str = f"\033[35m{p['status']}\033[0m" if p['status'] == "PINNED" else p['status']
                    row_str = f" {p['pid']:<7} {p['comm']:<10} {q_color}{p['queue']}\033[0m  {data_str:<11} {tok_str:<15} {p['drops']:<6} {pin_str:<6}"
                    print(f"│ {row_str:<73} │")

            print("\033[1;36m└──────────────────────────────────────────────────────────────────┘\033[0m")
            print(" Control Options (Run in another terminal):")
            print("   echo 'mode eco'      | sudo tee /proc/netspeed_ctl  (Green Routing)")
            print("   echo 'mode mlfq'     | sudo tee /proc/netspeed_ctl  (Standard MLFQ)")
            print("   echo 'pin <PID> 0'   | sudo tee /proc/netspeed_ctl  (RuralLink Priority Pin)")

    except KeyboardInterrupt:
        send_control("mode off")
        print("\nExiting dashboard.")

if __name__ == '__main__':
    main()