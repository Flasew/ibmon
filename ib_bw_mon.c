#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <ncurses.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#define SYSFS_IB_BASE "/sys/class/infiniband"

typedef struct {
    char *tx_data;
    char *rx_data;
    char *tx_pkts;
    char *rx_pkts;
    bool is_ib; // if true, data counters are 4-byte words
    char *link_layer;
    char *rate;
} counters_t;

typedef enum { UNITS_BITS, UNITS_BYTES } units_t;

typedef struct {
    const char *device;
    int port;
    double interval;
    units_t units;
    const char *csv_path;
    bool csv_append;
    bool csv_headers;
} opts_t;

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static char *path_join3(const char *a, const char *b, const char *c) {
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    size_t n = la + 1 + lb + 1 + lc + 1;
    char *s = (char *)malloc(n);
    if (!s) return NULL;
    snprintf(s, n, "%s/%s/%s", a, b, c);
    return s;
}

static char *path_join4(const char *a, const char *b, const char *c, const char *d) {
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c), ld = strlen(d);
    size_t n = la + 1 + lb + 1 + lc + 1 + ld + 1;
    char *s = (char *)malloc(n);
    if (!s) return NULL;
    snprintf(s, n, "%s/%s/%s/%s", a, b, c, d);
    return s;
}

static bool file_exists(const char *p) { return access(p, F_OK) == 0; }

static char *first_existing(const char *base, const char **names) {
    for (size_t i = 0; names[i] != NULL; ++i) {
        size_t n = strlen(base) + 1 + strlen(names[i]) + 1;
        char *p = (char *)malloc(n);
        if (!p) return NULL;
        snprintf(p, n, "%s/%s", base, names[i]);
        if (file_exists(p)) {
            return p;
        }
        free(p);
    }
    return NULL;
}

static char *read_str_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[256];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return NULL; }
    fclose(f);
    // trim
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || isspace((unsigned char)buf[len-1]))) {
        buf[--len] = '\0';
    }
    char *s = strdup(buf);
    return s;
}

static bool read_u64_file(const char *path, uint64_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char buf[64];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return false; }
    fclose(f);
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(buf, &end, 10);
    if (errno != 0) return false;
    *out = (uint64_t)v;
    return true;
}

static bool resolve_counters(const char *device, int port, counters_t *c) {
    memset(c, 0, sizeof(*c));
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    char *port_base = path_join4(SYSFS_IB_BASE, device, "ports", port_str);
    if (!port_base) return false;
    size_t n = strlen(port_base) + 1 + strlen("counters") + 1;
    char *counters_base = (char *)malloc(n);
    if (!counters_base) { free(port_base); return false; }
    snprintf(counters_base, n, "%s/%s", port_base, "counters");
    if (!file_exists(counters_base)) {
        free(port_base); free(counters_base);
        return false;
    }

    // link info
    char *link_layer_path = (char *)malloc(strlen(port_base) + 1 + strlen("link_layer") + 1);
    snprintf(link_layer_path, strlen(port_base) + 1 + strlen("link_layer") + 1, "%s/%s", port_base, "link_layer");
    c->link_layer = read_str_file(link_layer_path);
    free(link_layer_path);
    char *rate_path = (char *)malloc(strlen(port_base) + 1 + strlen("rate") + 1);
    snprintf(rate_path, strlen(port_base) + 1 + strlen("rate") + 1, "%s/%s", port_base, "rate");
    c->rate = read_str_file(rate_path);
    free(rate_path);

    c->is_ib = false;
    if (c->link_layer) {
        if (strncasecmp(c->link_layer, "infiniband", 10) == 0) c->is_ib = true;
    }

    // counters
    const char *tx_data_candidates[] = { "port_xmit_data", "tx_bytes", NULL };
    const char *rx_data_candidates[] = { "port_rcv_data", "rx_bytes", NULL };
    const char *tx_pkts_candidates[] = { "port_xmit_packets", "port_xmit_pkts", "tx_packets", NULL };
    const char *rx_pkts_candidates[] = { "port_rcv_packets", "port_rcv_pkts", "rx_packets", NULL };

    c->tx_data = first_existing(counters_base, tx_data_candidates);
    c->rx_data = first_existing(counters_base, rx_data_candidates);
    c->tx_pkts = first_existing(counters_base, tx_pkts_candidates);
    c->rx_pkts = first_existing(counters_base, rx_pkts_candidates);

    free(port_base);
    free(counters_base);

    return c->tx_data && c->rx_data && c->tx_pkts && c->rx_pkts;
}

static void free_counters(counters_t *c) {
    if (!c) return;
    free(c->tx_data); free(c->rx_data); free(c->tx_pkts); free(c->rx_pkts);
    free(c->link_layer); free(c->rate);
}

static double now_monotonic(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static const char *human_rate(double Bps, units_t units, char *buf, size_t buflen) {
    static const char *u[] = { " ", "K", "M", "G", "T", "P" };
    double v = Bps;
    if (units == UNITS_BITS) v *= 8.0;
    int i = 0;
    while (fabs(v) >= 1000.0 && i < 5) { v /= 1000.0; i++; }
    snprintf(buf, buflen, "%6.2f %s%s", v, u[i], units == UNITS_BITS ? "b/s" : "B/s");
    return buf;
}

static const char *human_pps(double pps, char *buf, size_t buflen) {
    static const char *u[] = { " ", "K", "M", "G", "T" };
    double v = pps; int i = 0;
    while (fabs(v) >= 1000.0 && i < 4) { v /= 1000.0; i++; }
    snprintf(buf, buflen, "%6.2f %spps", v, u[i]);
    return buf;
}

static double parse_rate_gbps(const char *rate) {
    if (!rate) return 0.0;
    // Expect leading number, e.g., "100 Gb/sec (4X EDR)"
    char *end = NULL;
    double v = strtod(rate, &end);
    if (end == rate) return 0.0;
    return v;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s -d DEVICE [-p PORT] [-i INTERVAL] [-u bits|bytes] [--csv PATH] [--csv-append] [--csv-headers]\n"
        "\n"
        "Monitor InfiniBand bandwidth and packets via sysfs.\n",
        prog);
}

int main(int argc, char **argv) {
    opts_t opt = {0};
    opt.port = 1;
    opt.interval = 0.2;
    opt.units = UNITS_BITS;

    static struct option long_opts[] = {
        {"device", required_argument, 0, 'd'},
        {"port", required_argument, 0, 'p'},
        {"interval", required_argument, 0, 'i'},
        {"units", required_argument, 0, 'u'},
        {"csv", required_argument, 0, 1000},
        {"csv-append", no_argument, 0, 1001},
        {"csv-headers", no_argument, 0, 1002},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "d:p:i:u:", long_opts, NULL)) != -1) {
        switch (c) {
            case 'd': opt.device = optarg; break;
            case 'p': opt.port = atoi(optarg); break;
            case 'i': opt.interval = atof(optarg); break;
            case 'u':
                if (strcasecmp(optarg, "bits") == 0) opt.units = UNITS_BITS;
                else if (strcasecmp(optarg, "bytes") == 0) opt.units = UNITS_BYTES;
                else { fprintf(stderr, "Invalid --units: %s\n", optarg); return 2; }
                break;
            case 1000: opt.csv_path = optarg; break;
            case 1001: opt.csv_append = true; break;
            case 1002: opt.csv_headers = true; break;
            default: usage(argv[0]); return 2;
        }
    }

    if (!opt.device) { usage(argv[0]); fprintf(stderr, "--device is required\n"); return 2; }
    if (opt.port <= 0) { fprintf(stderr, "--port must be > 0\n"); return 2; }
    if (opt.interval <= 0) { fprintf(stderr, "--interval must be > 0\n"); return 2; }

    signal(SIGINT, on_sigint);

    counters_t ctrs;
    if (!resolve_counters(opt.device, opt.port, &ctrs)) {
        fprintf(stderr, "Failed to locate expected counters under %s/%s/ports/%d/counters\n",
                SYSFS_IB_BASE, opt.device, opt.port);
        return 1;
    }

    // CSV setup
    FILE *csv = NULL;
    if (opt.csv_path) {
        const char *mode = opt.csv_append ? "a" : "w";
        csv = fopen(opt.csv_path, mode);
        if (!csv) {
            fprintf(stderr, "Failed to open CSV path: %s\n", opt.csv_path);
        } else if (!opt.csv_append || opt.csv_headers) {
            fprintf(csv, "time_s,rx_Bps,tx_Bps,rx_pps,tx_pps\n");
            fflush(csv);
        }
    }

    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(0);

    bool paused = false;
    double rate_gbps = parse_rate_gbps(ctrs.rate);

    uint64_t p_txB=0, p_rxB=0, p_txp=0, p_rxp=0;
    if (!read_u64_file(ctrs.tx_data, &p_txB) ||
        !read_u64_file(ctrs.rx_data, &p_rxB) ||
        !read_u64_file(ctrs.tx_pkts, &p_txp) ||
        !read_u64_file(ctrs.rx_pkts, &p_rxp)) {
        endwin();
        fprintf(stderr, "Error: failed to read initial counters.\n");
        free_counters(&ctrs);
        return 1;
    }
    double prev_t = now_monotonic();

    // history for graph
    enum { HIST_CAP = 4096 };
    static double rx_hist[HIST_CAP];
    static double tx_hist[HIST_CAP];
    int hist_len = 0; // number of valid samples

    for (; !g_stop; ) {
        double loop_start = now_monotonic();

        int ch = getch();
        if (ch != ERR) {
            if (ch == 'q' || ch == 'Q') break;
            else if (ch == 'p' || ch == 'P') paused = !paused;
            else if (ch == 'u' || ch == 'U') opt.units = (opt.units == UNITS_BITS) ? UNITS_BYTES : UNITS_BITS;
        }

        static double tx_Bps=0, rx_Bps=0, tx_pps=0, rx_pps=0;
        if (!paused) {
            uint64_t c_txB=0, c_rxB=0, c_txp=0, c_rxp=0;
            bool ok = read_u64_file(ctrs.tx_data, &c_txB)
                   && read_u64_file(ctrs.rx_data, &c_rxB)
                   && read_u64_file(ctrs.tx_pkts, &c_txp)
                   && read_u64_file(ctrs.rx_pkts, &c_rxp);
            double now = now_monotonic();
            double dt = now - prev_t; if (dt <= 0) dt = 1e-9;
            if (ok) {
                uint64_t d_txB = (c_txB >= p_txB) ? (c_txB - p_txB) : (c_txB + (1ULL<<64) - p_txB);
                uint64_t d_rxB = (c_rxB >= p_rxB) ? (c_rxB - p_rxB) : (c_rxB + (1ULL<<64) - p_rxB);
                uint64_t d_txp = (c_txp >= p_txp) ? (c_txp - p_txp) : (c_txp + (1ULL<<64) - p_txp);
                uint64_t d_rxp = (c_rxp >= p_rxp) ? (c_rxp - p_rxp) : (c_rxp + (1ULL<<64) - p_rxp);

                if (ctrs.is_ib) { d_txB *= 4; d_rxB *= 4; }
                tx_Bps = (double)d_txB / dt; rx_Bps = (double)d_rxB / dt;
                tx_pps = (double)d_txp / dt; rx_pps = (double)d_rxp / dt;

                p_txB = c_txB; p_rxB = c_rxB; p_txp = c_txp; p_rxp = c_rxp; prev_t = now;

                // append to history
                if (hist_len < HIST_CAP) {
                    rx_hist[hist_len] = rx_Bps;
                    tx_hist[hist_len] = tx_Bps;
                    hist_len++;
                } else {
                    // shift left (cheap and simple; acceptable for small caps)
                    memmove(rx_hist, rx_hist+1, sizeof(double)*(HIST_CAP-1));
                    memmove(tx_hist, tx_hist+1, sizeof(double)*(HIST_CAP-1));
                    rx_hist[HIST_CAP-1] = rx_Bps;
                    tx_hist[HIST_CAP-1] = tx_Bps;
                }

                // CSV log in bytes per second
                if (csv) {
                    fprintf(csv, "%.6f,%.0f,%.0f,%.0f,%.0f\n", now, rx_Bps, tx_Bps, rx_pps, tx_pps);
                    fflush(csv);
                }
            }
        }

        erase();
        mvprintw(0, 0, "InfiniBand Bandwidth Monitor — %s port %d  [q:quit p:pause u:units]",
                 opt.device, opt.port);
        mvprintw(1, 0, "Interval: %.0f ms   Units: %s", opt.interval*1000.0,
                 (opt.units == UNITS_BITS) ? "bits" : "bytes");
        if (ctrs.link_layer) mvprintw(2, 0, "Link: %s", ctrs.link_layer);
        if (ctrs.rate) mvprintw(2, 20, "Rate: %s", ctrs.rate);

        int row = 4;
        mvprintw(row, 0, "Direction     Data Rate            Packets/s");
        row++;

        if (paused) {
            mvprintw(row, 0, "PAUSED");
        } else {
            char buf1[64], buf2[64];
            mvprintw(row+0, 0, "RX           %18s    %12s",
                     human_rate(rx_Bps, opt.units, buf1, sizeof(buf1)),
                     human_pps(rx_pps, buf2, sizeof(buf2)));
            mvprintw(row+1, 0, "TX           %18s    %12s",
                     human_rate(tx_Bps, opt.units, buf1, sizeof(buf1)),
                     human_pps(tx_pps, buf2, sizeof(buf2)));

            if (rate_gbps > 0.0) {
                double rx_util = (rx_Bps * 8.0) / (rate_gbps * 1e9) * 100.0;
                double tx_util = (tx_Bps * 8.0) / (rate_gbps * 1e9) * 100.0;
                if (rx_util < 0) rx_util = 0; if (rx_util > 100) rx_util = 100;
                if (tx_util < 0) tx_util = 0; if (tx_util > 100) tx_util = 100;
                mvprintw(row+2, 0, "Utilization  RX: %6.2f%%   TX: %6.2f%%", rx_util, tx_util);
            }
        }

        // Draw bmon-like bar graph below metrics
        int maxy, maxx; getmaxyx(stdscr, maxy, maxx);
        int chart_top = row + 4;
        int chart_height = maxy - chart_top - 1;
        int y_label_w = 10;
        if (y_label_w < 0) y_label_w = 0;
        int chart_width = maxx - 2 - y_label_w; // 1 col margin left/right
        if (chart_height > 3 && chart_width > 10 && hist_len > 1) {
            // compute max value to scale (based on selected units)
            double maxv = 1.0;
            int samples_to_draw = chart_width;
            if (samples_to_draw > hist_len) samples_to_draw = hist_len;
            for (int i = 0; i < samples_to_draw; ++i) {
                double rxv = rx_hist[hist_len - samples_to_draw + i];
                double txv = tx_hist[hist_len - samples_to_draw + i];
                double rxd = (opt.units == UNITS_BITS) ? rxv*8.0 : rxv;
                double txd = (opt.units == UNITS_BITS) ? txv*8.0 : txv;
                if (rxd > maxv) maxv = rxd;
                if (txd > maxv) maxv = txd;
            }
            if (opt.units == UNITS_BITS && rate_gbps > 0) {
                double link_bps = rate_gbps * 1e9;
                if (link_bps > 0 && link_bps < maxv) maxv = link_bps;
            }

            // scale labels (top/mid/bottom)
            char topbuf[32], midbuf[32], botbuf[32];
            snprintf(topbuf, sizeof(topbuf), "%5.1f %s", maxv/1e9, (opt.units==UNITS_BITS)?"Gb/s":"GB/s");
            snprintf(midbuf, sizeof(midbuf), "%5.1f %s", (maxv/2.0)/1e9, (opt.units==UNITS_BITS)?"Gb/s":"GB/s");
            snprintf(botbuf, sizeof(botbuf), "%5.1f %s", 0.0, (opt.units==UNITS_BITS)?"Gb/s":"GB/s");
            mvprintw(chart_top, 0, "%*s |", y_label_w-2, topbuf);
            mvprintw(chart_top + chart_height/2, 0, "%*s |", y_label_w-2, midbuf);
            mvprintw(chart_top + chart_height - 1, 0, "%*s |", y_label_w-2, botbuf);

            // colors
            bool use_colors = has_colors();
            if (use_colors) { start_color(); init_pair(1, COLOR_CYAN, -1); init_pair(2, COLOR_RED, -1); init_pair(3, COLOR_WHITE, -1);}            

            // draw bars
            for (int x = 0; x < samples_to_draw; ++x) {
                double rxv = rx_hist[hist_len - samples_to_draw + x];
                double txv = tx_hist[hist_len - samples_to_draw + x];
                double rxd = (opt.units == UNITS_BITS) ? rxv*8.0 : rxv;
                double txd = (opt.units == UNITS_BITS) ? txv*8.0 : txv;
                int rh = (int)llround((rxd / maxv) * (chart_height));
                int th = (int)llround((txd / maxv) * (chart_height));
                if (rh < 0) rh = 0; if (rh > chart_height) rh = chart_height;
                if (th < 0) th = 0; if (th > chart_height) th = chart_height;
                int col = y_label_w + 1 + x;
                for (int yy = 0; yy < chart_height; ++yy) {
                    int y = chart_top + (chart_height - 1 - yy);
                    char ch = ' ';
                    short color = 0;
                    bool rx_on = (yy < rh);
                    bool tx_on = (yy < th);
                    if (rx_on && tx_on) { ch = '#'; color = 3; }
                    else if (rx_on) { ch = '*'; color = 1; }
                    else if (tx_on) { ch = '+'; color = 2; }
                    if (use_colors && color) attron(COLOR_PAIR(color));
                    mvaddch(y, col, ch);
                    if (use_colors && color) attroff(COLOR_PAIR(color));
                }
            }

            // x-axis baseline
            for (int x = 0; x < samples_to_draw; ++x) mvaddch(chart_top + chart_height, y_label_w + 1 + x, '-');
            mvprintw(chart_top + chart_height, maxx - 20, "*RX  +TX  #Both");
        }

        refresh();

        // sleep remaining time
        double elapsed = now_monotonic() - loop_start;
        double to_sleep = opt.interval - elapsed;
        if (to_sleep > 0) {
            struct timespec ts;
            ts.tv_sec = (time_t)to_sleep;
            ts.tv_nsec = (long)((to_sleep - ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);
        }
    }

    endwin();
    if (csv) fclose(csv);
    free_counters(&ctrs);
    return 0;
}
