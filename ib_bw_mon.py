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
    has_colors = curses.has_colors()
    if has_colors:
        curses.start_color()
        try:
            curses.use_default_colors()
        except Exception:
            pass
        curses.init_pair(1, curses.COLOR_CYAN, -1)      # RX bars
        curses.init_pair(2, curses.COLOR_RED, -1)       # TX bars
        curses.init_pair(10, curses.COLOR_WHITE, curses.COLOR_BLACK)  # light text
        curses.init_pair(11, curses.COLOR_BLACK, curses.COLOR_BLACK)  # panel bg
        curses.init_pair(12, curses.COLOR_BLACK, curses.COLOR_BLACK)  # header bg
        curses.init_pair(13, curses.COLOR_WHITE, curses.COLOR_BLACK)  # border

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

    # windows
    win_hdr = None
    win_rx = None
    win_tx = None

    start_time = time.perf_counter()

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

        # Layout windows
        maxy, maxx = screen.getmaxyx()
        hdr_h = 4
        remaining = maxy - hdr_h
        if remaining < 6:
            remaining = 6
        rx_h = remaining // 2
        tx_h = remaining - rx_h

        if win_hdr is None or win_rx is None or win_tx is None:
            win_hdr = curses.newwin(hdr_h, maxx, 0, 0)
            win_rx = curses.newwin(rx_h, maxx, hdr_h, 0)
            win_tx = curses.newwin(tx_h, maxx, hdr_h + rx_h, 0)
        else:
            # Recreate if size changed
            ch, cw = win_hdr.getmaxyx()
            if ch != hdr_h or cw != maxx:
                win_hdr = curses.newwin(hdr_h, maxx, 0, 0)
            ch, cw = win_rx.getmaxyx()
            if ch != rx_h or cw != maxx:
                win_rx = curses.newwin(rx_h, maxx, hdr_h, 0)
            ch, cw = win_tx.getmaxyx()
            if ch != tx_h or cw != maxx:
                win_tx = curses.newwin(tx_h, maxx, hdr_h + rx_h, 0)

        # Header
        win_hdr.erase()
        try:
            win_hdr.bkgd(' ', curses.color_pair(12))
            win_hdr.attron(curses.color_pair(13))
            win_hdr.box()
            win_hdr.attroff(curses.color_pair(13))
        except curses.error:
            pass
        try:
            win_hdr.attron(curses.color_pair(10))
            win_hdr.addstr(0, 2, " InfiniBand Bandwidth Monitor ")
            win_hdr.addstr(1, 2, f"{args.device} port {args.port}  [q:quit p:pause u:units]")
            win_hdr.addstr(2, 2, f"Interval: {args.interval*1000:.0f} ms   Units: {args.units}")
            if link_layer:
                win_hdr.addstr(1, maxx//2, f"Link: {link_layer}")
            if rate:
                win_hdr.addstr(2, maxx//2, f"Rate: {rate}")
            if paused:
                win_hdr.addstr(1, maxx-12, "[PAUSED]")
            win_hdr.attroff(curses.color_pair(10))
        except curses.error:
            pass
        win_hdr.noutrefresh()

        def draw_panel(win, is_rx: bool, cur_Bps: float, cur_pps: float, hist_deque: Deque[float]):
            wy, wx = win.getmaxyx()
            try:
                win.erase()
                if has_colors:
                    win.bkgd(' ', curses.color_pair(11))
                    win.attron(curses.color_pair(13))
                win.box()
                if has_colors:
                    win.attroff(curses.color_pair(13))
            except curses.error:
                pass
            # title and current values
            try:
                if has_colors:
                    win.attron(curses.color_pair(10))
                win.addstr(0, 2, f" {'RX' if is_rx else 'TX'}  {human_rate(cur_Bps, args.units)}  {human_pps(cur_pps)} ")
            except curses.error:
                pass
            y_label_w = 12
            chart_h = wy - 3
            chart_w = wx - 2 - y_label_w
            if chart_h < 3 or chart_w < 10 or len(hist_deque) < 2:
                win.noutrefresh()
                return
            samples = min(chart_w, len(hist_deque))
            data_list = list(hist_deque)
            def scaled(v):
                return v * 8.0 if args.units == 'bits' else v
            maxv = 1.0
            for i in range(samples):
                v = scaled(data_list[-samples + i])
                if v > maxv:
                    maxv = v
            rate_gbps = parse_rate_gbps(rate)
            if args.units == 'bits' and rate_gbps:
                link_bps = rate_gbps * 1e9
                if link_bps > 0 and link_bps < maxv:
                    maxv = link_bps
            top_label = f"{maxv/1e9:6.2f} {'Gb/s' if args.units=='bits' else 'GB/s'}"
            mid_label = f"{(maxv/2)/1e9:6.2f} {'Gb/s' if args.units=='bits' else 'GB/s'}"
            try:
                win.addstr(1, 1, f"{top_label:>{y_label_w-3}} |")
                win.addstr(1 + chart_h//2, 1, f"{mid_label:>{y_label_w-3}} |")
                win.addstr(1 + chart_h - 1, 1, f"{'0.00 ':>{y_label_w-3}} |")
            except curses.error:
                pass
            for x in range(samples):
                v = scaled(data_list[-samples + x])
                h = int(round((v / maxv) * chart_h))
                h = max(0, min(chart_h, h))
                col = y_label_w + 1 + x
                for yy in range(chart_h):
                    y = 1 + (chart_h - 1 - yy)
                    if yy < h:
                        try:
                            if has_colors:
                                win.attron(curses.color_pair(1 if is_rx else 2))
                            win.addch(y, col, curses.ACS_CKBOARD)
                            if has_colors:
                                win.attroff(curses.color_pair(1 if is_rx else 2))
                        except curses.error:
                            pass
            try:
                for x in range(samples):
                    win.addch(1 + chart_h, y_label_w + 1 + x, ord('-'))
            except curses.error:
                pass
            if has_colors:
                try:
                    win.attroff(curses.color_pair(10))
                except curses.error:
                    pass
            win.noutrefresh()

        # Draw panels
        draw_panel(win_rx, True, rx_Bps, rx_pps, rx_hist)
        draw_panel(win_tx, False, tx_Bps, tx_pps, tx_hist)

        curses.doupdate()

        # Sleep remaining time to maintain interval
        elapsed = time.perf_counter() - start_loop
        to_sleep = args.interval - elapsed
        if to_sleep > 0:
            time.sleep(to_sleep)

        if args.duration and args.duration > 0 and (time.perf_counter() - start_time) >= args.duration:
            break


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
        default=1.0,
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
    parser.add_argument("--duration", type=float, default=0.0, help="Auto-exit after N seconds (0 = infinite)")
    args = parser.parse_args()

    if args.interval <= 0:
        parser.error("--interval must be > 0")

    # Make Ctrl-C cleanly exit curses
    signal.signal(signal.SIGINT, signal.SIG_DFL)

    curses.wrapper(draw, args)


if __name__ == "__main__":
    main()
