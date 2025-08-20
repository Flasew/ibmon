# ibmon

InfiniBand bandwidth and packet monitor with a fast, curses-based TUI.

Two implementations are provided:
- `ib_bw_mon` (C, ncurses): high-performance TUI with plotting, raw counters, info pages, and multi-device grid.
- `ib_bw_mon.py` (Python, curses): lightweight TUI with plotting, CSV logging, and data/info pages for a single device.

## Features

- Monitors RX/TX instantaneous bandwidth and packets/s per port.
- Uses `/sys/class/infiniband/<device>/ports/<port>/counters`.
- Auto-detects InfiniBand link layer and converts `port_*_data` (4-byte words) to bytes.
- Shows link rate (e.g., `100 Gb/sec`) and utilization if available.
- Fast refresh intervals (sub-second) with minimal overhead.
- Toggle display units between bits/s and bytes/s.

## Requirements

- C: a C compiler and `ncurses` development headers.
- Python: Python 3 (standard library only).
- Access to InfiniBand sysfs counters.

## Build (C)

```
make
```

If your system requires wide curses, use: `make LIBS=-lncursesw`.

## Usage (C)

```
./ib_bw_mon [-d DEV[,DEV...]] [-p 1] [-i 1] [--units bits|bytes] [--bg black|terminal] [--csv out.csv] [--csv-append] [--csv-headers] [--duration 2]
```

## Usage (Python)

```
python3 ib_bw_mon.py [-d mlx5_0] [-p 1] [-i 1] [--units bits|bytes] [--csv out.csv] [--csv-append] [--csv-headers] [--duration 2]
```

Arguments:

- `-d, --device`: InfiniBand device (e.g., `mlx5_0`). If omitted, C monitors all ACTIVE devices; Python picks the first ACTIVE device.
- `-p, --port`: Port number (default: 1)
- `-i, --interval`: Refresh interval in seconds (default: 0.2)
- `-u, --units`: Show bandwidth in `bits` or `bytes` per second (default: `bits`)

## Controls

- `q`: quit
- `p`: pause/resume sampling
- `u`: toggle units between bits/s and bytes/s
- `d`: toggle Data page showing raw counters (plot keeps updating)
- `i`: toggle Info page showing GIDs and attributes
- `--bg`: choose `terminal` to use your terminal’s background or `black`
- `--duration N`: auto-exit after N seconds (useful for quick tests)

## Features

- Plot view (C and Python):
  - Right-anchored history graphs for RX/TX bandwidth.
  - Dots (empty) and bars (occupied) bmon-like style.
  - Y-axis scales auto-adjust with appropriate units (b/s, Kb/s, Mb/s, … or B/s, KB/s, …).
  - Header shows date time (e.g., `August-19-2025 14:05:33`) and link info.
- Data view (`d`):
  - RX: `port_rcv_data` (words), `port_rcv_packets`, `port_rcv_errors`, `port_rcv_remote_physical_errors`, `port_rcv_switch_relay_errors`.
  - TX: `port_xmit_data` (words), `port_xmit_packets`, `port_xmit_discards`, `port_xmit_wait`.
  - Other (C only): `port_local_phy_errors`, `symbol_error(s)`, `link_error_recovery`, `link_downed`, `vl15_dropped`, `excessive_buffer_overrun_errors`.
  - Plotting continues to update while viewing Data.
- Info view (`i`):
  - Lists non-zero GIDs and their Type and Ndev from `/sys/class/infiniband/<dev>/ports/<port>/`.
  - Refreshes approximately once per second.
- CSV logging (both): logs bytes/sec and packets/sec with timestamps.

## Details and Notes

- InfiniBand data counters (`port_*_data`) are octets/4 (4-byte words). The tools multiply by 4 for bytes conversions prior to rate calculation.
- 64-bit counter wrap-around is handled.
- Background and colors (C): `--bg black` forces black; `--bg terminal` blends with your terminal theme.
- Immediate redraws: both tools redraw immediately on `d`/`i`; C also renders the first frame immediately on startup. C’s multi-device grid avoids full-screen erases to reduce flicker.

## Examples

- Plot with terminal background (2s run):
  - `./ib_bw_mon -d mlx5_0 --bg terminal --duration 2`
- CSV logging:
  - `./ib_bw_mon -d mlx5_0 --csv out.csv`
