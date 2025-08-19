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
    double duration; // seconds, 0 = infinite
} opts_t;

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }
static volatile sig_atomic_t g_resized = 0;
static void on_sigwinch(int sig) { (void)sig; g_resized = 1; }

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

static void draw_panel_win(WINDOW *win, const char *title, double cur_Bps, double cur_pps,
                           double *hist, int hist_len, units_t units, double rate_gbps,
                           bool use_colors)
{
    int wy, wx; getmaxyx(win, wy, wx);
    werase(win);
    if (use_colors) wbkgd(win, COLOR_PAIR(11));
    if (use_colors) wattron(win, COLOR_PAIR(13));
    box(win, 0, 0);
    if (use_colors) wattroff(win, COLOR_PAIR(13));
    char ratebuf[64], ppsbuf[64];
    human_rate(cur_Bps, units, ratebuf, sizeof(ratebuf));
    human_pps(cur_pps, ppsbuf, sizeof(ppsbuf));
    if (use_colors) wattron(win, COLOR_PAIR(10));
    mvwprintw(win, 0, 2, " %s  %s  %s ", title ? title : "", ratebuf, ppsbuf);

    int y_label_w = 12;
    int chart_h = wy - 3;
    int chart_w = wx - 2 - y_label_w;
    if (chart_h < 3 || chart_w < 10 || hist_len < 2) {
        wnoutrefresh(win);
        return;
    }
    int samples = chart_w;
    if (samples > hist_len) samples = hist_len;
    double maxv = 1.0;
    for (int i = 0; i < samples; ++i) {
        double v = hist[hist_len - samples + i];
        if (units == UNITS_BITS) v *= 8.0;
        if (v > maxv) maxv = v;
    }
    if (units == UNITS_BITS && rate_gbps > 0) {
        double link_bps = rate_gbps * 1e9;
        if (link_bps > 0 && link_bps < maxv) maxv = link_bps;
    }
    char topbuf[32], midbuf[32];
    snprintf(topbuf, sizeof(topbuf), "%6.2f %s", maxv/1e9, (units==UNITS_BITS)?"Gb/s":"GB/s");
    snprintf(midbuf, sizeof(midbuf), "%6.2f %s", (maxv/2.0)/1e9, (units==UNITS_BITS)?"Gb/s":"GB/s");
    mvwprintw(win, 1, 1, "%*s |", y_label_w-3, topbuf);
    mvwprintw(win, 1 + chart_h/2, 1, "%*s |", y_label_w-3, midbuf);
    mvwprintw(win, 1 + chart_h - 1, 1, "%*s |", y_label_w-3, "0.00 ");

    for (int x = 0; x < samples; ++x) {
        double v = hist[hist_len - samples + x];
        if (units == UNITS_BITS) v *= 8.0;
        int h = (int)llround((v / maxv) * chart_h);
        if (h < 0) h = 0; if (h > chart_h) h = chart_h;
        int col = y_label_w + 1 + x;
        for (int yy = 0; yy < chart_h; ++yy) {
            int y = 1 + (chart_h - 1 - yy);
            bool on = (yy < h);
            if (on) {
                if (use_colors) wattron(win, (title && title[0]=='R')? COLOR_PAIR(1) : COLOR_PAIR(2));
                mvwaddch(win, y, col, ACS_CKBOARD);
                if (use_colors) wattroff(win, (title && title[0]=='R')? COLOR_PAIR(1) : COLOR_PAIR(2));
            }
        }
    }
    for (int x = 0; x < samples; ++x) mvwaddch(win, 1 + chart_h, y_label_w + 1 + x, '_');
    mvwprintw(win, wy-1, wx-18, (title && title[0]=='R')? "Bars:* Cyan" : "Bars:+ Red");
    if (use_colors) wattroff(win, COLOR_PAIR(10));
    wnoutrefresh(win);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s -d DEVICE [-p PORT] [-i INTERVAL] [-u bits|bytes] [--csv PATH] [--csv-append] [--csv-headers] [--duration SECONDS]\n"
        "\n"
        "Monitor InfiniBand bandwidth and packets via sysfs.\n",
        prog);
}

int main(int argc, char **argv) {
    opts_t opt = {0};
    opt.port = 1;
    opt.interval = 1.0;
    opt.units = UNITS_BITS;

    static struct option long_opts[] = {
        {"device", required_argument, 0, 'd'},
        {"port", required_argument, 0, 'p'},
        {"interval", required_argument, 0, 'i'},
        {"units", required_argument, 0, 'u'},
        {"csv", required_argument, 0, 1000},
        {"csv-append", no_argument, 0, 1001},
        {"csv-headers", no_argument, 0, 1002},
        {"duration", required_argument, 0, 1003},
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
            case 1003: opt.duration = atof(optarg); break;
            default: usage(argv[0]); return 2;
        }
    }

    if (!opt.device) { usage(argv[0]); fprintf(stderr, "--device is required\n"); return 2; }
    if (opt.port <= 0) { fprintf(stderr, "--port must be > 0\n"); return 2; }
    if (opt.interval <= 0) { fprintf(stderr, "--interval must be > 0\n"); return 2; }

    signal(SIGINT, on_sigint);
    signal(SIGWINCH, on_sigwinch);

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
    bool use_colors = false;
    if (has_colors()) {
        start_color();
        use_colors = true;
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);               // RX bars
        init_pair(2, COLOR_RED, -1);                // TX bars
        init_pair(10, COLOR_WHITE, COLOR_BLACK);    // light text on dark bg
        init_pair(11, COLOR_BLACK, COLOR_BLACK);    // panel bg
        init_pair(12, COLOR_BLACK, COLOR_BLACK);    // header bg
        init_pair(13, COLOR_WHITE, COLOR_BLACK);    // borders
    }

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

    // windows
    WINDOW *win_hdr = NULL, *win_rx = NULL, *win_tx = NULL;
    int prev_maxy = -1, prev_maxx = -1;

    double start_time = now_monotonic();
    for (; !g_stop; ) {
        double loop_start = now_monotonic();

        if (g_resized) {
            g_resized = 0;
            endwin();
            refresh();
            prev_maxy = -1; prev_maxx = -1; // force window recreate
        }

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
            }

            // append to history regardless so the graph scrolls
            if (hist_len < HIST_CAP) {
                rx_hist[hist_len] = rx_Bps;
                tx_hist[hist_len] = tx_Bps;
                hist_len++;
            } else {
                memmove(rx_hist, rx_hist+1, sizeof(double)*(HIST_CAP-1));
                memmove(tx_hist, tx_hist+1, sizeof(double)*(HIST_CAP-1));
                rx_hist[HIST_CAP-1] = rx_Bps;
                tx_hist[HIST_CAP-1] = tx_Bps;
            }

            // CSV log in bytes per second (even if same values)
            if (csv) {
                fprintf(csv, "%.6f,%.0f,%.0f,%.0f,%.0f\n", now, rx_Bps, tx_Bps, rx_pps, tx_pps);
                fflush(csv);
            }
        }

        // Layout: header + two stacked windows: RX then TX
        int maxy, maxx; getmaxyx(stdscr, maxy, maxx);
        int hdr_h = 4;
        int remaining = maxy - hdr_h;
        if (remaining < 6) remaining = 6;
        int rx_h = remaining / 2;
        int tx_h = remaining - rx_h;

        // recreate windows if size changed
        if (maxy != prev_maxy || maxx != prev_maxx || !win_hdr || !win_rx || !win_tx) {
            if (win_hdr) { delwin(win_hdr); win_hdr = NULL; }
            if (win_rx) { delwin(win_rx); win_rx = NULL; }
            if (win_tx) { delwin(win_tx); win_tx = NULL; }
            win_hdr = newwin(hdr_h, maxx, 0, 0);
            win_rx = newwin(rx_h, maxx, hdr_h, 0);
            win_tx = newwin(tx_h, maxx, hdr_h + rx_h, 0);
            prev_maxy = maxy; prev_maxx = maxx;
        }

        // Header window
        werase(win_hdr);
        if (use_colors) wbkgd(win_hdr, COLOR_PAIR(12));
        if (use_colors) wattron(win_hdr, COLOR_PAIR(13));
        box(win_hdr, 0, 0);
        if (use_colors) wattroff(win_hdr, COLOR_PAIR(13));
        if (use_colors) wattron(win_hdr, COLOR_PAIR(10));
        // Title and current time on same line
        mvwprintw(win_hdr, 0, 2, " InfiniBand Bandwidth Monitor ");
        // Time: MMMM-YY-DD HH:MM:SS
        {
            time_t t = time(NULL);
            struct tm lt; localtime_r(&t, &lt);
            char tbuf[64];
            strftime(tbuf, sizeof(tbuf), "%B-%y-%d %H:%M:%S", &lt);
            int maxx_hdr, maxy_hdr; getmaxyx(win_hdr, maxy_hdr, maxx_hdr);
            int col = maxx_hdr - (int)strlen(tbuf) - 2; if (col < 2) col = 2;
            mvwprintw(win_hdr, 0, col, "%s", tbuf);
        }
        mvwprintw(win_hdr, 1, 2, "%s port %d  [q:quit p:pause u:units]",
                  opt.device, opt.port);
        mvwprintw(win_hdr, 2, 2, "Interval: %.0f ms   Units: %s",
                  opt.interval*1000.0, (opt.units == UNITS_BITS) ? "bits" : "bytes");
        if (ctrs.link_layer) mvwprintw(win_hdr, 1, maxx/2, "Link: %s", ctrs.link_layer);
        if (ctrs.rate) mvwprintw(win_hdr, 2, maxx/2, "Rate: %s", ctrs.rate);
        if (paused) mvwprintw(win_hdr, 1, maxx-12, "[PAUSED]");
        if (use_colors) wattroff(win_hdr, COLOR_PAIR(10));
        wnoutrefresh(win_hdr);

        // Draw RX panel
        draw_panel_win(win_rx, "RX", rx_Bps, rx_pps, rx_hist, hist_len, opt.units, rate_gbps, use_colors);
        // Draw TX panel
        draw_panel_win(win_tx, "TX", tx_Bps, tx_pps, tx_hist, hist_len, opt.units, rate_gbps, use_colors);

        doupdate();

        // sleep remaining time
        double elapsed = now_monotonic() - loop_start;
        double to_sleep = opt.interval - elapsed;
        if (to_sleep > 0) {
            struct timespec ts;
            ts.tv_sec = (time_t)to_sleep;
            ts.tv_nsec = (long)((to_sleep - ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);
        }

        if (opt.duration > 0 && (now_monotonic() - start_time) >= opt.duration) {
            break;
        }
    }

    if (win_hdr) delwin(win_hdr);
    if (win_rx) delwin(win_rx);
    if (win_tx) delwin(win_tx);
    endwin();
    if (csv) fclose(csv);
    free_counters(&ctrs);
    return 0;
}
