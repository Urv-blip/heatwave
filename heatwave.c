/*
 * =====================================================================
 *  AI-POWERED HEATWAVE EARLY WARNING DASHBOARD
 *  Data Structures Lab - Mini Project (written entirely in C)
 * =====================================================================
 *
 *  The C program is BOTH the data-structure engine AND a tiny web server.
 *  The browser (index.html) only displays what the C program computes.
 *
 *  DATA STRUCTURES USED
 *  ---------------------------------------------------------------
 *  1. Circular Queue  - buffers incoming weather readings (FIFO),
 *                       with overflow handling when the buffer is full
 *  2. Linked List     - stores the most recent processed observations
 *                       (insert at head, delete from tail)
 *  3. Priority Queue  - max-heap of alerts, most severe is served first
 *  4. Sorting         - Bubble / Insertion / Quick sort to rank cities
 *  5. Searching       - Binary search (sorted array) vs Linear search
 *
 *  "AI" PART: a rule-based expert system. It computes the Heat Index
 *  (Rothfusz regression used by the US National Weather Service) from
 *  temperature + humidity and classifies risk using IMD-style thresholds.
 *
 *  BUILD & RUN
 *    Linux / macOS / WSL :  gcc heatwave.c -o heatwave -lm && ./heatwave
 *    Windows (MinGW)     :  gcc heatwave.c -o heatwave.exe -lws2_32 -lm
 *  Then open  http://localhost:8080  in your browser.
 *  (index.html must be in the same folder as the program.)
 * =====================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <stdarg.h>

#ifndef HW_WASM   /* native web-server build only */
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define CLOSESOCK closesocket
#else
  #include <unistd.h>
  #include <signal.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <sys/time.h>
  typedef int sock_t;
  #define CLOSESOCK close
  #define INVALID_SOCKET (-1)
#endif
#endif

#define PORT         8080
#define NUM_STATIONS 12
#define QUEUE_CAP    8      /* circular queue size            */
#define MAX_RECENT   10     /* linked list keeps last N       */
#define HEAP_CAP     64     /* max pending alerts             */

/* ------------------------------------------------------------------ */
/*  Basic records                                                      */
/* ------------------------------------------------------------------ */
typedef struct {            /* one raw sensor reading            */
    int    station;
    double temp, hum;
    long   ts;
} Reading;

typedef struct {            /* one weather station / city        */
    int    id;
    char   name[24], state[4];
    double base_temp, base_hum;     /* used by the simulator */
    double temp, hum, hi, eff;      /* latest values         */
    int    risk;
    long   updated;
    int    readings;
} Station;

static const char *RISK_NAME[4] = { "Safe", "Caution", "Danger", "Extreme" };

/* Kept in ALPHABETICAL order so that binary search works on it. */
static Station g_st[NUM_STATIONS] = {
    {0,"Ahmedabad","GJ",41.0,30},{0,"Bengaluru","KA",30.0,50},
    {0,"Bhopal","MP",40.0,28},   {0,"Chennai","TN",34.0,60},
    {0,"Delhi","DL",42.0,25},    {0,"Hyderabad","TS",39.0,40},
    {0,"Jaipur","RJ",41.5,25},   {0,"Kolkata","WB",32.0,62},
    {0,"Lucknow","UP",41.0,35},  {0,"Mumbai","MH",31.0,65},
    {0,"Nagpur","MH",43.0,22},   {0,"Pune","MH",38.0,35}
};

/* ------------------------------------------------------------------ */
/*  1. CIRCULAR QUEUE  (incoming readings buffer)                      */
/* ------------------------------------------------------------------ */
typedef struct {
    Reading buf[QUEUE_CAP];
    int head;     /* index of the oldest item (next to dequeue)  */
    int tail;     /* index where the next item will be written   */
    int count;
} CQueue;

static CQueue g_q;

static int cq_full(const CQueue *q)  { return q->count == QUEUE_CAP; }
static int cq_empty(const CQueue *q) { return q->count == 0; }

static int cq_enqueue(CQueue *q, Reading r) {
    if (cq_full(q)) return 0;                 /* OVERFLOW -> caller handles */
    q->buf[q->tail] = r;
    q->tail = (q->tail + 1) % QUEUE_CAP;      /* wrap around */
    q->count++;
    return 1;
}

static int cq_dequeue(CQueue *q, Reading *out) {
    if (cq_empty(q)) return 0;                /* UNDERFLOW */
    *out = q->buf[q->head];
    q->head = (q->head + 1) % QUEUE_CAP;
    q->count--;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  2. SINGLY LINKED LIST  (recent observations)                       */
/* ------------------------------------------------------------------ */
typedef struct Node {
    int    station;
    double temp, hum, hi;
    int    risk;
    long   ts;
    struct Node *next;
} Node;

static Node *g_recent = NULL;
static int   g_recent_size = 0;

static void ll_insert_head(int st, double t, double h, double hi, int risk, long ts) {
    Node *n = (Node *)malloc(sizeof(Node));
    n->station = st; n->temp = t; n->hum = h; n->hi = hi; n->risk = risk; n->ts = ts;
    n->next = g_recent;
    g_recent = n;
    g_recent_size++;
    if (g_recent_size > MAX_RECENT) {         /* delete the last node */
        Node *p = g_recent;
        while (p->next && p->next->next) p = p->next;
        free(p->next);
        p->next = NULL;
        g_recent_size--;
    }
}

static void ll_clear(void) {
    while (g_recent) { Node *t = g_recent; g_recent = t->next; free(t); }
    g_recent_size = 0;
}

/* ------------------------------------------------------------------ */
/*  3. PRIORITY QUEUE  (max-heap of alerts)                            */
/* ------------------------------------------------------------------ */
typedef struct {
    int    id, station, risk;
    double eff;               /* priority = effective temperature */
    long   ts;
} Alert;

typedef struct { Alert a[HEAP_CAP]; int n; } Heap;

static Heap g_heap;
static int  g_next_alert_id = 1;

/* true if x must be served BEFORE y */
static int higher(const Alert *x, const Alert *y) {
    if (x->eff != y->eff) return x->eff > y->eff;
    return x->id < y->id;                     /* tie: older alert first */
}

static int heap_push(Heap *h, Alert x) {
    if (h->n == HEAP_CAP) return 0;
    int i = h->n++;
    h->a[i] = x;
    while (i > 0) {                           /* sift up */
        int p = (i - 1) / 2;
        if (higher(&h->a[i], &h->a[p])) {
            Alert t = h->a[i]; h->a[i] = h->a[p]; h->a[p] = t;
            i = p;
        } else break;
    }
    return 1;
}

static int heap_pop(Heap *h, Alert *out) {
    if (h->n == 0) return 0;
    *out = h->a[0];
    h->a[0] = h->a[--h->n];
    int i = 0;
    for (;;) {                                /* sift down */
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < h->n && higher(&h->a[l], &h->a[m])) m = l;
        if (r < h->n && higher(&h->a[r], &h->a[m])) m = r;
        if (m == i) break;
        Alert t = h->a[i]; h->a[i] = h->a[m]; h->a[m] = t;
        i = m;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  4. SORTING  (rank cities)                                          */
/* ------------------------------------------------------------------ */
static long g_cmp;                 /* comparison counter (shown in the UI) */
static int  g_sort_by_name = 0;    /* 0 = by heat (descending), 1 = by name */

static int ci_cmp(const char *a, const char *b) {   /* case-insensitive strcmp */
    while (*a && *b) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y) return x - y;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* true if a should come BEFORE b in the chosen ordering */
static int before(const Station *a, const Station *b) {
    g_cmp++;
    if (g_sort_by_name) return ci_cmp(a->name, b->name) < 0;
    return a->eff > b->eff;
}

static void bubble_sort(Station *s, int n) {
    for (int i = 0; i < n - 1; i++) {
        int swapped = 0;
        for (int j = 0; j < n - 1 - i; j++)
            if (before(&s[j + 1], &s[j])) {
                Station t = s[j]; s[j] = s[j + 1]; s[j + 1] = t; swapped = 1;
            }
        if (!swapped) break;
    }
}

static void insertion_sort(Station *s, int n) {
    for (int i = 1; i < n; i++) {
        Station key = s[i];
        int j = i - 1;
        while (j >= 0 && before(&key, &s[j])) { s[j + 1] = s[j]; j--; }
        s[j + 1] = key;
    }
}

static void quick_sort(Station *s, int lo, int hi) {
    if (lo >= hi) return;
    Station pivot = s[hi];
    int i = lo - 1;
    for (int j = lo; j < hi; j++)
        if (before(&s[j], &pivot)) { i++; Station t = s[i]; s[i] = s[j]; s[j] = t; }
    Station t = s[i + 1]; s[i + 1] = s[hi]; s[hi] = t;
    quick_sort(s, lo, i);
    quick_sort(s, i + 2, hi);
}

/* ------------------------------------------------------------------ */
/*  5. SEARCHING                                                       */
/* ------------------------------------------------------------------ */
/* Binary search on g_st (alphabetical). Returns index or -1. */
static int binary_search(const char *key, int *cmps) {
    int lo = 0, hi = NUM_STATIONS - 1;
    *cmps = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c;
        (*cmps)++;
        c = ci_cmp(key, g_st[mid].name);
        if (c == 0) return mid;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

static int ci_contains(const char *hay, const char *needle) {
    size_t n = strlen(needle), h = strlen(hay);
    if (n == 0) return 1;
    for (size_t i = 0; i + n <= h; i++) {
        size_t k = 0;
        while (k < n && tolower((unsigned char)hay[i + k]) == tolower((unsigned char)needle[k])) k++;
        if (k == n) return 1;
    }
    return 0;
}

/* Linear search: partial match on city name or state code. */
static int linear_search(const char *key, int *out, int *cmps) {
    int found = 0;
    *cmps = 0;
    for (int i = 0; i < NUM_STATIONS; i++) {
        (*cmps)++;
        if (ci_contains(g_st[i].name, key) || ci_cmp(g_st[i].state, key) == 0)
            out[found++] = i;
    }
    return found;
}

/* ------------------------------------------------------------------ */
/*  "AI" - rule-based heat risk engine                                 */
/* ------------------------------------------------------------------ */
static double heat_index_c(double tc, double rh) {
    double t = tc * 9.0 / 5.0 + 32.0, hi;
    if (t < 80.0) {
        hi = 0.5 * (t + 61.0 + ((t - 68.0) * 1.2) + (rh * 0.094));
    } else {
        hi = -42.379 + 2.04901523 * t + 10.14333127 * rh
             - 0.22475541 * t * rh - 0.00683783 * t * t
             - 0.05481717 * rh * rh + 0.00122874 * t * t * rh
             + 0.00085282 * t * rh * rh - 0.00000199 * t * t * rh * rh;
        if (rh < 13 && t >= 80 && t <= 112)
            hi -= ((13 - rh) / 4.0) * sqrt((17 - fabs(t - 95.0)) / 17.0);
        else if (rh > 85 && t >= 80 && t <= 87)
            hi += ((rh - 85) / 10.0) * ((87 - t) / 5.0);
    }
    return (hi - 32.0) * 5.0 / 9.0;
}

/* Risk level from the "effective" temperature (max of air temp, heat index) */
static int classify(double eff) {
    if (eff < 35.0) return 0;      /* Safe     */
    if (eff < 40.0) return 1;      /* Caution  */
    if (eff < 45.0) return 2;      /* Danger   (IMD heatwave starts at 40 C) */
    return 3;                      /* Extreme  */
}

static void apply_reading(Station *s, double t, double h, long ts) {
    s->temp = t; s->hum = h;
    s->hi  = heat_index_c(t, h);
    s->eff = s->hi > t ? s->hi : t;
    if (s->eff > 60.0) s->eff = 60.0;          /* sanity cap: formula is unreliable beyond this */
    s->risk = classify(s->eff);
    s->updated = ts;
}

/* ------------------------------------------------------------------ */
/*  Global stats                                                       */
/* ------------------------------------------------------------------ */
static int g_processed = 0, g_dropped = 0, g_dispatched = 0;

static void init_system(void) {
    long now = (long)time(NULL);
    for (int i = 0; i < NUM_STATIONS; i++) {
        g_st[i].id = i;
        apply_reading(&g_st[i], g_st[i].base_temp, g_st[i].base_hum, now);
        g_st[i].readings = 0;
    }
    memset(&g_q, 0, sizeof g_q);
    memset(&g_heap, 0, sizeof g_heap);
    ll_clear();
    g_next_alert_id = 1;
    g_processed = g_dropped = g_dispatched = 0;
}

/* ------------------------------------------------------------------ */
/*  Operations triggered by the website                                */
/* ------------------------------------------------------------------ */
static int enqueue_reading(int st, double t, double h, char *msg, size_t ml) {
    Reading r = { st, t, h, (long)time(NULL) };
    if (!cq_enqueue(&g_q, r)) {
        g_dropped++;
        snprintf(msg, ml, "OVERFLOW: buffer full, reading from %s dropped.", g_st[st].name);
        return 0;
    }
    snprintf(msg, ml, "Enqueued %s: %.1f\xC2\xB0""C, RH %.0f%%.", g_st[st].name, t, h);
    return 1;
}

static int process_one(char *msg, size_t ml) {
    Reading r;
    if (!cq_dequeue(&g_q, &r)) { snprintf(msg, ml, "UNDERFLOW: queue is empty."); return 0; }
    Station *s = &g_st[r.station];
    apply_reading(s, r.temp, r.hum, r.ts);
    s->readings++;
    g_processed++;
    ll_insert_head(r.station, s->temp, s->hum, s->hi, s->risk, r.ts);

    int alerted = 0;
    if (s->risk >= 1) {
        Alert a = { g_next_alert_id, r.station, s->risk, s->eff, r.ts };
        if (heap_push(&g_heap, a)) { g_next_alert_id++; alerted = 1; }
    }
    snprintf(msg, ml, "Processed %s: %.1f\xC2\xB0""C (feels %.1f\xC2\xB0""C) -> %s.%s",
             s->name, s->temp, s->eff, RISK_NAME[s->risk],
             alerted ? " Alert added to priority queue." : "");
    return 1;
}

static void simulate(int n, char *msg, size_t ml) {
    int ok = 0, bad = 0;
    for (int i = 0; i < n; i++) {
        int k = rand() % NUM_STATIONS;
        double t = g_st[k].base_temp + (rand() % 90) / 10.0 - 3.0;     /* -3 .. +6 */
        double h = g_st[k].base_hum + (rand() % 21) - 10;
        if (h < 10) h = 10;
        if (h > 80) h = 80;
        char tmp[160];
        if (enqueue_reading(k, floor(t * 10) / 10.0, h, tmp, sizeof tmp)) ok++; else bad++;
    }
    snprintf(msg, ml, "Simulator sent %d readings: %d queued, %d dropped (overflow).", n, ok, bad);
}

/* ------------------------------------------------------------------ */
/*  JSON helpers                                                       */
/* ------------------------------------------------------------------ */
typedef struct { char *p; size_t len, cap; } SB;

static void sb_init(SB *s) { s->cap = 4096; s->p = (char *)malloc(s->cap); s->len = 0; s->p[0] = 0; }

static void sb_printf(SB *s, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt); va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    while (s->len + n + 1 > s->cap) { s->cap *= 2; s->p = (char *)realloc(s->p, s->cap); }
    vsnprintf(s->p + s->len, s->cap - s->len, fmt, ap2);
    va_end(ap2);
    s->len += n;
}

static void sb_str(SB *s, const char *str) {          /* JSON-escaped string */
    sb_printf(s, "\"");
    for (; *str; str++) {
        unsigned char c = (unsigned char)*str;
        if (c == '"' || c == '\\') sb_printf(s, "\\%c", c);
        else if (c < 0x20)         sb_printf(s, " ");
        else                       sb_printf(s, "%c", c);
    }
    sb_printf(s, "\"");
}

static void station_json(SB *s, const Station *x) {
    sb_printf(s, "{\"id\":%d,\"name\":\"%s\",\"state\":\"%s\",\"temp\":%.1f,\"hum\":%.0f,"
                 "\"hi\":%.1f,\"eff\":%.1f,\"risk\":%d,\"riskName\":\"%s\","
                 "\"updated\":%ld,\"readings\":%d}",
              x->id, x->name, x->state, x->temp, x->hum, x->hi, x->eff,
              x->risk, RISK_NAME[x->risk], x->updated, x->readings);
}

static void build_state(SB *s, const char *msg, int ok, const char *algo, const char *by) {
    /* ---- sort a COPY of the station table with the chosen algorithm ---- */
    Station copy[NUM_STATIONS];
    memcpy(copy, g_st, sizeof copy);
    g_sort_by_name = (strcmp(by, "name") == 0);
    g_cmp = 0;
    if      (strcmp(algo, "bubble") == 0)    bubble_sort(copy, NUM_STATIONS);
    else if (strcmp(algo, "insertion") == 0) insertion_sort(copy, NUM_STATIONS);
    else { algo = "quick";                   quick_sort(copy, 0, NUM_STATIONS - 1); }
    long cmps = g_cmp;

    sb_printf(s, "{\"ok\":%s,\"msg\":", ok ? "true" : "false");
    sb_str(s, msg);

    /* ---- stats ---- */
    int hot = 0;
    for (int i = 1; i < NUM_STATIONS; i++) if (g_st[i].eff > g_st[hot].eff) hot = i;
    sb_printf(s, ",\"stats\":{\"stations\":%d,\"queueCount\":%d,\"queueCap\":%d,"
                 "\"alerts\":%d,\"processed\":%d,\"dropped\":%d,\"dispatched\":%d,"
                 "\"hottest\":\"%s\",\"hottestEff\":%.1f}",
              NUM_STATIONS, g_q.count, QUEUE_CAP, g_heap.n, g_processed, g_dropped,
              g_dispatched, g_st[hot].name, g_st[hot].eff);

    /* ---- sorted stations ---- */
    sb_printf(s, ",\"sort\":{\"algo\":\"%s\",\"by\":\"%s\",\"comparisons\":%ld},\"stations\":[",
              algo, g_sort_by_name ? "name" : "heat", cmps);
    for (int i = 0; i < NUM_STATIONS; i++) { if (i) sb_printf(s, ","); station_json(s, &copy[i]); }
    sb_printf(s, "]");

    /* ---- circular queue ---- */
    sb_printf(s, ",\"queue\":{\"cap\":%d,\"head\":%d,\"tail\":%d,\"count\":%d,\"slots\":[",
              QUEUE_CAP, g_q.head, g_q.tail, g_q.count);
    for (int i = 0; i < QUEUE_CAP; i++) {
        int occupied = 0;
        for (int k = 0; k < g_q.count; k++) if ((g_q.head + k) % QUEUE_CAP == i) occupied = 1;
        if (i) sb_printf(s, ",");
        if (occupied)
            sb_printf(s, "{\"name\":\"%s\",\"temp\":%.1f,\"hum\":%.0f}",
                      g_st[g_q.buf[i].station].name, g_q.buf[i].temp, g_q.buf[i].hum);
        else sb_printf(s, "null");
    }
    sb_printf(s, "]}");

    /* ---- linked list ---- */
    sb_printf(s, ",\"recent\":[");
    for (Node *p = g_recent; p; p = p->next) {
        sb_printf(s, "%s{\"name\":\"%s\",\"temp\":%.1f,\"hum\":%.0f,\"hi\":%.1f,\"risk\":%d,\"riskName\":\"%s\",\"ts\":%ld}",
                  p == g_recent ? "" : ",", g_st[p->station].name, p->temp, p->hum, p->hi,
                  p->risk, RISK_NAME[p->risk], p->ts);
    }
    sb_printf(s, "]");

    /* ---- priority queue: heap array + served-order view ---- */
    sb_printf(s, ",\"heap\":[");
    for (int i = 0; i < g_heap.n; i++)
        sb_printf(s, "%s{\"v\":%.1f,\"n\":\"%s\"}", i ? "," : "", g_heap.a[i].eff, g_st[g_heap.a[i].station].name);
    sb_printf(s, "],\"alerts\":[");
    {
        Heap tmp = g_heap; Alert a; int first = 1;
        while (heap_pop(&tmp, &a)) {
            sb_printf(s, "%s{\"id\":%d,\"name\":\"%s\",\"eff\":%.1f,\"risk\":%d,\"riskName\":\"%s\",\"ts\":%ld}",
                      first ? "" : ",", a.id, g_st[a.station].name, a.eff, a.risk, RISK_NAME[a.risk], a.ts);
            first = 0;
        }
    }
    sb_printf(s, "]}");
}

/* ------------------------------------------------------------------ */
/*  Tiny HTTP layer                                                    */
/* ------------------------------------------------------------------ */
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Reads ?key=value from a query string, URL-decoded. Returns 1 if found. */
static int get_param(const char *query, const char *key, char *out, size_t outsz) {
    size_t kl = strlen(key);
    const char *p = query;
    while (*p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            p += kl + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < outsz) {
                if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
                    out[i++] = (char)(hexval(p[1]) * 16 + hexval(p[2])); p += 3;
                } else if (*p == '+') { out[i++] = ' '; p++; }
                else out[i++] = *p++;
            }
            out[i] = 0;
            return 1;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  API dispatcher - shared by the native server AND the WebAssembly   */
/*  build. Returns a malloc'd JSON string; *status is the HTTP code.   */
/* ------------------------------------------------------------------ */
static char *api_dispatch(const char *path, const char *query, int *status, size_t *outlen) {
    char msg[300] = "Dashboard refreshed.", algo[16] = "quick", by[16] = "heat", v[64];
    int ok = 1;
    *status = 200;
    get_param(query, "algo", algo, sizeof algo);
    get_param(query, "by",   by,   sizeof by);

    SB sb; sb_init(&sb);

    if (strcmp(path, "/api/search") == 0) {
        char q[64] = ""; int idx[NUM_STATIONS], c1 = 0, c2 = 0;
        get_param(query, "q", q, sizeof q);
        int b = (q[0]) ? binary_search(q, &c1) : -1;
        int nl = q[0] ? linear_search(q, idx, &c2) : 0;
        sb_printf(&sb, "{\"q\":"); sb_str(&sb, q);
        sb_printf(&sb, ",\"binary\":{\"found\":%s,\"comparisons\":%d,\"station\":", b >= 0 ? "true" : "false", c1);
        if (b >= 0) station_json(&sb, &g_st[b]); else sb_printf(&sb, "null");
        sb_printf(&sb, "},\"linear\":{\"comparisons\":%d,\"matches\":[", c2);
        for (int i = 0; i < nl; i++) { if (i) sb_printf(&sb, ","); station_json(&sb, &g_st[idx[i]]); }
        sb_printf(&sb, "]}}");
    } else {
        if (strcmp(path, "/api/add") == 0) {
            char cs[16] = "", ts[16] = "", hs[16] = "";
            get_param(query, "st", cs, sizeof cs);
            get_param(query, "temp", ts, sizeof ts);
            get_param(query, "hum", hs, sizeof hs);
            int st = atoi(cs); double t = atof(ts), h = atof(hs);
            if (!cs[0] || !ts[0] || !hs[0] || st < 0 || st >= NUM_STATIONS ||
                t < -10 || t > 60 || h < 0 || h > 100) {
                ok = 0; snprintf(msg, sizeof msg, "Invalid input: temp must be -10..60 and humidity 0..100.");
            } else ok = enqueue_reading(st, t, h, msg, sizeof msg);
        } else if (strcmp(path, "/api/simulate") == 0) {
            int n = 3;
            if (get_param(query, "n", v, sizeof v)) n = atoi(v);
            if (n < 1) n = 1;
            if (n > 20) n = 20;
            simulate(n, msg, sizeof msg);
        } else if (strcmp(path, "/api/process") == 0) {
            ok = process_one(msg, sizeof msg);
        } else if (strcmp(path, "/api/processall") == 0) {
            int cnt = 0; char last[300];
            while (!cq_empty(&g_q)) { process_one(last, sizeof last); cnt++; }
            if (cnt) snprintf(msg, sizeof msg, "Processed %d readings from the queue.", cnt);
            else { ok = 0; snprintf(msg, sizeof msg, "UNDERFLOW: queue is empty."); }
        } else if (strcmp(path, "/api/dispatch") == 0) {
            Alert a;
            if (heap_pop(&g_heap, &a)) {
                g_dispatched++;
                snprintf(msg, sizeof msg, "Dispatched alert #%d: %s - %s (%.1f\xC2\xB0""C). Highest priority served first.",
                         a.id, g_st[a.station].name, RISK_NAME[a.risk], a.eff);
            } else { ok = 0; snprintf(msg, sizeof msg, "No pending alerts."); }
        } else if (strcmp(path, "/api/reset") == 0) {
            init_system(); snprintf(msg, sizeof msg, "System reset.");
        } else if (strcmp(path, "/api/state") != 0) {
            *status = 404;
            sb_printf(&sb, "{\"ok\":false,\"msg\":\"Unknown endpoint\"}");
            *outlen = sb.len;
            return sb.p;
        }
        build_state(&sb, msg, ok, algo, by);
    }
    *outlen = sb.len;
    return sb.p;
}

#ifndef HW_WASM   /* ---------- native HTTP server only ---------- */

static void send_all(sock_t c, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = (int)send(c, data + sent, (int)(len - sent), 0);
        if (n <= 0) return;
        sent += n;
    }
}

static void send_response(sock_t c, int code, const char *ctype, const char *body, size_t len) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        code, code == 200 ? "OK" : (code == 404 ? "Not Found" : "Error"), ctype, len);
    send_all(c, hdr, n);
    send_all(c, body, len);
}

static void serve_index(sock_t c) {
    FILE *f = fopen("index.html", "rb");
    if (!f) {
        const char *m = "index.html not found. Run the program from the folder that contains index.html.";
        send_response(c, 500, "text/plain", m, strlen(m));
        return;
    }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = (char *)malloc(sz + 1);
    size_t got = fread(buf, 1, sz, f);
    fclose(f);
    send_response(c, 200, "text/html", buf, got);
    free(buf);
}

static void handle_api(sock_t c, const char *path, const char *query) {
    int status; size_t len;
    char *json = api_dispatch(path, query, &status, &len);
    send_response(c, status, "application/json", json, len);
    free(json);
}

static void handle_client(sock_t c) {
    char req[4096];
    int n = (int)recv(c, req, sizeof req - 1, 0);
    if (n <= 0) return;
    req[n] = 0;

    char method[8], target[1024];
    if (sscanf(req, "%7s %1023s", method, target) != 2) return;
    char *q = strchr(target, '?');
    const char *query = "";
    if (q) { *q = 0; query = q + 1; }

    printf("[%s] %s%s%s\n", method, target, q ? "?" : "", query);

    if (strcmp(target, "/") == 0 || strcmp(target, "/index.html") == 0) serve_index(c);
    else if (strncmp(target, "/api/", 5) == 0) handle_api(c, target, query);
    else { const char *m = "404 Not Found"; send_response(c, 404, "text/plain", m, strlen(m)); }
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main(void) {
    srand((unsigned)time(NULL));
    init_system();

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "WSAStartup failed\n"); return 1; }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    /* Cloud hosts give the port in the PORT environment variable and need
       the server to listen on all interfaces. Locally: 8080, localhost only. */
    const char *penv = getenv("PORT");
    int port = penv ? atoi(penv) : PORT;
    addr.sin_addr.s_addr = htonl(penv ? INADDR_ANY : INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind (is the port already in use?)"); return 1; }
    if (listen(srv, 16) < 0) { perror("listen"); return 1; }

    printf("=====================================================\n");
    printf("  Heatwave Early Warning Dashboard (C backend)\n");
    printf("  Open  http://localhost:%d  in your browser\n", port);
    printf("  Press Ctrl+C to stop\n");
    printf("=====================================================\n");

    for (;;) {
        sock_t c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
#ifdef _WIN32
        DWORD tmo = 2000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
#else
        struct timeval tv = { 2, 0 }; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
        handle_client(c);
        CLOSESOCK(c);
    }
}

#else  /* HW_WASM ------------------------------------------------------ */

/* WebAssembly build: the browser calls these two functions directly.   */
static char *g_last = NULL;

__attribute__((export_name("hw_init")))
void hw_init(unsigned seed) { srand(seed); init_system(); }

/* Same routes as the HTTP server, e.g. api_call("/api/add", "st=3&temp=44&hum=30") */
__attribute__((export_name("api_call")))
const char *api_call(const char *path, const char *query) {
    int status; size_t len;
    free(g_last);
    g_last = api_dispatch(path, query, &status, &len);
    return g_last;
}

/* lets JS allocate strings inside wasm memory */
__attribute__((export_name("hw_alloc"))) void *hw_alloc(size_t n) { return malloc(n); }
__attribute__((export_name("hw_free")))  void  hw_free(void *p)   { free(p); }

#endif
