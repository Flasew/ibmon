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
        self.data_is_words = False
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
        # Determine if data counters are 4-byte words
        if self.tx_data and os.path.basename(self.tx_data) == "port_xmit_data":
            self.data_is_words = True
        if self.rx_data and os.path.basename(self.rx_data) == "port_rcv_data":
            self.data_is_words = True


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
    # Setup terminal modes for immediate, non-echoed key handling
    try:
        curses.raw()
        curses.noecho()
    except Exception:
        pass
    curses.use_default_colors()
    screen.nodelay(True)
    # Small blocking timeout improves key capture reliability on some terminals
    # Make getch non-blocking; we manage sleeping ourselves to allow instant redraw on toggles
    try:
        screen.timeout(0)
    except Exception:
        pass
    try:
        screen.keypad(True)
    except Exception:
        pass
    has_colors = curses.has_colors()
    if has_colors:
        curses.start_color()
        try:
            curses.use_default_colors()
        except Exception:
            pass
        curses.init_pair(1, curses.COLOR_CYAN, curses.COLOR_BLACK)  # RX bars (bg matches panel)
        curses.init_pair(2, curses.COLOR_RED, curses.COLOR_BLACK)   # TX bars (bg matches panel)
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
    data_mode = False
    info_mode = False
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
    first_draw = True
    # Start with immediate getch to render first frame instantly; switch to interval timeout after first draw
    screen.timeout(0)
    timeout_ms = max(10, int(args.interval * 1000))

    while True:
        start_loop = time.perf_counter()
        fast_switch = False

        # Input handling (drain input buffer for reliability)
        while True:
            ch = screen.getch()
            if ch == -1:
                break
            if ch in (ord('q'), ord('Q')):
                return
            elif ch in (ord('p'), ord('P')):
                paused = not paused
                fast_switch = True
                break
            elif ch in (ord('u'), ord('U')):
                args.units = 'bytes' if args.units == 'bits' else 'bits'
                fast_switch = True
                break
            elif ch in (ord('d'), ord('D')):
                data_mode = not data_mode
                if data_mode:
                    info_mode = False
                fast_switch = True
                break
            elif ch in (ord('i'), ord('I')):
                info_mode = not info_mode
                if info_mode:
                    data_mode = False
                fast_switch = True
                break

        if not paused and not fast_switch:
            try:
                cur = read_counters(paths)
                now = time.perf_counter()
                dt = max(1e-9, now - prev_t)
                d_txB, d_rxB, d_txp, d_rxp = diff_counters(prev, cur)

                if is_ib:
                    d_txB *= 4
                    d_rxB *= 4

                tx_Bps = d_txB / dt
                rx_Bps = d_rxB / dt
                tx_pps = d_txp / dt
                rx_pps = d_rxp / dt

                prev = cur
                prev_t = now
            except Exception:
                now = time.perf_counter()
                # keep previous values if read fails
                pass

            # always append to history so the graph scrolls
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
            # Current time top-right: MMMM-YY-DD HH:MM:SS
            now_str = time.strftime("%B-%y-%d %H:%M:%S", time.localtime())
            try:
                win_hdr.addstr(0, maxx - len(now_str) - 2, now_str)
            except curses.error:
                pass
            win_hdr.addstr(1, 2, f"{args.device} port {args.port}  [q:quit p:pause u:units]")
            view = 'DATA' if data_mode else ('INFO' if info_mode else 'PLOT')
            win_hdr.addstr(2, 2, f"Interval: {args.interval*1000:.0f} ms   Units: {args.units}   View: {view}")
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
                    win.attrset(curses.color_pair(10))
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
            # dynamic labels with appropriate units and right alignment
            def fmt_label(val):
                v = val
                if args.units == 'bits':
                    suffix = ['b/s','Kb/s','Mb/s','Gb/s','Tb/s','Pb/s']
                else:
                    suffix = ['B/s','KB/s','MB/s','GB/s','TB/s','PB/s']
                idx = 0
                while v >= 1000.0 and idx < len(suffix)-1:
                    v /= 1000.0
                    idx += 1
                return f"{v:6.2f} {suffix[idx]}"
            top_label = fmt_label(maxv)
            mid_label = fmt_label(maxv/2)
            bot_label = '0.00 b/s' if args.units == 'bits' else '0.00 B/s'
            lblw = max(len(top_label), len(mid_label), len(bot_label))
            y_label_w = lblw + 3
            chart_w = wx - 2 - y_label_w
            if chart_w < 1:
                chart_w = 1
            try:
                win.addstr(1, 1, f"{top_label:>{y_label_w-3}} |")
                win.addstr(1 + chart_h//2, 1, f"{mid_label:>{y_label_w-3}} |")
                win.addstr(1 + chart_h - 1, 1, f"{bot_label:>{y_label_w-3}} |")
            except curses.error:
                pass
            # right-aligned columns: newest at far right
            base_col = y_label_w + 1 + (chart_w - samples)
            # fill dots
            for i in range(samples):
                col = base_col + i
                for yy in range(chart_h):
                    y = 1 + (chart_h - 1 - yy)
                    try:
                        if has_colors:
                            win.attrset(curses.color_pair(1 if is_rx else 2))
                        win.addch(y, col, ord('.'))
                    except curses.error:
                        pass
            # draw bars
            for i in range(samples):
                v = scaled(data_list[-samples + i])
                h = int(round((v / maxv) * chart_h))
                h = max(0, min(chart_h, h))
                col = base_col + i
                for yy in range(h):
                    y = 1 + (chart_h - 1 - yy)
                    try:
                        if has_colors:
                            win.attrset(curses.color_pair(1 if is_rx else 2))
                        win.addch(y, col, ord('|'))
                    except curses.error:
                        pass
            # no x-axis
            if has_colors:
                try:
                    win.attroff(curses.color_pair(10))
                except curses.error:
                    pass
            win.noutrefresh()

        # Draw panels
        if not data_mode and not info_mode:
            draw_panel(win_rx, True, rx_Bps, rx_pps, rx_hist)
            draw_panel(win_tx, False, tx_Bps, tx_pps, tx_hist)
        elif data_mode:
            # Raw counters view
            win_rx.erase()
            try:
                if has_colors:
                    win_rx.bkgd(' ', curses.color_pair(11))
                    win_rx.attron(curses.color_pair(13))
                win_rx.box()
                if has_colors:
                    win_rx.attroff(curses.color_pair(13))
                    win_rx.attron(curses.color_pair(10))
                win_rx.addstr(0, 2, " RX - Raw Counters ")
                def read_u64(p):
                    try:
                        with open(p, 'r') as f:
                            return int(f.read().split()[0])
                    except Exception:
                        return None
                base = f"/sys/class/infiniband/{args.device}/ports/{args.port}/counters"
                lines = []
                val = read_u64(f"{base}/port_rcv_data");   lines.append(("port_rcv_data", val))
                val = read_u64(f"{base}/port_rcv_packets");lines.append(("port_rcv_packets", val))
                val = read_u64(f"{base}/port_rcv_errors"); lines.append(("port_rcv_errors", val))
                val = read_u64(f"{base}/port_rcv_remote_physical_errors"); lines.append(("rcv_remote_phy", val))
                val = read_u64(f"{base}/port_rcv_switch_relay_errors");  lines.append(("rcv_switch_relay", val))
                for idx,(k,v) in enumerate(lines):
                    if v is not None:
                        win_rx.addstr(1+idx, 2, f"{k:20s} {v:20d}")
                if has_colors:
                    win_rx.attroff(curses.color_pair(10))
            except curses.error:
                pass
            win_rx.noutrefresh()

            win_tx.erase()
            try:
                if has_colors:
                    win_tx.bkgd(' ', curses.color_pair(11))
                    win_tx.attron(curses.color_pair(13))
                win_tx.box()
                if has_colors:
                    win_tx.attroff(curses.color_pair(13))
                    win_tx.attron(curses.color_pair(10))
                win_tx.addstr(0, 2, " TX - Raw Counters ")
                base = f"/sys/class/infiniband/{args.device}/ports/{args.port}/counters"
                lines = []
                val = read_u64(f"{base}/port_xmit_data");    lines.append(("port_xmit_data", val))
                val = read_u64(f"{base}/port_xmit_packets"); lines.append(("port_xmit_packets", val))
                val = read_u64(f"{base}/port_xmit_discards");lines.append(("xmit_discards", val))
                val = read_u64(f"{base}/port_xmit_wait");    lines.append(("xmit_wait", val))
                for idx,(k,v) in enumerate(lines):
                    if v is not None:
                        win_tx.addstr(1+idx, 2, f"{k:20s} {v:20d}")
                if has_colors:
                    win_tx.attroff(curses.color_pair(10))
            except curses.error:
                pass
            win_tx.noutrefresh()
        else:
            # Info (GIDs) view in RX pane; clear TX pane
            win_rx.erase()
            try:
                if has_colors:
                    win_rx.bkgd(' ', curses.color_pair(11))
                    win_rx.attron(curses.color_pair(13))
                win_rx.box()
                if has_colors:
                    win_rx.attroff(curses.color_pair(13))
                    win_rx.attron(curses.color_pair(10))
                win_rx.addstr(0, 2, " GID Table ")
                win_rx.addstr(1, 2, "Idx  Type        Ndev              GID")
                base = f"/sys/class/infiniband/{args.device}/ports/{args.port}"
                row = 2
                def read_text(p):
                    try:
                        with open(p, 'r') as f:
                            return f.read().strip()
                    except Exception:
                        return None
                for i in range(256):
                    gid = read_text(f"{base}/gids/{i}")
                    if not gid or gid.replace(':','') == '0'*32:
                        continue
                    gtype = read_text(f"{base}/gid_attrs/types/{i}") or ''
                    ndev = read_text(f"{base}/gid_attrs/ndevs/{i}") or ''
                    win_rx.addstr(row, 2, f"{i:3d}  {gtype:10s}  {ndev:16s}  {gid}")
                    row += 1
                    if row >= win_rx.getmaxyx()[0]-1:
                        break
                if has_colors:
                    win_rx.attroff(curses.color_pair(10))
            except curses.error:
                pass
            win_rx.noutrefresh()

            try:
                win_tx.erase()
                if has_colors:
                    win_tx.bkgd(' ', curses.color_pair(11))
                win_tx.noutrefresh()
            except curses.error:
                pass

        curses.doupdate()
        if first_draw:
            first_draw = False
            screen.timeout(timeout_ms)

        # Sleep remaining time to maintain interval
        # No explicit sleep; getch() blocks up to timeout_ms or returns immediately on keypress

        if args.duration and args.duration > 0 and (time.perf_counter() - start_time) >= args.duration:
            break


def main():
    parser = argparse.ArgumentParser(
        description="TUI monitor for InfiniBand bandwidth and packets via sysfs counters",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("-d", "--device", help="InfiniBand device, e.g. mlx5_0. If omitted, picks first ACTIVE device.")
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

    # If device not specified, pick first ACTIVE device on port 1
    if not args.device:
        base = "/sys/class/infiniband"
        try:
            for d in sorted(os.listdir(base)):
                state_path = os.path.join(base, d, "ports", "1", "state")
                try:
                    with open(state_path, "r") as f:
                        if "ACTIVE" in f.read():
                            args.device = d
                            break
                except Exception:
                    continue
        except Exception:
            pass
        if not args.device:
            parser.error("No ACTIVE InfiniBand devices found and no --device specified")

    # Make Ctrl-C cleanly exit curses
    signal.signal(signal.SIGINT, signal.SIG_DFL)

    curses.wrapper(draw, args)


if __name__ == "__main__":
    main()
