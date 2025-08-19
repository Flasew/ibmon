#!/usr/bin/env python3
import argparse
import curses
import os
import signal
import time
from typing import Optional, Tuple, Deque
from collections import deque
import csv


SYSFS_IB_BASE = "/sys/class/infiniband"


class CounterPaths:
    def __init__(self, base: str, is_ib: bool):
        self.base = base
        self.is_ib = is_ib
        # Prefer canonical names; allow common variants as fallback.
        self.tx_data = first_existing(
            base,
            [
                "port_xmit_data",  # IB words (4 bytes)
                "tx_bytes",  # uncommon under this tree, but guard anyway
            ],
        )
        self.rx_data = first_existing(
            base,
            [
                "port_rcv_data",
                "rx_bytes",
            ],
        )
        self.tx_pkts = first_existing(
            base,
            [
                "port_xmit_packets",
                "port_xmit_pkts",
                "tx_packets",
            ],
        )
        self.rx_pkts = first_existing(
            base,
            [
                "port_rcv_packets",
                "port_rcv_pkts",
                "rx_packets",
            ],
        )


def first_existing(base: str, names: list) -> Optional[str]:
    for n in names:
        p = os.path.join(base, n)
        if os.path.exists(p):
            return p
    return None


def read_uint_from_file(path: str) -> Optional[int]:
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            s = f.read().strip().split()[0]
            return int(s)
    except Exception:
        return None


def read_str(path: str) -> Optional[str]:
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            return f.read().strip()
    except Exception:
        return None


def get_link_info(dev: str, port: int) -> Tuple[Optional[str], Optional[str]]:
    port_base = os.path.join(SYSFS_IB_BASE, dev, "ports", str(port))
    return (
        read_str(os.path.join(port_base, "link_layer")),
        read_str(os.path.join(port_base, "rate")),
    )


def parse_rate_gbps(rate_str: Optional[str]) -> Optional[float]:
    # Example: "100 Gb/sec (4X EDR)" -> 100.0
    if not rate_str:
        return None
    try:
        token = rate_str.split()[0]
        return float(token)
    except Exception:
        return None


def human_rate(v: float, mode: str) -> str:
    # mode: 'bits' or 'bytes'
    units = [" ", "K", "M", "G", "T", "P"]
    base = 1000.0
    suffix = "b/s" if mode == "bits" else "B/s"
    val = v
    if mode == "bits":
        val *= 8.0
    i = 0
    while abs(val) >= base and i < len(units) - 1:
        val /= base
        i += 1
    return f"{val:6.2f} {units[i]}{suffix}"


def human_pps(v: float) -> str:
    units = [" ", "K", "M", "G", "T"]
    base = 1000.0
    val = v
    i = 0
    while abs(val) >= base and i < len(units) - 1:
        val /= base
        i += 1
    return f"{val:6.2f} {units[i]}pps"


def resolve_counters(dev: str, port: int) -> Tuple[CounterPaths, Optional[str], Optional[str]]:
    port_base = os.path.join(SYSFS_IB_BASE, dev, "ports", str(port))
    counters_base = os.path.join(port_base, "counters")
    if not os.path.isdir(counters_base):
        raise FileNotFoundError(
            f"Counters path not found: {counters_base}. Is the device/port correct?"
        )
    link_layer, rate = get_link_info(dev, port)
    is_ib = (link_layer or "").lower().startswith("infiniband")
    paths = CounterPaths(counters_base, is_ib)
    # At minimum we need data and packet counters
    if not (paths.tx_data and paths.rx_data and paths.tx_pkts and paths.rx_pkts):
        missing = []
        if not paths.tx_data:
            missing.append("tx_data")
        if not paths.rx_data:
            missing.append("rx_data")
        if not paths.tx_pkts:
            missing.append("tx_pkts")
        if not paths.rx_pkts:
            missing.append("rx_pkts")
        raise FileNotFoundError(
            f"Missing expected counter files under {counters_base}: {', '.join(missing)}"
        )
    return paths, link_layer, rate


def read_counters(paths: CounterPaths) -> Tuple[int, int, int, int]:
    tx_data = read_uint_from_file(paths.tx_data)
    rx_data = read_uint_from_file(paths.rx_data)
    tx_pkts = read_uint_from_file(paths.tx_pkts)
    rx_pkts = read_uint_from_file(paths.rx_pkts)
    if None in (tx_data, rx_data, tx_pkts, rx_pkts):
        raise RuntimeError("Failed to read one or more counters")
    return int(tx_data), int(rx_data), int(tx_pkts), int(rx_pkts)


def diff_counters(prev: Tuple[int, int, int, int], cur: Tuple[int, int, int, int]) -> Tuple[int, int, int, int]:
    diffs = []
    for p, c in zip(prev, cur):
        d = c - p
        if d < 0:
            # handle wrap assuming 64-bit counter
            d = (c + (1 << 64)) - p
        diffs.append(d)
    return tuple(diffs)  # type: ignore


def draw(screen, args):
    curses.use_default_colors()
    screen.nodelay(True)
    screen.timeout(0)

    try:
        paths, link_layer, rate = resolve_counters(args.device, args.port)
    except Exception as e:
        screen.clear()
        screen.addstr(0, 0, f"Error: {e}")
        screen.addstr(2, 0, "Press any key to exit.")
        screen.refresh()
        screen.getch()
        return

    is_ib = paths.is_ib
    rate_gbps = parse_rate_gbps(rate)

    # Prime counters
    prev = read_counters(paths)
    prev_t = time.perf_counter()

    paused = False
    # CSV setup
    csv_file = None
    csv_writer = None
    if args.csv:
        mode = 'a' if args.csv_append else 'w'
        try:
            csv_file = open(args.csv, mode, newline='')
            csv_writer = csv.writer(csv_file)
            if (not args.csv_append) or args.csv_headers:
                csv_writer.writerow(["time_s", "rx_Bps", "tx_Bps", "rx_pps", "tx_pps"])  # bytes/sec
                csv_file.flush()
        except Exception:
            csv_file = None
            csv_writer = None

    # history for plotting (store recent samples)
    rx_hist: Deque[float] = deque(maxlen=4096)
    tx_hist: Deque[float] = deque(maxlen=4096)

    while True:
        start_loop = time.perf_counter()

        # Input handling
        ch = screen.getch()
        if ch != -1:
            if ch in (ord("q"), ord("Q")):
                break
            elif ch in (ord("p"), ord("P")):
                paused = not paused
            elif ch in (ord("u"), ord("U")):
                args.units = "bytes" if args.units == "bits" else "bits"

        if not paused:
            cur = read_counters(paths)
            now = time.perf_counter()
            dt = max(1e-9, now - prev_t)
            d_txB, d_rxB, d_txp, d_rxp = diff_counters(prev, cur)

            # IB data counters are in 4-byte words; convert to bytes if IB
            if is_ib:
                d_txB *= 4
                d_rxB *= 4

            tx_Bps = d_txB / dt
            rx_Bps = d_rxB / dt
            tx_pps = d_txp / dt
            rx_pps = d_rxp / dt

            prev = cur
            prev_t = now

            # update history
            rx_hist.append(rx_Bps)
            tx_hist.append(tx_Bps)

            # CSV log (bytes/sec)
            if csv_writer is not None:
                csv_writer.writerow([f"{now:.6f}", f"{rx_Bps:.0f}", f"{tx_Bps:.0f}", f"{rx_pps:.0f}", f"{tx_pps:.0f}"])
                try:
                    csv_file.flush()
                except Exception:
                    pass

        # Rendering
        screen.erase()
        title = f"InfiniBand Bandwidth Monitor — {args.device} port {args.port}  [q:quit p:pause u:units]"
        screen.addstr(0, 0, title)
        screen.addstr(1, 0, f"Interval: {args.interval*1000:.0f} ms   Units: {args.units}")
        if link_layer:
            screen.addstr(2, 0, f"Link: {link_layer}")
        if rate:
            screen.addstr(2, 20, f"Rate: {rate}")

        row = 4
        screen.addstr(row, 0, "Direction     Data Rate            Packets/s")
        row += 1

        if paused:
            screen.addstr(row, 0, "PAUSED")
        else:
            screen.addstr(row + 0, 0, f"RX           {human_rate(rx_Bps, args.units):>18}    {human_pps(rx_pps):>12}")
            screen.addstr(row + 1, 0, f"TX           {human_rate(tx_Bps, args.units):>18}    {human_pps(tx_pps):>12}")

            if rate_gbps is not None:
                # Utilization using bits/s
                rx_util = min(100.0, (rx_Bps * 8.0) / (rate_gbps * 1e9) * 100.0)
                tx_util = min(100.0, (tx_Bps * 8.0) / (rate_gbps * 1e9) * 100.0)
                screen.addstr(row + 2, 0, f"Utilization  RX: {rx_util:6.2f}%   TX: {tx_util:6.2f}%")

        # Draw bmon-like bar graph
        maxy, maxx = screen.getmaxyx()
        chart_top = row + 4
        chart_height = maxy - chart_top - 1
        y_label_w = 10
        chart_width = maxx - 2 - y_label_w
        if chart_height > 3 and chart_width > 10 and len(rx_hist) > 1:
            samples = min(chart_width, len(rx_hist))
            def scaled(v):
                return v * 8.0 if args.units == "bits" else v
            maxv = 1.0
            rx_list = list(rx_hist)
            tx_list = list(tx_hist)
            for i in range(samples):
                r = scaled(rx_list[-samples + i])
                t = scaled(tx_list[-samples + i])
                if r > maxv:
                    maxv = r
                if t > maxv:
                    maxv = t
            rate_gbps = parse_rate_gbps(rate)
            if args.units == "bits" and rate_gbps is not None and rate_gbps > 0:
                link_bps = rate_gbps * 1e9
                if link_bps < maxv:
                    maxv = link_bps

            # Y labels top/mid/bottom
            top_label = f"{maxv/1e9:5.1f} {'Gb/s' if args.units=='bits' else 'GB/s'}"
            mid_label = f"{(maxv/2)/1e9:5.1f} {'Gb/s' if args.units=='bits' else 'GB/s'}"
            bot_label = f"{0.0:5.1f} {'Gb/s' if args.units=='bits' else 'GB/s'}"
            try:
                screen.addstr(chart_top, 0, f"{top_label:>{y_label_w-2}} |")
                screen.addstr(chart_top + chart_height//2, 0, f"{mid_label:>{y_label_w-2}} |")
                screen.addstr(chart_top + chart_height - 1, 0, f"{bot_label:>{y_label_w-2}} |")
            except curses.error:
                pass

            # Bars
            for x in range(samples):
                r = scaled(rx_list[-samples + x])
                t = scaled(tx_list[-samples + x])
                rh = int(round((r / maxv) * chart_height))
                th = int(round((t / maxv) * chart_height))
                rh = max(0, min(chart_height, rh))
                th = max(0, min(chart_height, th))
                col = y_label_w + 1 + x
                for yy in range(chart_height):
                    y = chart_top + (chart_height - 1 - yy)
                    ch = ord(' ')
                    rx_on = yy < rh
                    tx_on = yy < th
                    if rx_on and tx_on:
                        ch = ord('#')
                    elif rx_on:
                        ch = ord('*')
                    elif tx_on:
                        ch = ord('+')
                    try:
                        screen.addch(y, col, ch)
                    except curses.error:
                        pass

            # baseline and legend
            try:
                for x in range(samples):
                    screen.addch(chart_top + chart_height, y_label_w + 1 + x, ord('-'))
                screen.addstr(chart_top + chart_height, maxx - 22, "*RX  +TX  #Both")
            except curses.error:
                pass

        screen.refresh()

        # Sleep remaining time to maintain interval
        elapsed = time.perf_counter() - start_loop
        to_sleep = args.interval - elapsed
        if to_sleep > 0:
            time.sleep(to_sleep)


def main():
    parser = argparse.ArgumentParser(
        description="TUI monitor for InfiniBand bandwidth and packets via sysfs counters",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("-d", "--device", required=True, help="InfiniBand device, e.g. mlx5_0")
    parser.add_argument("-p", "--port", type=int, default=1, help="Port number")
    parser.add_argument(
        "-i",
        "--interval",
        type=float,
        default=0.2,
        help="Refresh interval in seconds (supports sub-second)",
    )
    parser.add_argument(
        "-u",
        "--units",
        choices=["bits", "bytes"],
        default="bits",
        help="Display bandwidth as bits/s or bytes/s",
    )
    parser.add_argument("--csv", help="CSV output path (logs bytes/sec and pps)")
    parser.add_argument("--csv-append", action="store_true", help="Append to CSV if exists")
    parser.add_argument("--csv-headers", action="store_true", help="Write CSV header row")
    args = parser.parse_args()

    if args.interval <= 0:
        parser.error("--interval must be > 0")

    # Make Ctrl-C cleanly exit curses
    signal.signal(signal.SIGINT, signal.SIG_DFL)

    curses.wrapper(draw, args)


if __name__ == "__main__":
    main()
