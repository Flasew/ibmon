# ib_bw_mon

Lightweight TUI tools (C and Python) to monitor InfiniBand bandwidth and packet rates via sysfs.

## Features

- Monitors RX/TX instantaneous bandwidth and packets/s per port.
- Uses `/sys/class/infiniband/<device>/ports/<port>/counters`.
- Auto-detects InfiniBand link layer and converts `port_*_data` (4-byte words) to bytes.
- Shows link rate (e.g., `100 Gb/sec`) and utilization if available.
- Fast refresh intervals (sub-second) with minimal overhead.
- Toggle display units between bits/s and bytes/s.

## Requirements

- For C binary: a C compiler and `ncurses` development libraries.
- For Python script: Python 3 (standard library only).
- Access to InfiniBand sysfs counters.

## Build (C)

```
make
```

If your system requires wide curses, use: `make LIBS=-lncursesw`.

## Usage (C)

```
./ib_bw_mon -d mlx5_0 [-p 1] [-i 1] [--units bits|bytes] [--bg black|terminal] [--csv file.csv] [--csv-append] [--csv-headers] [--duration 2]
```

## Usage (Python)

```
python3 ib_bw_mon.py -d mlx5_0 [-p 1] [-i 1] [--units bits|bytes] [--csv file.csv] [--csv-append] [--csv-headers] [--duration 2]
```

Arguments:

- `-d, --device`: InfiniBand device (e.g., `mlx5_0`) [required]
- `-p, --port`: Port number (default: 1)
- `-i, --interval`: Refresh interval in seconds (default: 0.2)
- `-u, --units`: Show bandwidth in `bits` or `bytes` per second (default: `bits`)

Keys in TUI:

- `q`: quit
- `p`: pause/resume sampling
- `u`: toggle units between bits/s and bytes/s
 - `--bg`: choose `terminal` to use your terminal’s background or `black` (default: terminal on some terminals may look better)
 - `--duration N`: auto-exit after N seconds (useful for quick tests)

## Notes

- On InfiniBand, `port_xmit_data`/`port_rcv_data` are in 4-byte words; the tools convert to bytes automatically when `link_layer` is `InfiniBand`.
- Counters are assumed to be 64-bit and wrap-around is handled.
- If expected counter files are missing, the tool reports which ones are absent.
- CSV logs are bytes/sec for bandwidth (consistent across units) and packets/sec.
 - TUI layout: header + two colored panels (RX on blue, TX on magenta) with per-panel history graphs.
