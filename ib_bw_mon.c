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
#include <inttypes.h>
#include <dirent.h>

#define SYSFS_IB_BASE "/sys/class/infiniband"

typedef struct {
    char *tx_data;
    char *rx_data;
    char *tx_pkts;
    char *rx_pkts;
    bool data_is_words; // true if data counters are 4-byte words
    char *link_layer;
    char *rate;
    // optional counters
    char *tx_discards;
    char *tx_wait;
    char *rx_errors;
    char *rx_remote_phy_err;
    char *rx_switch_relay_err;
    char *local_phy_errors;
    char *symbol_error;
    char *link_error_recovery;
    char *link_downed;
    char *vl15_dropped;
    char *excessive_buf_overrun;
} counters_t;

typedef struct {
    int idx;
    char *gid;
    char *type;
    char *ndev;
} gid_entry_t;

// Multi-device monitoring state
typedef struct {
    char name[128];
    counters_t ctrs;
    double rate_gbps;
    uint64_t prev_tx_data, prev_rx_data, prev_tx_pkts, prev_rx_pkts;
    double tx_Bps, rx_Bps, tx_pps, rx_pps;
    int hist_len;
    double rx_hist[4096];
    double tx_hist[4096];
    WINDOW *win;
} mon_dev_t;

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
    int bg_mode; // 0 = black, 1 = terminal default (-1)
} opts_t;

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }
static volatile sig_atomic_t g_resized = 0;
static void on_sigwinch(int sig) { (void)sig; g_resized = 1; }

/* removed unused path_join3 */

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

    c->data_is_words = false;

    // counters
    const char *tx_data_candidates[] = { "port_xmit_data", "tx_bytes", NULL };
    const char *rx_data_candidates[] = { "port_rcv_data", "rx_bytes", NULL };
    const char *tx_pkts_candidates[] = { "port_xmit_packets", "port_xmit_pkts", "tx_packets", NULL };
    const char *rx_pkts_candidates[] = { "port_rcv_packets", "port_rcv_pkts", "rx_packets", NULL };

    c->tx_data = first_existing(counters_base, tx_data_candidates);
    c->rx_data = first_existing(counters_base, rx_data_candidates);
    c->tx_pkts = first_existing(counters_base, tx_pkts_candidates);
    c->rx_pkts = first_existing(counters_base, rx_pkts_candidates);

    // Determine if data counters are in 4-byte words (typical for port_*_data)
    if (c->tx_data && strstr(c->tx_data, "port_xmit_data")) c->data_is_words = true;
    if (c->rx_data && strstr(c->rx_data, "port_rcv_data")) c->data_is_words = true;

    // Optional counters
    const char *tx_discards_candidates[] = { "port_xmit_discards", NULL };
    const char *tx_wait_candidates[] = { "port_xmit_wait", NULL };
    const char *rx_errors_candidates[] = { "port_rcv_errors", NULL };
    const char *rx_remote_phy_candidates[] = { "port_rcv_remote_physical_errors", NULL };
    const char *rx_switch_relay_candidates[] = { "port_rcv_switch_relay_errors", NULL };
    const char *local_phy_err_candidates[] = { "port_local_phy_errors", "port_local_physical_errors", NULL };
    const char *symbol_error_candidates[] = { "symbol_error", "symbol_errors", NULL };
    const char *link_err_recovery_candidates[] = { "link_error_recovery", NULL };
    const char *link_downed_candidates[] = { "link_downed", NULL };
    const char *vl15_candidates[] = { "VL15_dropped", "vl15_dropped", NULL };
    const char *excessive_buf_overrun_candidates[] = { "excessive_buffer_overrun_errors", NULL };

    c->tx_discards = first_existing(counters_base, tx_discards_candidates);
    c->tx_wait = first_existing(counters_base, tx_wait_candidates);
    c->rx_errors = first_existing(counters_base, rx_errors_candidates);
    c->rx_remote_phy_err = first_existing(counters_base, rx_remote_phy_candidates);
    c->rx_switch_relay_err = first_existing(counters_base, rx_switch_relay_candidates);
    c->local_phy_errors = first_existing(counters_base, local_phy_err_candidates);
    c->symbol_error = first_existing(counters_base, symbol_error_candidates);
    c->link_error_recovery = first_existing(counters_base, link_err_recovery_candidates);
    c->link_downed = first_existing(counters_base, link_downed_candidates);
    c->vl15_dropped = first_existing(counters_base, vl15_candidates);
    c->excessive_buf_overrun = first_existing(counters_base, excessive_buf_overrun_candidates);

    free(port_base);
    free(counters_base);

    return c->tx_data && c->rx_data && c->tx_pkts && c->rx_pkts;
}

static void free_counters(counters_t *c) {
    if (!c) return;
    free(c->tx_data); free(c->rx_data); free(c->tx_pkts); free(c->rx_pkts);
    free(c->link_layer); free(c->rate);
    free(c->tx_discards); free(c->tx_wait); free(c->rx_errors); free(c->rx_remote_phy_err);
    free(c->rx_switch_relay_err); free(c->local_phy_errors); free(c->symbol_error);
    free(c->link_error_recovery); free(c->link_downed); free(c->vl15_dropped);
    free(c->excessive_buf_overrun);
}

static bool gid_is_zero(const char *s)
{
    if (!s) return true;
    for (const char *p = s; *p; ++p) {
        if (*p == ':') continue;
        if (*p != '0') return false;
    }
    return true;
}

static void free_gid_list(gid_entry_t *list, int count)
{
    if (!list) return;
    for (int i = 0; i < count; ++i) {
        free(list[i].gid);
        free(list[i].type);
        free(list[i].ndev);
    }
    free(list);
}

static void fetch_gid_list(const char *dev, int port, gid_entry_t **out_list, int *out_count)
{
    char port_str[16]; snprintf(port_str, sizeof(port_str), "%d", port);
    char base[512]; snprintf(base, sizeof(base), "%s/%s/ports/%s", SYSFS_IB_BASE, dev, port_str);
    gid_entry_t *arr = (gid_entry_t*)calloc(256, sizeof(gid_entry_t));
    int cnt = 0;
    for (int i = 0; i < 256; ++i) {
        char p_gid[640]; snprintf(p_gid, sizeof(p_gid), "%s/gids/%d", base, i);
        if (access(p_gid, R_OK) != 0) continue;
        char *gid = read_str_file(p_gid);
        if (!gid || gid_is_zero(gid)) { free(gid); continue; }
        char p_type[640]; snprintf(p_type, sizeof(p_type), "%s/gid_attrs/types/%d", base, i);
        char p_ndev[640]; snprintf(p_ndev, sizeof(p_ndev), "%s/gid_attrs/ndevs/%d", base, i);
        char *type = read_str_file(p_type);
        char *ndev = read_str_file(p_ndev);
        arr[cnt].idx = i; arr[cnt].gid = gid; arr[cnt].type = type ? type : strdup(""); arr[cnt].ndev = ndev ? ndev : strdup("");
        cnt++;
    }
    *out_list = arr;
    *out_count = cnt;
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

static void format_scale_label(double v_disp_per_s, units_t units, char *buf, size_t buflen) {
    // v_disp_per_s is already in chosen display units (bits or bytes per second)
    const char *suffixes_bits[] = {"b/s","Kb/s","Mb/s","Gb/s","Tb/s","Pb/s"};
    const char *suffixes_bytes[] = {"B/s","KB/s","MB/s","GB/s","TB/s","PB/s"};
    double v = v_disp_per_s;
    int idx = 0;
    while (v >= 1000.0 && idx < 5) { v /= 1000.0; idx++; }
    const char *suf = (units == UNITS_BITS) ? suffixes_bits[idx] : suffixes_bytes[idx];
    snprintf(buf, buflen, "%6.2f %s", v, suf);
}

static void draw_ascii_box(WINDOW *w)
{
    wborder(w, '|', '|', '-', '-', '+', '+', '+', '+');
}

static void draw_panel_win(WINDOW *win, const char *title, double cur_Bps, double cur_pps,
                           double *hist, int hist_len, units_t units, double rate_gbps,
                           bool use_colors, bool light)
{
    int wy, wx; getmaxyx(win, wy, wx);
    werase(win);
    if (use_colors) {
        wbkgd(win, COLOR_PAIR(11));
        wbkgdset(win, COLOR_PAIR(11) | ' ');
        wattron(win, COLOR_PAIR(13));
    }
    draw_ascii_box(win);
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
    // dynamic labels with appropriate units
    char topbuf[32], midbuf[32], botbuf[32];
    format_scale_label(maxv, units, topbuf, sizeof(topbuf));
    format_scale_label(maxv/2.0, units, midbuf, sizeof(midbuf));
    snprintf(botbuf, sizeof(botbuf), "%s", (units == UNITS_BITS) ? "0.00 b/s" : "0.00 B/s");
    int lblw = (int)strlen(topbuf);
    if ((int)strlen(midbuf) > lblw) lblw = (int)strlen(midbuf);
    if ((int)strlen(botbuf) > lblw) lblw = (int)strlen(botbuf);
    y_label_w = lblw + 3; // 1 space padding + '|' + margin
    chart_w = wx - 2 - y_label_w;
    if (chart_w < 1) chart_w = 1;
    // y-axis with right-aligned labels
    mvwprintw(win, 1, 1, "%*s |", y_label_w-3, topbuf);
    mvwprintw(win, 1 + chart_h/2, 1, "%*s |", y_label_w-3, midbuf);
    mvwprintw(win, 1 + chart_h - 1, 1, "%*s |", y_label_w-3, botbuf);

    // right-aligned drawing: newest sample at far right
    int base_col = y_label_w + 1 + (chart_w - samples);
    // fill plot area with dots in bar color; ensure background matches panel
    if (!light) {
        if (use_colors) wcolor_set(win, (title && title[0]=='R')? 1 : 2, NULL);
        for (int i = 0; i < samples; ++i) {
            int col = base_col + i;
            for (int yy = 0; yy < chart_h; ++yy) {
                int y = 1 + (chart_h - 1 - yy);
                mvwaddch(win, y, col, '.');
            }
        }
    }
    // draw bars on top
    if (use_colors) wcolor_set(win, (title && title[0]=='R')? 1 : 2, NULL);
    for (int i = 0; i < samples; ++i) {
        double v = hist[hist_len - samples + i];
        if (units == UNITS_BITS) v *= 8.0;
        int h = (int)llround((v / maxv) * chart_h);
        if (h < 0) h = 0;
        if (h > chart_h) h = chart_h;
        int col = base_col + i;
        for (int yy = 0; yy < h; ++yy) {
            int y = 1 + (chart_h - 1 - yy);
            mvwaddch(win, y, col, '|');
        }
    }
    if (use_colors) wcolor_set(win, 10, NULL);
    wnoutrefresh(win);
}

static void draw_device_pane(WINDOW *pane, const char *devname, mon_dev_t *md, units_t units, bool use_colors, bool light)
{
    int ph, pw; getmaxyx(pane, ph, pw);
    werase(pane);
    if (use_colors) { wbkgd(pane, COLOR_PAIR(11)); wattron(pane, COLOR_PAIR(13)); }
    draw_ascii_box(pane);
    if (use_colors) { wattroff(pane, COLOR_PAIR(13)); wattron(pane, COLOR_PAIR(10)); }
    mvwprintw(pane, 0, 2, " %s ", devname);
    if (use_colors) wattroff(pane, COLOR_PAIR(10));
    int inner_h = ph - 2; if (inner_h < 4) inner_h = 4;
    int rx_h = inner_h / 2;
    int tx_h = inner_h - rx_h;
    WINDOW *sub_rx = derwin(pane, rx_h, pw - 2, 1, 1);
    WINDOW *sub_tx = derwin(pane, tx_h, pw - 2, 1 + rx_h, 1);
    draw_panel_win(sub_rx, "RX", md->rx_Bps, md->rx_pps, md->rx_hist, md->hist_len, units, md->rate_gbps, use_colors, light);
    draw_panel_win(sub_tx, "TX", md->tx_Bps, md->tx_pps, md->tx_hist, md->hist_len, units, md->rate_gbps, use_colors, light);
    delwin(sub_rx);
    delwin(sub_tx);
    wnoutrefresh(pane);
}

static void draw_device_data_pane(WINDOW *pane, const char *devname, mon_dev_t *md, bool use_colors)
{
    werase(pane);
    if (use_colors) { wbkgd(pane, COLOR_PAIR(11)); wattron(pane, COLOR_PAIR(13)); }
    draw_ascii_box(pane);
    if (use_colors) { wattroff(pane, COLOR_PAIR(13)); wattron(pane, COLOR_PAIR(10)); }
    mvwprintw(pane, 0, 2, " %s - Raw Counters ", devname);
    int row = 1;
    uint64_t v;
    if (md->ctrs.rx_data && read_u64_file(md->ctrs.rx_data, &v)) mvwprintw(pane, row++, 2, "port_rcv_data:    %20" PRIu64 " %s", v, md->ctrs.data_is_words ? "(words)" : "");
    if (md->ctrs.rx_pkts && read_u64_file(md->ctrs.rx_pkts, &v)) mvwprintw(pane, row++, 2, "port_rcv_packets: %20" PRIu64, v);
    if (md->ctrs.rx_errors && read_u64_file(md->ctrs.rx_errors, &v)) mvwprintw(pane, row++, 2, "port_rcv_errors:  %20" PRIu64, v);
    if (md->ctrs.rx_remote_phy_err && read_u64_file(md->ctrs.rx_remote_phy_err, &v)) mvwprintw(pane, row++, 2, "rcv_remote_phy:   %20" PRIu64, v);
    if (md->ctrs.rx_switch_relay_err && read_u64_file(md->ctrs.rx_switch_relay_err, &v)) mvwprintw(pane, row++, 2, "rcv_switch_relay: %20" PRIu64, v);
    if (md->ctrs.tx_data && read_u64_file(md->ctrs.tx_data, &v)) mvwprintw(pane, row++, 2, "port_xmit_data:   %20" PRIu64 " %s", v, md->ctrs.data_is_words ? "(words)" : "");
    if (md->ctrs.tx_pkts && read_u64_file(md->ctrs.tx_pkts, &v)) mvwprintw(pane, row++, 2, "port_xmit_packets:%20" PRIu64, v);
    if (md->ctrs.tx_discards && read_u64_file(md->ctrs.tx_discards, &v)) mvwprintw(pane, row++, 2, "xmit_discards:    %20" PRIu64, v);
    if (md->ctrs.tx_wait && read_u64_file(md->ctrs.tx_wait, &v)) mvwprintw(pane, row++, 2, "xmit_wait:        %20" PRIu64, v);
    if (md->ctrs.local_phy_errors && read_u64_file(md->ctrs.local_phy_errors, &v)) mvwprintw(pane, row++, 2, "local_phy_errors: %20" PRIu64, v);
    if (md->ctrs.symbol_error && read_u64_file(md->ctrs.symbol_error, &v)) mvwprintw(pane, row++, 2, "symbol_error:     %20" PRIu64, v);
    if (md->ctrs.link_error_recovery && read_u64_file(md->ctrs.link_error_recovery, &v)) mvwprintw(pane, row++, 2, "link_err_recov:   %20" PRIu64, v);
    if (md->ctrs.link_downed && read_u64_file(md->ctrs.link_downed, &v)) mvwprintw(pane, row++, 2, "link_downed:      %20" PRIu64, v);
    if (md->ctrs.vl15_dropped && read_u64_file(md->ctrs.vl15_dropped, &v)) mvwprintw(pane, row++, 2, "vl15_dropped:     %20" PRIu64, v);
    if (md->ctrs.excessive_buf_overrun && read_u64_file(md->ctrs.excessive_buf_overrun, &v)) mvwprintw(pane, row++, 2, "excess_buf_over:  %20" PRIu64, v);
    if (use_colors) wattroff(pane, COLOR_PAIR(10));
    wnoutrefresh(pane);
}

static void draw_device_info_pane(WINDOW *pane, const char *devname, bool use_colors)
{
    int ph, pw; getmaxyx(pane, ph, pw);
    werase(pane);
    if (use_colors) { wbkgd(pane, COLOR_PAIR(11)); wattron(pane, COLOR_PAIR(13)); }
    draw_ascii_box(pane);
    if (use_colors) { wattroff(pane, COLOR_PAIR(13)); wattron(pane, COLOR_PAIR(10)); }
    mvwprintw(pane, 0, 2, " %s - GIDs ", devname);
    mvwprintw(pane, 1, 2, "Idx  Type        Ndev              GID");
    gid_entry_t *list = NULL; int cnt = 0;
    fetch_gid_list(devname, 1, &list, &cnt);
    int row = 2;
    for (int i = 0; i < cnt && row < ph - 1; ++i) {
        char line[512];
        snprintf(line, sizeof(line), "%3d  %-10s  %-16s  %s", list[i].idx,
                 list[i].type ? list[i].type : "",
                 list[i].ndev ? list[i].ndev : "",
                 list[i].gid ? list[i].gid : "");
        mvwprintw(pane, row++, 2, "%.*s", pw - 4, line);
    }
    free_gid_list(list, cnt);
    if (use_colors) wattroff(pane, COLOR_PAIR(10));
    wnoutrefresh(pane);
}

static bool file_read_has(const char *path, const char *needle)
{
    char *s = read_str_file(path);
    if (!s) return false;
    bool ok = (strstr(s, needle) != NULL);
    free(s);
    return ok;
}

static int enumerate_active_devices(char names[][128], int maxn)
{
    DIR *d = opendir(SYSFS_IB_BASE);
    if (!d) return 0;
    int count = 0; struct dirent *de;
    while ((de = readdir(d)) != NULL && count < maxn) {
        if (de->d_name[0] == '.') continue;
        char p[512]; snprintf(p, sizeof(p), "%s/%s/ports/1/state", SYSFS_IB_BASE, de->d_name);
        if (access(p, R_OK) != 0) continue;
        if (!file_read_has(p, "ACTIVE")) continue;
        // Copy device name safely into fixed buffer
        snprintf(names[count], 128, "%.127s", de->d_name);
        count++;
    }
    closedir(d);
    return count;
}

static int parse_device_list(const char *arg, char names[][128], int maxn)
{
    if (!arg) return 0;
    int count = 0; const char *p = arg; const char *start = p;
    while (*p && count < maxn) {
        if (*p == ',') {
            int len = (int)(p - start);
            if (len > 0) {
                if (len > 127) len = 127;
                memcpy(names[count], start, (size_t)len);
                names[count][len] = '\0';
                count++;
            }
            start = p + 1;
        }
        p++;
    }
    if (start && *start && count < maxn) {
        int len = (int)strlen(start);
        if (len > 0) {
            if (len > 127) len = 127;
            memcpy(names[count], start, (size_t)len);
            names[count][len] = '\0';
            count++;
        }
    }
    return count;
}

static int run_multi_mode(char devs[][128], int ndev, opts_t *opt)
{
    initscr(); cbreak(); noecho(); nodelay(stdscr, FALSE); keypad(stdscr, TRUE); curs_set(0); timeout((int)(opt->interval * 1000));
    bool use_colors = false;
    if (has_colors()) {
        start_color(); use_colors = true; use_default_colors();
        int bg = (opt->bg_mode == 1 ? -1 : COLOR_BLACK);
        int fg_text = (opt->bg_mode == 1 ? -1 : COLOR_WHITE);
        int fg_border = (opt->bg_mode == 1 ? -1 : COLOR_WHITE);
        init_pair(1, COLOR_CYAN, bg); init_pair(2, COLOR_RED, bg);
        init_pair(10, fg_text, bg); init_pair(11, bg, bg); init_pair(12, bg, bg); init_pair(13, fg_border, bg);
    }
    mon_dev_t *md = calloc(ndev, sizeof(mon_dev_t));
    for (int i = 0; i < ndev; ++i) {
        snprintf(md[i].name, sizeof(md[i].name), "%s", devs[i]);
        if (!resolve_counters(devs[i], 1, &md[i].ctrs)) continue;
        md[i].rate_gbps = parse_rate_gbps(md[i].ctrs.rate);
        read_u64_file(md[i].ctrs.tx_data, &md[i].prev_tx_data);
        read_u64_file(md[i].ctrs.rx_data, &md[i].prev_rx_data);
        read_u64_file(md[i].ctrs.tx_pkts, &md[i].prev_tx_pkts);
        read_u64_file(md[i].ctrs.rx_pkts, &md[i].prev_rx_pkts);
        md[i].hist_len = 0; md[i].tx_Bps = md[i].rx_Bps = md[i].tx_pps = md[i].rx_pps = 0.0; md[i].win = NULL;
    }
    double start_time = now_monotonic();
    double prev_t = now_monotonic();
    enum { VIEW_PLOT=0, VIEW_DATA=1, VIEW_INFO=2 };
    int view = VIEW_PLOT; bool paused = false;
    for (;;) {
        int ch = getch();
        bool fast_switch = false;
        if (ch != ERR) {
            if (ch == 'q' || ch == 'Q') break;
            if (ch == 'u' || ch == 'U') opt->units = (opt->units == UNITS_BITS) ? UNITS_BYTES : UNITS_BITS;
            if (ch == 'p' || ch == 'P') paused = !paused;
            if (ch == 'd' || ch == 'D') { view = (view == VIEW_DATA) ? VIEW_PLOT : VIEW_DATA; fast_switch = true; }
            if (ch == 'i' || ch == 'I') { view = (view == VIEW_INFO) ? VIEW_PLOT : VIEW_INFO; fast_switch = true; }
        }
        double nowt = now_monotonic(); double dt = nowt - prev_t; if (dt <= 0) dt = 1e-9;
        if (!fast_switch && !paused) {
            for (int i = 0; i < ndev; ++i) {
                if (!md[i].ctrs.tx_data) continue;
                uint64_t c_txB=0,c_rxB=0,c_txp=0,c_rxp=0;
                if (!read_u64_file(md[i].ctrs.tx_data,&c_txB)) continue;
                if (!read_u64_file(md[i].ctrs.rx_data,&c_rxB)) continue;
                if (!read_u64_file(md[i].ctrs.tx_pkts,&c_txp)) continue;
                if (!read_u64_file(md[i].ctrs.rx_pkts,&c_rxp)) continue;
                uint64_t d_txB = (c_txB >= md[i].prev_tx_data) ? (c_txB - md[i].prev_tx_data) : (c_txB + (UINT64_MAX - md[i].prev_tx_data) + 1);
                uint64_t d_rxB = (c_rxB >= md[i].prev_rx_data) ? (c_rxB - md[i].prev_rx_data) : (c_rxB + (UINT64_MAX - md[i].prev_rx_data) + 1);
                uint64_t d_txp = (c_txp >= md[i].prev_tx_pkts) ? (c_txp - md[i].prev_tx_pkts) : (c_txp + (UINT64_MAX - md[i].prev_tx_pkts) + 1);
                uint64_t d_rxp = (c_rxp >= md[i].prev_rx_pkts) ? (c_rxp - md[i].prev_rx_pkts) : (c_rxp + (UINT64_MAX - md[i].prev_rx_pkts) + 1);
                if (md[i].ctrs.data_is_words) { d_txB *= 4; d_rxB *= 4; }
                md[i].tx_Bps = (double)d_txB / dt; md[i].rx_Bps = (double)d_rxB / dt;
                md[i].tx_pps = (double)d_txp / dt; md[i].rx_pps = (double)d_rxp / dt;
                md[i].prev_tx_data = c_txB; md[i].prev_rx_data = c_rxB; md[i].prev_tx_pkts=c_txp; md[i].prev_rx_pkts=c_rxp;
                if (md[i].hist_len < 4096) { md[i].rx_hist[md[i].hist_len]=md[i].rx_Bps; md[i].tx_hist[md[i].hist_len]=md[i].tx_Bps; md[i].hist_len++; }
                else { memmove(md[i].rx_hist, md[i].rx_hist+1, sizeof(double)*4095); memmove(md[i].tx_hist, md[i].tx_hist+1, sizeof(double)*4095); md[i].rx_hist[4095]=md[i].rx_Bps; md[i].tx_hist[4095]=md[i].tx_Bps; }
            }
            prev_t = nowt;
        }
        // Header (avoid full-screen erase to reduce flicker)
        int maxy = getmaxy(stdscr), maxx = getmaxx(stdscr);
        mvhline(0, 0, ' ', maxx);
        if (use_colors) attron(COLOR_PAIR(10));
        const char *mode_str = (view==VIEW_PLOT?"PLOT":(view==VIEW_DATA?"DATA":"INFO"));
        mvprintw(0,2," ibmon - multi-device (%d) [%s] [q:quit u:units p:pause d:data i:info] ", ndev, mode_str);
        if (use_colors) attroff(COLOR_PAIR(10));
        int hdr_h = 1;
        // Grid
        int cols = (int)ceil(sqrt((double)ndev)); if (cols < 1) cols = 1; int rows = (ndev + cols - 1)/cols;
        int cell_h = (maxy - hdr_h) / rows; if (cell_h < 6) cell_h = 6;
        int cell_w = (maxx) / cols; if (cell_w < 20) cell_w = 20;
        for (int i = 0; i < ndev; ++i) {
            int r = i / cols, c = i % cols;
            int y = hdr_h + r * cell_h; int h = (r == rows-1) ? (maxy - y) : cell_h;
            int x = c * cell_w; int w = (c == cols-1) ? (maxx - x) : cell_w;
            if (!md[i].win) md[i].win = newwin(h, w, y, x);
            else { int ch, cw; getmaxyx(md[i].win, ch, cw); if (ch != h || cw != w) { delwin(md[i].win); md[i].win = newwin(h, w, y, x);} }
            if (view == VIEW_PLOT)
                draw_device_pane(md[i].win, md[i].name, &md[i], opt->units, use_colors, false);
            else if (view == VIEW_DATA)
                draw_device_data_pane(md[i].win, md[i].name, &md[i], use_colors);
            else
                draw_device_info_pane(md[i].win, md[i].name, use_colors);
        }
        doupdate();
        if (opt->duration > 0 && (now_monotonic() - start_time) >= opt->duration) break;
    }
    for (int i=0;i<ndev;++i){ if (md[i].win) delwin(md[i].win); free_counters(&md[i].ctrs);} free(md);
    endwin();
    return 0;
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
    opt.bg_mode = 0;

    static struct option long_opts[] = {
        {"device", required_argument, 0, 'd'},
        {"port", required_argument, 0, 'p'},
        {"interval", required_argument, 0, 'i'},
        {"units", required_argument, 0, 'u'},
        {"bg", required_argument, 0, 1004},
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
            case 1004:
                if (strcasecmp(optarg, "black") == 0) opt.bg_mode = 0;
                else if (strcasecmp(optarg, "terminal") == 0) opt.bg_mode = 1;
                else { fprintf(stderr, "Invalid --bg: %s (use black|terminal)\n", optarg); return 2; }
                break;
            default: usage(argv[0]); return 2;
        }
    }

    // Multi-device handling: parse list or enumerate ACTIVE devices when -d omitted
    char dev_names[64][128]; int dev_count = 0;
    if (opt.device) dev_count = parse_device_list(opt.device, dev_names, 64);
    if (!opt.device || dev_count == 0) {
        dev_count = enumerate_active_devices(dev_names, 64);
    }
    if (dev_count > 1) {
        return run_multi_mode(dev_names, dev_count, &opt);
    }
    if (!opt.device) {
        if (dev_count == 1) {
            opt.device = strdup(dev_names[0]);
        } else {
            usage(argv[0]); fprintf(stderr, "No ACTIVE InfiniBand devices found and no -d specified.\n");
            return 2;
        }
    }
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
    nodelay(stdscr, FALSE);
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(0);
    bool use_colors = false;
    if (has_colors()) {
        start_color();
        use_colors = true;
        use_default_colors();
        int bg = (opt.bg_mode == 1 ? -1 : COLOR_BLACK);
        int fg_text = (opt.bg_mode == 1 ? -1 : COLOR_WHITE);
        int fg_border = (opt.bg_mode == 1 ? -1 : COLOR_WHITE);
        init_pair(1, COLOR_CYAN, bg);               // RX bars
        init_pair(2, COLOR_RED, bg);                // TX bars
        init_pair(10, fg_text, bg);                 // text matches terminal in 'terminal' mode
        init_pair(11, bg, bg);                      // panel bg
        init_pair(12, bg, bg);                      // header bg
        init_pair(13, fg_border, bg);               // borders
    }

    bool paused = false;
    bool data_mode = false; // 'd' toggles data page
    bool info_mode = false; // 'i' toggles info page
    double rate_gbps = parse_rate_gbps(ctrs.rate);

    uint64_t p_txB=0, p_rxB=0, p_txp=0, p_rxp=0;
    uint64_t raw_tx_data=0, raw_rx_data=0, raw_tx_pkts=0, raw_rx_pkts=0;
    if (!read_u64_file(ctrs.tx_data, &p_txB) ||
        !read_u64_file(ctrs.rx_data, &p_rxB) ||
        !read_u64_file(ctrs.tx_pkts, &p_txp) ||
        !read_u64_file(ctrs.rx_pkts, &p_rxp)) {
        endwin();
        fprintf(stderr, "Error: failed to read initial counters.\n");
        free_counters(&ctrs);
        return 1;
    }
    raw_tx_data = p_txB; raw_rx_data = p_rxB; raw_tx_pkts = p_txp; raw_rx_pkts = p_rxp;
    double prev_t = now_monotonic();
    bool first_draw = true;

    // history for graph
    enum { HIST_CAP = 4096 };
    static double rx_hist[HIST_CAP];
    static double tx_hist[HIST_CAP];
    int hist_len = 0; // number of valid samples

    // windows
    WINDOW *win_hdr = NULL, *win_rx = NULL, *win_tx = NULL, *win_other = NULL, *win_info = NULL;
    int prev_maxy = -1, prev_maxx = -1;
    // info cache
    gid_entry_t *gid_list = NULL; int gid_count = 0; double last_gid_refresh = 0.0;

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
        bool fast_switch = false;
        if (ch != ERR) {
            if (ch == 'q' || ch == 'Q') break;
            else if (ch == 'p' || ch == 'P') paused = !paused;
            else if (ch == 'u' || ch == 'U') opt.units = (opt.units == UNITS_BITS) ? UNITS_BYTES : UNITS_BITS;
            else if (ch == 'd' || ch == 'D') { data_mode = !data_mode; info_mode = false; fast_switch = true; }
            else if (ch == 'i' || ch == 'I') { info_mode = !info_mode; data_mode = false; fast_switch = true; }
        }

        static double tx_Bps=0, rx_Bps=0, tx_pps=0, rx_pps=0;
        if (!paused && !fast_switch) {
            uint64_t c_txB=0, c_rxB=0, c_txp=0, c_rxp=0;
            bool ok = read_u64_file(ctrs.tx_data, &c_txB)
                   && read_u64_file(ctrs.rx_data, &c_rxB)
                   && read_u64_file(ctrs.tx_pkts, &c_txp)
                   && read_u64_file(ctrs.rx_pkts, &c_rxp);
            double now = now_monotonic();
            double dt = now - prev_t; if (dt <= 0) dt = 1e-9;
            if (ok) {
                uint64_t d_txB = (c_txB >= p_txB) ? (c_txB - p_txB) : (c_txB + (UINT64_MAX - p_txB) + 1);
                uint64_t d_rxB = (c_rxB >= p_rxB) ? (c_rxB - p_rxB) : (c_rxB + (UINT64_MAX - p_rxB) + 1);
                uint64_t d_txp = (c_txp >= p_txp) ? (c_txp - p_txp) : (c_txp + (UINT64_MAX - p_txp) + 1);
                uint64_t d_rxp = (c_rxp >= p_rxp) ? (c_rxp - p_rxp) : (c_rxp + (UINT64_MAX - p_rxp) + 1);

                if (ctrs.data_is_words) { d_txB *= 4; d_rxB *= 4; }
                tx_Bps = (double)d_txB / dt; rx_Bps = (double)d_rxB / dt;
                tx_pps = (double)d_txp / dt; rx_pps = (double)d_rxp / dt;

                p_txB = c_txB; p_rxB = c_rxB; p_txp = c_txp; p_rxp = c_rxp; prev_t = now;
                raw_tx_data = c_txB; raw_rx_data = c_rxB; raw_tx_pkts = c_txp; raw_rx_pkts = c_rxp;
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

        // Layout: header + panels
        int maxy, maxx; getmaxyx(stdscr, maxy, maxx);
        int hdr_h = 4;
        if (maxy != prev_maxy || maxx != prev_maxx || !win_hdr) {
            if (win_hdr) { delwin(win_hdr); }
            win_hdr = newwin(hdr_h, maxx, 0, 0);
            prev_maxy = maxy; prev_maxx = maxx;
        }
        if (info_mode) {
            // single info window
            if (win_rx) { delwin(win_rx); win_rx = NULL; }
            if (win_tx) { delwin(win_tx); win_tx = NULL; }
            if (win_other) { delwin(win_other); win_other = NULL; }
            int remaining = maxy - hdr_h; if (remaining < 6) remaining = 6;
            int ch, cw;
            if (!win_info) { win_info = newwin(remaining, maxx, hdr_h, 0);
            } else { getmaxyx(win_info, ch, cw); if (ch != remaining || cw != maxx) { delwin(win_info); win_info = newwin(remaining, maxx, hdr_h, 0);} }
        } else if (!data_mode) {
            // two stacked windows: RX then TX
            int remaining = maxy - hdr_h;
            if (remaining < 6) remaining = 6;
            int rx_h = remaining / 2;
            int tx_h = remaining - rx_h;
            // remove other box if present
            if (win_other) { delwin(win_other); win_other = NULL; }
            if (win_info) { delwin(win_info); win_info = NULL; }
            // recreate RX/TX if size changed
            int ch, cw;
            if (!win_rx) {
                win_rx = newwin(rx_h, maxx, hdr_h, 0);
            } else { getmaxyx(win_rx, ch, cw); if (ch != rx_h || cw != maxx) { delwin(win_rx); win_rx = newwin(rx_h, maxx, hdr_h, 0); } }
            if (!win_tx) {
                win_tx = newwin(tx_h, maxx, hdr_h + rx_h, 0);
            } else { getmaxyx(win_tx, ch, cw); if (ch != tx_h || cw != maxx) { delwin(win_tx); win_tx = newwin(tx_h, maxx, hdr_h + rx_h, 0); } }
        } else {
            // three stacked windows: RX, TX, OTHER
            int remaining = maxy - hdr_h;
            if (remaining < 9) remaining = 9;
            int each = remaining / 3;
            int rx_h = each;
            int tx_h = each;
            int other_h = remaining - rx_h - tx_h;
            int ch, cw;
            if (win_info) { delwin(win_info); win_info = NULL; }
            if (!win_rx) { win_rx = newwin(rx_h, maxx, hdr_h, 0);
            } else { getmaxyx(win_rx, ch, cw); if (ch != rx_h || cw != maxx) { delwin(win_rx); win_rx = newwin(rx_h, maxx, hdr_h, 0); } }
            if (!win_tx) { win_tx = newwin(tx_h, maxx, hdr_h + rx_h, 0);
            } else { getmaxyx(win_tx, ch, cw); if (ch != tx_h || cw != maxx) { delwin(win_tx); win_tx = newwin(tx_h, maxx, hdr_h + rx_h, 0); } }
            if (!win_other) { win_other = newwin(other_h, maxx, hdr_h + rx_h + tx_h, 0);
            } else { getmaxyx(win_other, ch, cw); if (ch != other_h || cw != maxx) { delwin(win_other); win_other = newwin(other_h, maxx, hdr_h + rx_h + tx_h, 0); } }
        }

        // Header window
        werase(win_hdr);
        if (use_colors) wbkgd(win_hdr, COLOR_PAIR(12));
        if (use_colors) wattron(win_hdr, COLOR_PAIR(13));
        draw_ascii_box(win_hdr);
        if (use_colors) wattroff(win_hdr, COLOR_PAIR(13));
        if (use_colors) wattron(win_hdr, COLOR_PAIR(10));
        // Title and current time on same line (ASCII only)
        mvwprintw(win_hdr, 0, 2, " InfiniBand Bandwidth Monitor ");
        // Time: MonthName-DD-YYYY HH:MM:SS
        {
            time_t t = time(NULL);
            struct tm lt; localtime_r(&t, &lt);
            char tbuf[64];
            strftime(tbuf, sizeof(tbuf), "%B-%d-%Y %H:%M:%S", &lt);
            int cols_hdr = getmaxx(win_hdr);
            int col = cols_hdr - (int)strlen(tbuf) - 2; if (col < 2) col = 2;
            mvwprintw(win_hdr, 0, col, "%s", tbuf);
        }
        mvwprintw(win_hdr, 1, 2, "%s port %d  [q:quit p:pause u:units]",
                  opt.device, opt.port);
        mvwprintw(win_hdr, 2, 2, "Interval: %.0f ms   Units: %s",
                  opt.interval*1000.0, (opt.units == UNITS_BITS) ? "bits" : "bytes");
        if (ctrs.link_layer) mvwprintw(win_hdr, 1, maxx/2, "Link: %s", ctrs.link_layer);
        if (ctrs.rate) mvwprintw(win_hdr, 2, maxx/2, "Rate: %s", ctrs.rate);
        if (paused) mvwprintw(win_hdr, 1, maxx-12, "[PAUSED]");
        if (data_mode) mvwprintw(win_hdr, 0, 32, "[DATA]");
        if (info_mode) mvwprintw(win_hdr, 0, 40, "[INFO]");
        if (use_colors) wattroff(win_hdr, COLOR_PAIR(10));
        wnoutrefresh(win_hdr);

        if (info_mode) {
            // refresh gid list once per second
            double nowt = now_monotonic();
            if ((nowt - last_gid_refresh) > 1.0 || gid_list == NULL) {
                free_gid_list(gid_list, gid_count);
                gid_list = NULL; gid_count = 0;
                fetch_gid_list(opt.device, opt.port, &gid_list, &gid_count);
                last_gid_refresh = nowt;
            }
            werase(win_info);
            if (use_colors) { wbkgd(win_info, COLOR_PAIR(11)); wattron(win_info, COLOR_PAIR(13)); }
            box(win_info, 0, 0);
            if (use_colors) { wattroff(win_info, COLOR_PAIR(13)); wattron(win_info, COLOR_PAIR(10)); }
            mvwprintw(win_info, 0, 2, " GID Table (non-zero) ");
            mvwprintw(win_info, 1, 2, "Idx  Type        Ndev              GID");
            int wy, wx; getmaxyx(win_info, wy, wx);
            int row = 2;
            for (int i = 0; i < gid_count && row < wy-1; ++i) {
                char line[512];
                snprintf(line, sizeof(line), "%3d  %-10s  %-16s  %s", gid_list[i].idx,
                         gid_list[i].type ? gid_list[i].type : "",
                         gid_list[i].ndev ? gid_list[i].ndev : "",
                         gid_list[i].gid ? gid_list[i].gid : "");
                mvwprintw(win_info, row++, 2, "%.*s", wx-4, line);
            }
            if (use_colors) wattroff(win_info, COLOR_PAIR(10));
            wnoutrefresh(win_info);
        } else if (!data_mode) {
            // Draw RX/TX graph panels
            draw_panel_win(win_rx, "RX", rx_Bps, rx_pps, rx_hist, hist_len, opt.units, rate_gbps, use_colors, false);
            draw_panel_win(win_tx, "TX", tx_Bps, tx_pps, tx_hist, hist_len, opt.units, rate_gbps, use_colors, false);
        } else {
            // Draw raw counters panels
            // RX panel
            werase(win_rx);
            if (use_colors) {
                wbkgd(win_rx, COLOR_PAIR(11));
                wattron(win_rx, COLOR_PAIR(13));
            }
            box(win_rx, 0, 0);
            if (use_colors) {
                wattroff(win_rx, COLOR_PAIR(13));
                wattron(win_rx, COLOR_PAIR(10));
            }
            mvwprintw(win_rx, 0, 2, " RX Raw Counters ");
            mvwprintw(win_rx, 1, 2, "port_rcv_data:    %20" PRIu64 " %s", raw_rx_data, ctrs.data_is_words ? "(words)" : "" );
            mvwprintw(win_rx, 2, 2, "port_rcv_packets: %20" PRIu64, raw_rx_pkts);
            if (!fast_switch && ctrs.rx_errors) {
                uint64_t v; if (read_u64_file(ctrs.rx_errors, &v)) mvwprintw(win_rx, 3, 2, "port_rcv_errors: %20" PRIu64, v);
            }
            if (!fast_switch && ctrs.rx_remote_phy_err) {
                uint64_t v; if (read_u64_file(ctrs.rx_remote_phy_err, &v)) mvwprintw(win_rx, 4, 2, "rcv_remote_phy:   %20" PRIu64, v);
            }
            if (!fast_switch && ctrs.rx_switch_relay_err) {
                uint64_t v; if (read_u64_file(ctrs.rx_switch_relay_err, &v)) mvwprintw(win_rx, 5, 2, "rcv_switch_relay: %20" PRIu64, v);
            }
            if (use_colors) wattroff(win_rx, COLOR_PAIR(10));
            wnoutrefresh(win_rx);

            // TX panel
            werase(win_tx);
            if (use_colors) {
                wbkgd(win_tx, COLOR_PAIR(11));
                wattron(win_tx, COLOR_PAIR(13));
            }
            box(win_tx, 0, 0);
            if (use_colors) {
                wattroff(win_tx, COLOR_PAIR(13));
                wattron(win_tx, COLOR_PAIR(10));
            }
            mvwprintw(win_tx, 0, 2, " TX Raw Counters ");
            mvwprintw(win_tx, 1, 2, "port_xmit_data:   %20" PRIu64 " %s", raw_tx_data, ctrs.data_is_words ? "(words)" : "" );
            mvwprintw(win_tx, 2, 2, "port_xmit_packets:%20" PRIu64, raw_tx_pkts);
            if (!fast_switch && ctrs.tx_discards) {
                uint64_t v; if (read_u64_file(ctrs.tx_discards, &v)) mvwprintw(win_tx, 3, 2, "xmit_discards:    %20" PRIu64, v);
            }
            if (!fast_switch && ctrs.tx_wait) {
                uint64_t v; if (read_u64_file(ctrs.tx_wait, &v)) mvwprintw(win_tx, 4, 2, "xmit_wait:        %20" PRIu64, v);
            }
            if (use_colors) wattroff(win_tx, COLOR_PAIR(10));
            wnoutrefresh(win_tx);

            // OTHER panel
            werase(win_other);
            if (use_colors) {
                wbkgd(win_other, COLOR_PAIR(11));
                wattron(win_other, COLOR_PAIR(13));
            }
            box(win_other, 0, 0);
            if (use_colors) {
                wattroff(win_other, COLOR_PAIR(13));
                wattron(win_other, COLOR_PAIR(10));
            }
            mvwprintw(win_other, 0, 2, " Other Counters ");
            int rowo = 1;
            if (!fast_switch && ctrs.local_phy_errors) { uint64_t v; if (read_u64_file(ctrs.local_phy_errors, &v)) { mvwprintw(win_other, rowo++, 2, "local_phy_errors: %20" PRIu64, v);} }
            if (!fast_switch && ctrs.symbol_error) { uint64_t v; if (read_u64_file(ctrs.symbol_error, &v)) { mvwprintw(win_other, rowo++, 2, "symbol_error:     %20" PRIu64, v);} }
            if (!fast_switch && ctrs.link_error_recovery) { uint64_t v; if (read_u64_file(ctrs.link_error_recovery, &v)) { mvwprintw(win_other, rowo++, 2, "link_err_recov:   %20" PRIu64, v);} }
            if (!fast_switch && ctrs.link_downed) { uint64_t v; if (read_u64_file(ctrs.link_downed, &v)) { mvwprintw(win_other, rowo++, 2, "link_downed:      %20" PRIu64, v);} }
            if (!fast_switch && ctrs.vl15_dropped) { uint64_t v; if (read_u64_file(ctrs.vl15_dropped, &v)) { mvwprintw(win_other, rowo++, 2, "vl15_dropped:     %20" PRIu64, v);} }
            if (!fast_switch && ctrs.excessive_buf_overrun) { uint64_t v; if (read_u64_file(ctrs.excessive_buf_overrun, &v)) { mvwprintw(win_other, rowo++, 2, "excess_buf_over:  %20" PRIu64, v);} }
            if (use_colors) wattroff(win_other, COLOR_PAIR(10));
            wnoutrefresh(win_other);
        }

        doupdate();
        if (first_draw) { timeout((int)(opt.interval * 1000)); first_draw = false; }

        // sleep remaining time
        double elapsed = now_monotonic() - loop_start;
        double to_sleep = fast_switch ? 0.0 : (opt.interval - elapsed);
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
    if (win_other) delwin(win_other);
    if (win_info) delwin(win_info);
    endwin();
    if (csv) fclose(csv);
    free_gid_list(gid_list, gid_count);
    free_counters(&ctrs);
    return 0;
}
