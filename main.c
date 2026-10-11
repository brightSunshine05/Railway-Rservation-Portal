/*
 * Railway Reservation System - OS Synchronization Project  (v2)
 * ----------------------------------------------------------------------
 *  - Group berth allotment (family seats together, 1AC / 2AC / 3AC,
 *    lower / middle / upper / side berths, auto-allocated at ticket time)
 *  - "Seats not available together" fallback + ticket note
 *  - Cancellation with time-based refund
 *  - Three websites on one backend: Railway Portal (main) + two partner
 *    sites (MakeMyTrip-style, Goibibo-style). Partner bookings get a
 *    discount, are stored in the main database and are served with PRIORITY.
 *
 * OS concepts: threads, mutex, readers-writer lock, counting semaphores,
 * bounded priority queue (producer-consumer) with aging (anti-starvation),
 * critical section, race-condition demo (--unsafe), seat audit.
 *
 * Build : gcc main.c -o railway -pthread
 * Run   : ./railway serve              (websites on http://localhost:8080)
 *         ./railway simulate           (console stress test, safe mode)
 *         ./railway simulate --unsafe  (console race-condition demo)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef MSG_NOSIGNAL            /* macOS has no MSG_NOSIGNAL; SIGPIPE is ignored in main() instead */
#define MSG_NOSIGNAL 0
#endif


/* ------------------------------------------------------------------ */
/*  Portable counting semaphore (mutex + condition variable).          */
/*  macOS does not support unnamed POSIX semaphores (sem_init), so we   */
/*  build the same semaphore from pthread primitives. Works everywhere. */
/* ------------------------------------------------------------------ */
typedef struct { pthread_mutex_t m; pthread_cond_t c; int v; } PSem;
static void psem_init(PSem *s, int v) { pthread_mutex_init(&s->m, NULL); pthread_cond_init(&s->c, NULL); s->v = v; }
static void psem_wait(PSem *s) {                      /* P / down: sleep while the count is 0 */
    pthread_mutex_lock(&s->m);
    while (s->v <= 0) pthread_cond_wait(&s->c, &s->m);
    s->v--;
    pthread_mutex_unlock(&s->m);
}
static void psem_post(PSem *s) {                      /* V / up: wake one sleeper */
    pthread_mutex_lock(&s->m);
    s->v++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
}

#define MAX_COACHES   3
#define MAX_BERTHS    72
#define MAX_GROUP     6
#define MAX_BOOKINGS  5000
#define MAX_JOURNEYS  256    /* one journey = one train on one date; each has its own berth map */
#define MAX_STOPS     8
#define MAX_TRAINS    32
#define QUEUE_CAP     128
#define MAX_WORKERS   16
#define AGING_LIMIT   3      /* after 3 partner jobs in a row, serve 1 waiting direct job */
#ifndef HOLD_SECONDS
#define HOLD_SECONDS  180    /* seats are held this long while the passenger chooses meals */
#endif
#define ADVANCE_DAYS  60     /* tickets can be booked up to 60 days ahead */
#define REQ_CAP       16384

/* ------------------------------------------------------------------ */
/*  Static railway data                                                */
/* ------------------------------------------------------------------ */
enum { CL_1A, CL_2A, CL_3A };
enum { CH_DIRECT, CH_MMT, CH_GOI };
enum { T_LB, T_MB, T_UB, T_SL, T_SU, T_SB };
enum { ST_FREE = 0, ST_CONFIRMED = 1, ST_CANCELLED = 2, ST_HELD = 3, ST_EXPIRED = 4 };
enum { JOB_BOOK, JOB_CONFIRM, JOB_RELEASE, JOB_CANCEL, JOB_STOP };

/* fare = distance x rate (per 100 km), but never below min_fare */
typedef struct { const char *code, *name; int ncoaches, bays, bay_size, rate, min_fare, flat_cancel; char prefix; } ClassInfo;
typedef struct { const char *code, *name; int discount_pct, priority; } ChannelInfo;

static const ClassInfo CLASSES[3] = {
    { "1A", "First AC",  1, 6, 4, 150, 450, 240, 'H' },   /* 6 cabins x 4 single berths  = 24 */
    { "2A", "Second AC", 2, 8, 6,  90, 260, 200, 'A' },   /* 8 bays x 6 berths x 2 coaches = 96 */
    { "3A", "Third AC",  3, 9, 8,  60, 190, 180, 'B' },   /* 9 bays x 8 berths x 3 coaches = 216 */
};
static const ChannelInfo CHANNELS[3] = {
    { "DIRECT", "Railway Portal", 0,  2 },
    { "MMT",    "MakeMyTrip",    10,  1 },            /* priority 1 = served first */
    { "GOI",    "Goibibo",       12,  1 },
};
static const char *CHANNEL_KEYS[3] = { "", "MMT-KEY-2026", "GOI-KEY-2026" };
static const char *TYPE_CODE[] = { "LB", "MB", "UB", "SL", "SU", "SB" };
static const char *TYPE_NAME[] = { "Lower", "Middle", "Upper", "Side Lower", "Side Upper", "Single" };

/* ---- meals (ordered per passenger, delivered to the seat number) ---- */
typedef struct { const char *code, *name; int price; } Meal;
static const Meal MEALS[] = {
    { "NONE", "No meal",            0 },
    { "VEG",  "Veg Meal",         180 },
    { "NVG",  "Non-Veg Meal",     220 },
    { "JAIN", "Jain Meal",        190 },
    { "SNK",  "Snack Box",         90 },
};
#define NMEALS ((int)(sizeof MEALS / sizeof MEALS[0]))

/* ---- stations ---- */
typedef struct { const char *code, *name; } Station;
static const Station STATIONS[] = {
    { "MAS", "Chennai Central" },   { "KPD", "Katpadi Jn" },        { "SBC", "KSR Bengaluru City" },
    { "MYS", "Mysuru Jn" },         { "CBE", "Coimbatore Jn" },     { "MDU", "Madurai Jn" },
    { "TVC", "Thiruvananthapuram Central" }, { "SC", "Secunderabad Jn" }, { "BZA", "Vijayawada Jn" },
    { "NGP", "Nagpur Jn" },         { "BPL", "Bhopal Jn" },         { "NDLS", "New Delhi" },
    { "CSMT", "Mumbai CSMT" },      { "PUNE", "Pune Jn" },          { "HWH", "Howrah Jn" },
};
#define NSTATIONS ((int)(sizeof STATIONS / sizeof STATIONS[0]))
static int station_index(const char *code) {
    for (int i = 0; i < NSTATIONS; i++) if (!strcasecmp(code, STATIONS[i].code)) return i;
    return -1;
}

/* ---- trains: every base train also gets a RETURN train (reverse route) ---- */
typedef struct {
    int no, rno; const char *name; int dep_h, dep_m, rdep_h, rdep_m, n;
    const char *code[MAX_STOPS]; int km[MAX_STOPS], mins[MAX_STOPS]; int cmask;   /* cmask: bit per class */
} BaseTrain;
#define ALLC ((1 << CL_1A) | (1 << CL_2A) | (1 << CL_3A))
#define NO1A ((1 << CL_2A) | (1 << CL_3A))
/* Sample (simulated) timetable data for the project - numbers/timings are not the real railway timetable. */
static const BaseTrain BASE[] = {
    { 12345, 12346, "Chennai Express",   22,  0, 21, 30, 6, { "MAS","KPD","BZA","NGP","BPL","NDLS" }, { 0,126,432,1100,1370,2180 }, { 0,150,470,1280,1580,2400 }, ALLC },
    { 12607, 12608, "Bengaluru Express",  6,  0, 15,  0, 3, { "MAS","KPD","SBC" },                     { 0,126,350 },                { 0,120,300 },                 NO1A },
    { 16021, 16022, "Mysuru Superfast",  21, 15, 20,  0, 4, { "MAS","KPD","SBC","MYS" },               { 0,126,350,500 },            { 0,135,330,540 },             ALLC },
    { 12633, 12634, "Southern Mail",     19, 15, 18, 30, 3, { "MAS","MDU","TVC" },                     { 0,497,920 },                { 0,500,900 },                 ALLC },
    { 12675, 12676, "Kovai Express",      6, 15, 14, 15, 3, { "MAS","KPD","CBE" },                     { 0,126,500 },                { 0,130,480 },                 NO1A },
    { 11041, 11042, "Mumbai Mail",       20,  0, 19, 30, 4, { "MAS","KPD","PUNE","CSMT" },             { 0,126,1160,1280 },          { 0,150,1240,1390 },           ALLC },
    { 12839, 12840, "Howrah Mail",       23, 45, 22, 15, 3, { "MAS","BZA","HWH" },                     { 0,432,1660 },              { 0,470,1560 },                ALLC },
    { 12759, 12760, "Charminar Express", 18, 10, 18,  0, 3, { "MAS","BZA","SC" },                      { 0,432,700 },                { 0,470,740 },                 NO1A },
    { 12951, 12952, "Rajdhani Express",  16, 30, 17,  0, 3, { "NDLS","BPL","CSMT" },                   { 0,700,1385 },               { 0,600,960 },                 ALLC },
};
typedef struct { int no; const char *name; int dep_h, dep_m, n, stn[MAX_STOPS], km[MAX_STOPS], mins[MAX_STOPS], cmask; } Train;
static Train trains[MAX_TRAINS];
static int ntrains = 0;

static void build_trains(void) {
    ntrains = 0;
    for (size_t b = 0; b < sizeof BASE / sizeof BASE[0]; b++) {
        const BaseTrain *t = &BASE[b];
        Train *f = &trains[ntrains++], *r = &trains[ntrains++];
        f->no = t->no; f->name = t->name; f->dep_h = t->dep_h; f->dep_m = t->dep_m; f->n = t->n; f->cmask = t->cmask;
        r->no = t->rno; r->name = t->name; r->dep_h = t->rdep_h; r->dep_m = t->rdep_m; r->n = t->n; r->cmask = t->cmask;
        for (int i = 0; i < t->n; i++) {
            f->stn[i] = station_index(t->code[i]); f->km[i] = t->km[i]; f->mins[i] = t->mins[i];
            int k = t->n - 1 - i;                                     /* reverse route */
            r->stn[i] = station_index(t->code[k]);
            r->km[i] = t->km[t->n - 1] - t->km[k];
            r->mins[i] = t->mins[t->n - 1] - t->mins[k];
        }
    }
}
static int find_train(int no) { for (int i = 0; i < ntrains; i++) if (trains[i].no == no) return i; return -1; }
static int stop_of(const Train *t, int stn) { for (int i = 0; i < t->n; i++) if (t->stn[i] == stn) return i; return -1; }

static int berth_type(int cls, int berth) {
    int pos = (berth - 1) % CLASSES[cls].bay_size;
    static const int m2[6] = { T_LB, T_UB, T_LB, T_UB, T_SL, T_SU };
    static const int m3[8] = { T_LB, T_MB, T_UB, T_LB, T_MB, T_UB, T_SL, T_SU };
    if (cls == CL_1A) return T_SB;
    return cls == CL_2A ? m2[pos] : m3[pos];
}
static int per_coach(int cls) { return CLASSES[cls].bays * CLASSES[cls].bay_size; }
static int class_total(int cls) { return CLASSES[cls].ncoaches * per_coach(cls); }

/* ------------------------------------------------------------------ */
/*  Dates, journeys (train + date) and trips (boarding -> destination)  */
/* ------------------------------------------------------------------ */
/* A "day number" = days since 1970-01-01 of a calendar date (timezone-free arithmetic). */
static long dn_from_ymd(int y, int m, int d) {
    struct tm tm; memset(&tm, 0, sizeof tm); tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d;
    return (long)(timegm(&tm) / 86400);
}
static long dn_today(void) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    return dn_from_ymd(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}
static long dn_parse(const char *s) {                    /* "YYYY-MM-DD" -> day number, -1 if invalid */
    int y, m, d; char extra;
    if (sscanf(s, "%d-%d-%d%c", &y, &m, &d, &extra) != 3 || y < 2000 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
    long dn = dn_from_ymd(y, m, d);
    time_t t = (time_t)dn * 86400; struct tm tm; gmtime_r(&t, &tm);
    return (tm.tm_mday == d && tm.tm_mon == m - 1) ? dn : -1;      /* rejects 31 Feb etc. */
}
static void dn_fmt(long dn, char *out, size_t n) {
    time_t t = (time_t)dn * 86400; struct tm tm; gmtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%d", &tm);
}

typedef struct { int train, from, to, km; long run_dn; time_t dep, arr; int fare[3]; } Trip;   /* from/to = stop positions */

/* Fills 't' for 'train' travelling from station 'from' to 'to', boarding on calendar day 'board_dn'.
 * Returns 0 = bookable, 1 = train already departed, -1 = invalid (message in err). */
static int make_trip(int train, int from_stn, int to_stn, long board_dn, Trip *t, char *err, size_t en) {
    const Train *tr = &trains[train];
    int i = stop_of(tr, from_stn), j = stop_of(tr, to_stn);
    if (i < 0 || j < 0 || i >= j) { snprintf(err, en, "Train %d does not run from %s to %s", tr->no, STATIONS[from_stn].name, STATIONS[to_stn].name); return -1; }
    long today = dn_today();
    if (board_dn < today) { snprintf(err, en, "Travel date is in the past"); return -1; }
    if (board_dn > today + ADVANCE_DAYS) { snprintf(err, en, "Bookings open only %d days in advance", ADVANCE_DAYS); return -1; }
    int day_off = (tr->dep_h * 60 + tr->dep_m + tr->mins[i]) / 1440;      /* train may reach this stop on a later day */
    long run = board_dn - day_off;
    time_t rt = (time_t)run * 86400; struct tm d; gmtime_r(&rt, &d);
    struct tm lt; memset(&lt, 0, sizeof lt);
    lt.tm_year = d.tm_year; lt.tm_mon = d.tm_mon; lt.tm_mday = d.tm_mday; lt.tm_hour = tr->dep_h; lt.tm_min = tr->dep_m; lt.tm_isdst = -1;
    time_t origin = mktime(&lt);
    t->train = train; t->from = i; t->to = j; t->run_dn = run; t->km = tr->km[j] - tr->km[i];
    t->dep = origin + (time_t)tr->mins[i] * 60; t->arr = origin + (time_t)tr->mins[j] * 60;
    for (int c = 0; c < 3; c++) {
        int f = t->km * CLASSES[c].rate / 100; if (f < CLASSES[c].min_fare) f = CLASSES[c].min_fare;
        t->fare[c] = (f + 2) / 5 * 5;                                       /* round to nearest Rs 5 */
    }
    return t->dep <= time(NULL) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Shared state (protected by db_lock)                                */
/* ------------------------------------------------------------------ */
typedef struct { int coach, berth; } SeatRef;
typedef struct { int train; long dn; int owner[3][MAX_COACHES][MAX_BERTHS + 1]; } Journey;   /* owner: 0 = free, else PNR */
typedef struct {
    int pnr, channel, cls, pref, count, priority, rank;
    char names[MAX_GROUP][32];
    SeatRef seats[MAX_GROUP];
    int meal[MAX_GROUP];                                   /* index into MEALS, 0 = no meal */
    int together;
    int fare_total, discount, meal_total, paid;
    int status, refund, charge, hours_before;
    int train, jidx, from, to, km;                         /* from/to = station indexes */
    time_t dep, arr, booked_at, cancelled_at, expires_at;
} Booking;

static Journey journeys[MAX_JOURNEYS];
static int njourneys = 0;
static pthread_mutex_t jmutex = PTHREAD_MUTEX_INITIALIZER;     /* protects the journey table itself */
static Booking bookings[MAX_BOOKINGS];
static int nbookings = 0, next_pnr = 1000001;
static pthread_rwlock_t db_lock = PTHREAD_RWLOCK_INITIALIZER;   /* writers = mutex, readers = shared */

static int use_lock = 1, quiet = 0, delay_ms = 40, num_workers = 4, port = 8080;
static const char *web_dir = "web";
static volatile int hold_workers = 0;

/* Finds the berth map of (train, date). Creates it on first booking when create=1. Returns -1 if none. */
static int journey_find(int train, long dn, int create) {
    int r = -1;
    pthread_mutex_lock(&jmutex);
    for (int i = 0; i < njourneys; i++) if (journeys[i].train == train && journeys[i].dn == dn) { r = i; break; }
    if (r < 0 && create && njourneys < MAX_JOURNEYS) {
        memset(&journeys[njourneys], 0, sizeof journeys[0]);
        journeys[njourneys].train = train; journeys[njourneys].dn = dn;
        r = njourneys++;
    }
    pthread_mutex_unlock(&jmutex);
    return r;
}

/* statistics */
static int st_conf, st_rej, st_cancel, st_together, st_split, st_refunded, st_expired, st_meals, st_meal_rev, st_ch[3];
/* ------------------------------------------------------------------ */
/*  Activity log (ring buffer)                                         */
/* ------------------------------------------------------------------ */
#define LOG_LINES 60
static char logbuf[LOG_LINES][200];
static int log_count = 0;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void logf_(const char *fmt, ...) {
    char line[150], ts[16];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    pthread_mutex_lock(&log_mutex);
    snprintf(logbuf[log_count % LOG_LINES], sizeof logbuf[0], "%s  %s", ts, line);
    if (!quiet) puts(logbuf[log_count % LOG_LINES]);
    log_count++;
    pthread_mutex_unlock(&log_mutex);
}

/* ------------------------------------------------------------------ */
/*  Request jobs + bounded PRIORITY queue (producer-consumer)          */
/* ------------------------------------------------------------------ */
typedef struct Job {
    int type, channel, cls, pref, count, pnr, hours, hold;
    char names[MAX_GROUP][32];
    int meal[MAX_GROUP];
    Trip trip;
    int priority, rank;
    unsigned long seq;
    int ok; char msg[160]; int res_pnr;
    PSem done;
} Job;

static Job *pq[QUEUE_CAP];
static int pq_n = 0, hi_streak = 0, service_rank = 0;
static unsigned long seq_counter = 0;
static PSem q_slots, q_items;                    /* counting semaphores */
static pthread_mutex_t q_mutex = PTHREAD_MUTEX_INITIALIZER;

static void job_init(Job *j) { memset(j, 0, sizeof *j); j->hours = -1; psem_init(&j->done, 0); }
static void pq_enqueue(Job *j) {
    psem_wait(&q_slots);                           /* wait for a free slot (back-pressure) */
    pthread_mutex_lock(&q_mutex);
    j->seq = ++seq_counter;
    pq[pq_n++] = j;
    pthread_mutex_unlock(&q_mutex);
    psem_post(&q_items);                           /* announce a new request */
}

static Job *pq_dequeue(void) {
    psem_wait(&q_items);                           /* sleep until a request exists */
    while (hold_workers) usleep(1000);            /* only used by the burst demo */
    pthread_mutex_lock(&q_mutex);
    int hi = -1, lo = -1;
    for (int i = 0; i < pq_n; i++) {
        Job *j = pq[i];
        if (j->priority == 1) { if (hi < 0 || j->seq < pq[hi]->seq) hi = i; }
        else                  { if (lo < 0 || j->seq < pq[lo]->seq) lo = i; }
    }
    int best;
    if (hi >= 0 && (lo < 0 || hi_streak < AGING_LIMIT)) { best = hi; hi_streak = (lo >= 0) ? hi_streak + 1 : 0; }
    else { best = lo; hi_streak = 0; }            /* aging: stop low-priority starvation */
    Job *j = pq[best];
    pq[best] = pq[--pq_n];
    j->rank = ++service_rank;
    pthread_mutex_unlock(&q_mutex);
    psem_post(&q_slots);
    return j;
}

/* ------------------------------------------------------------------ */
/*  Berth allotment                                                    */
/* ------------------------------------------------------------------ */
static int pref_match(int cls, int berth, int pref) { return pref >= 0 && berth_type(cls, berth) == pref; }
static int bay_of(int cls, int berth) { return (berth - 1) / CLASSES[cls].bay_size; }

/*
 * Chooses n berths (does NOT mark them).  Strategy:
 *   1. Look in every coach for n free berths lying in the same compartment
 *      (or two neighbouring compartments), using as many berths of the
 *      preferred type as possible.  => seats TOGETHER.
 *   2. If none exists, scatter the passengers over different compartments /
 *      coaches (preferred berth type first)  => NOT TOGETHER.
 * Returns 0 if the class does not have n free berths at all.
 */
typedef int (*OwnerMap)[MAX_COACHES][MAX_BERTHS + 1];       /* the berth map of ONE journey */
static int choose_seats(OwnerMap own, int cls, int pref, int n, SeatRef out[], int *together) {
    const ClassInfo *ci = &CLASSES[cls];
    int per = per_coach(cls), total_free = 0;
    for (int c = 0; c < ci->ncoaches; c++)
        for (int b = 1; b <= per; b++) if (!own[cls][c][b]) total_free++;
    if (total_free < n) return 0;

    /* Candidate pools: one compartment, or two neighbouring compartments, of a coach.
     * From each pool take the preferred berth type first, then the lowest free numbers.
     * Rank:  fewest compartments used  >  most preferred-type berths  >  smallest span. */
    int best_bays = 9, best_match = -1, best_span = 1 << 30, found = 0;
    for (int c = 0; c < ci->ncoaches; c++)
        for (int s0 = 0; s0 < ci->bays; s0++)
        for (int w = 1; w <= 2; w++) {                       /* pool = 1 compartment, then 2 neighbouring ones */
            int g[2 * 8], m = 0;
            for (int b = s0 * ci->bay_size + 1; b <= (s0 + w) * ci->bay_size && b <= per; b++)
                if (!own[cls][c][b]) g[m++] = b;
            if (m < n) continue;
            int pick[MAX_GROUP], got = 0, match = 0;
            for (int i = 0; i < m && got < n; i++) if (pref_match(cls, g[i], pref)) { pick[got++] = g[i]; match++; }
            for (int i = 0; i < m && got < n; i++) if (!pref_match(cls, g[i], pref)) pick[got++] = g[i];
            int lo = pick[0], hi = pick[0];
            for (int k = 1; k < n; k++) { if (pick[k] < lo) lo = pick[k]; if (pick[k] > hi) hi = pick[k]; }
            int bays = bay_of(cls, hi) - bay_of(cls, lo) + 1, span = hi - lo;
            if (bays < best_bays || (bays == best_bays && (match > best_match || (match == best_match && span < best_span)))) {
                best_bays = bays; best_match = match; best_span = span; found = 1;
                for (int k = 0; k < n; k++) { out[k].coach = c; out[k].berth = pick[k]; }
            }
        }
    if (found) { *together = 1; return 1; }

    *together = (n == 1);
    int got = 0;
    for (int pass = 0; pass < 3 && got < n; pass++)
        for (int c = ci->ncoaches - 1; c >= 0 && got < n; c--)
            for (int b = 1; b <= per && got < n; b++) {
                if (own[cls][c][b]) continue;
                int dup = 0, same_bay = 0;
                for (int k = 0; k < got; k++) {
                    if (out[k].coach == c && out[k].berth == b) dup = 1;
                    if (out[k].coach == c && bay_of(cls, out[k].berth) == bay_of(cls, b)) same_bay = 1;
                }
                if (dup) continue;
                if (pass == 0 && (!pref_match(cls, b, pref) || same_bay)) continue;   /* far apart + preferred type */
                if (pass == 1 && same_bay) continue;                                  /* far apart, any type */
                out[got].coach = c; out[got].berth = b; got++;
            }
    return got == n;
}

static void seat_label(int cls, SeatRef s, char *out, size_t n) {
    snprintf(out, n, "%c%d/%d", CLASSES[cls].prefix, s.coach + 1, s.berth);
}

/* ------------------------------------------------------------------ */
/*  Refund policy                                                      */
/* ------------------------------------------------------------------ */
static int ticket_paid(const Booking *b) { return b->fare_total - b->discount; }     /* meals are refunded separately */

static int calc_charge(const Booking *b, int hours) {
    int c, paid = ticket_paid(b);
    if (hours >= 48) { c = CLASSES[b->cls].flat_cancel * b->count; if (c > paid) c = paid; }  /* flat fee per passenger */
    else if (hours >= 12) c = paid * 25 / 100;
    else if (hours >= 4)  c = paid * 50 / 100;
    else                  c = paid;                                                           /* chart prepared */
    return c;
}

/* ------------------------------------------------------------------ */
/*  Workers: hold / confirm / release / cancel (critical sections)     */
/* ------------------------------------------------------------------ */
static int find_booking(int pnr) {
    for (int i = 0; i < nbookings && i < MAX_BOOKINGS; i++) if (bookings[i].pnr == pnr) return i;
    return -1;
}

static void free_berths(Booking *b) {                   /* caller holds the write lock */
    for (int k = 0; k < b->count; k++) {
        int *o = &journeys[b->jidx].owner[b->cls][b->seats[k].coach][b->seats[k].berth];
        if (*o == b->pnr) *o = 0;
    }
}

static void count_confirmed(const Booking *b) {         /* statistics for a ticket that became CONFIRMED */
    __sync_fetch_and_add(&st_conf, 1); __sync_fetch_and_add(&st_ch[b->channel], 1);
    __sync_fetch_and_add(b->together ? &st_together : &st_split, 1);
    int nm = 0; for (int k = 0; k < b->count; k++) if (b->meal[k]) nm++;
    if (nm) { __sync_fetch_and_add(&st_meals, nm); __sync_fetch_and_add(&st_meal_rev, b->meal_total); }
}

/* BOOK: choose berths + record the booking.  hold=1 -> status HELD (seats reserved for HOLD_SECONDS while the
 * passenger picks meals); hold=0 -> straight to CONFIRMED (console simulation). */
static void process_book(Job *j, long wid) {
    const ClassInfo *ci = &CLASSES[j->cls];
    const ChannelInfo *ch = &CHANNELS[j->channel];
    SeatRef seats[MAX_GROUP]; int together = 0, ok = 0, pnr = 0;
    int ji = journey_find(j->trip.train, j->trip.run_dn, 1);
    if (ji < 0) {
        j->ok = 0; __sync_fetch_and_add(&st_rej, 1);
        snprintf(j->msg, sizeof j->msg, "Too many journeys open in this demo - reset the system");
        return;
    }

    if (use_lock) pthread_rwlock_wrlock(&db_lock);
    /* ============ CRITICAL SECTION: choose berths + commit ============ */
    ok = choose_seats(journeys[ji].owner, j->cls, j->pref, j->count, seats, &together);
    if (ok) {
        usleep(2000);                                   /* simulated payment/DB latency: widens the race window */
        int idx = __sync_fetch_and_add(&nbookings, 1);
        pnr = __sync_fetch_and_add(&next_pnr, 1);
        Booking *b = &bookings[idx % MAX_BOOKINGS];
        memset(b, 0, sizeof *b);
        b->pnr = pnr; b->channel = j->channel; b->cls = j->cls; b->pref = j->pref; b->count = j->count;
        b->priority = j->priority; b->rank = j->rank; b->together = together;
        b->train = j->trip.train; b->jidx = ji; b->km = j->trip.km; b->dep = j->trip.dep; b->arr = j->trip.arr;
        b->from = trains[j->trip.train].stn[j->trip.from]; b->to = trains[j->trip.train].stn[j->trip.to];
        for (int k = 0; k < j->count; k++) {
            memcpy(b->names[k], j->names[k], 32);
            b->seats[k] = seats[k];
            journeys[ji].owner[j->cls][seats[k].coach][seats[k].berth] = pnr;
        }
        b->fare_total = j->trip.fare[j->cls] * j->count;
        b->discount = b->fare_total * ch->discount_pct / 100;
        b->paid = b->fare_total - b->discount;
        b->booked_at = time(NULL);
        b->expires_at = b->booked_at + HOLD_SECONDS;
        b->status = j->hold ? ST_HELD : ST_CONFIRMED;
        if (!j->hold) count_confirmed(b);
    }
    /* ============================ END ================================= */
    if (use_lock) pthread_rwlock_unlock(&db_lock);

    if (ok) {
        j->ok = 1; j->res_pnr = pnr;
        snprintf(j->msg, sizeof j->msg, j->hold ? "Seats reserved - choose meals or skip, then confirm within %d seconds" : "Booking confirmed", HOLD_SECONDS);
        logf_("[Worker %ld] #%d %-6s %s PNR %d  %d x %s  train %d  %s", wid, j->rank, ch->code, j->hold ? "HELD     " : "CONFIRMED", pnr, j->count, ci->code,
              trains[j->trip.train].no, j->count == 1 ? "single" : together ? "seats together" : "NOT together");
    } else {
        j->ok = 0; __sync_fetch_and_add(&st_rej, 1);
        snprintf(j->msg, sizeof j->msg, "Fewer than %d berths left in %s on this train/date - booking not possible", j->count, ci->name);
        logf_("[Worker %ld] #%d %-6s REJECTED  %d x %s train %d (sold out)", wid, j->rank, ch->code, j->count, ci->code, trains[j->trip.train].no);
    }
}

/* CONFIRM: attach the meals chosen for each seat (or none) and turn the held booking into a ticket. */
static void process_confirm(Job *j, long wid) {
    if (use_lock) pthread_rwlock_wrlock(&db_lock);
    int idx = find_booking(j->pnr);
    j->ok = 0;
    if (idx < 0) snprintf(j->msg, sizeof j->msg, "PNR %d not found", j->pnr);
    else {
        Booking *b = &bookings[idx];
        if (b->channel != j->channel) snprintf(j->msg, sizeof j->msg, "This booking belongs to another website");
        else if (b->status == ST_CONFIRMED) snprintf(j->msg, sizeof j->msg, "PNR %d is already confirmed", j->pnr);
        else if (b->status != ST_HELD) snprintf(j->msg, sizeof j->msg, "Seat hold expired - please search and book again");
        else if (b->expires_at <= time(NULL)) {
            free_berths(b); b->status = ST_EXPIRED; __sync_fetch_and_add(&st_expired, 1);
            snprintf(j->msg, sizeof j->msg, "Seat hold expired - please search and book again");
        } else {
            int mt = 0;
            for (int k = 0; k < b->count; k++) { b->meal[k] = j->meal[k]; mt += MEALS[j->meal[k]].price; }
            b->meal_total = mt; b->paid = b->fare_total - b->discount + mt;
            b->booked_at = time(NULL); b->status = ST_CONFIRMED;
            count_confirmed(b);
            j->ok = 1; j->res_pnr = b->pnr;
            snprintf(j->msg, sizeof j->msg, mt ? "Ticket confirmed with meals" : "Ticket confirmed (no meals)");
            logf_("[Worker %ld] #%d %-6s CONFIRMED PNR %d  meals Rs %d", wid, j->rank, CHANNELS[b->channel].code, b->pnr, mt);
        }
    }
    if (use_lock) pthread_rwlock_unlock(&db_lock);
}

/* RELEASE: passenger abandoned the booking page -> seats go back to the pool immediately. */
static void process_release(Job *j, long wid) {
    if (use_lock) pthread_rwlock_wrlock(&db_lock);
    int idx = find_booking(j->pnr);
    j->ok = 0;
    if (idx < 0) snprintf(j->msg, sizeof j->msg, "PNR %d not found", j->pnr);
    else {
        Booking *b = &bookings[idx];
        if (b->channel != j->channel) snprintf(j->msg, sizeof j->msg, "This booking belongs to another website");
        else if (b->status != ST_HELD) snprintf(j->msg, sizeof j->msg, "Nothing to release");
        else {
            free_berths(b); b->status = ST_EXPIRED; j->ok = 1; j->res_pnr = b->pnr;
            snprintf(j->msg, sizeof j->msg, "Seat hold released");
            logf_("[Worker %ld] #%d %-6s RELEASED  PNR %d (seats back in the pool)", wid, j->rank, CHANNELS[b->channel].code, b->pnr);
        }
    }
    if (use_lock) pthread_rwlock_unlock(&db_lock);
}

static void process_cancel(Job *j, long wid) {
    if (use_lock) pthread_rwlock_wrlock(&db_lock);
    int idx = find_booking(j->pnr);
    j->ok = 0;
    if (idx < 0) snprintf(j->msg, sizeof j->msg, "PNR %d not found", j->pnr);
    else {
        Booking *b = &bookings[idx];
        if (b->status == ST_HELD) snprintf(j->msg, sizeof j->msg, "PNR %d is not confirmed yet", j->pnr);
        else if (b->status == ST_EXPIRED) snprintf(j->msg, sizeof j->msg, "PNR %d was never confirmed (hold expired)", j->pnr);
        else if (b->status != ST_CONFIRMED) snprintf(j->msg, sizeof j->msg, "PNR %d is already cancelled", j->pnr);
        else if (j->channel != CH_DIRECT && b->channel != j->channel)
            snprintf(j->msg, sizeof j->msg, "This ticket was not booked through %s", CHANNELS[j->channel].name);
        else {
            int hrs = j->hours >= 0 ? j->hours : (int)(difftime(b->dep, time(NULL)) / 3600.0);
            if (hrs < 0) hrs = 0;
            int meal_back = hrs >= 4 ? b->meal_total : 0;               /* meals: full refund unless the pantry is already loaded */
            b->charge = calc_charge(b, hrs) + (b->meal_total - meal_back);
            b->refund = b->paid - b->charge;
            b->hours_before = hrs;
            free_berths(b);
            b->cancelled_at = time(NULL);
            b->status = ST_CANCELLED;
            j->ok = 1; j->res_pnr = b->pnr;
            snprintf(j->msg, sizeof j->msg, "Ticket cancelled, Rs %d refunded", b->refund);
            __sync_fetch_and_add(&st_cancel, 1); __sync_fetch_and_add(&st_refunded, b->refund);
            logf_("[Worker %ld] #%d %-6s CANCELLED PNR %d  refund Rs %d (%d h before departure)", wid, j->rank,
                  CHANNELS[b->channel].code, b->pnr, b->refund, hrs);
        }
    }
    if (use_lock) pthread_rwlock_unlock(&db_lock);
}

/* Reaper thread: a held booking that is not confirmed in time loses its seats (timeout). */
static void *reaper_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(1);
        time_t now = time(NULL);
        if (use_lock) pthread_rwlock_wrlock(&db_lock);
        for (int i = 0; i < nbookings && i < MAX_BOOKINGS; i++) {
            Booking *b = &bookings[i];
            if (b->status == ST_HELD && b->expires_at <= now) {
                free_berths(b); b->status = ST_EXPIRED; __sync_fetch_and_add(&st_expired, 1);
                logf_("[Reaper] hold on PNR %d expired - seats released", b->pnr);
            }
        }
        if (use_lock) pthread_rwlock_unlock(&db_lock);
    }
    return NULL;
}

static void *worker_thread(void *arg) {
    long wid = (long)arg;
    for (;;) {
        Job *j = pq_dequeue();
        if (j->type == JOB_STOP) break;                 /* poison pill */
        if (delay_ms > 0) usleep(delay_ms * 1000);      /* simulated request processing time */
        switch (j->type) {
            case JOB_BOOK:    process_book(j, wid);    break;
            case JOB_CONFIRM: process_confirm(j, wid); break;
            case JOB_RELEASE: process_release(j, wid); break;
            default:          process_cancel(j, wid);  break;
        }
        psem_post(&j->done);
    }
    return NULL;
}

static void submit_and_wait(Job *j) {
    j->priority = CHANNELS[j->channel].priority;
    pq_enqueue(j);
    psem_wait(&j->done);
}

/* ------------------------------------------------------------------ */
/*  Seat audit (HELD and CONFIRMED tickets both own their berths)       */
/* ------------------------------------------------------------------ */
static void run_audit(int *doubles, int *orphans, int *confirmed) {
    static unsigned char cnt[MAX_JOURNEYS][3][MAX_COACHES][MAX_BERTHS + 1];
    memset(cnt, 0, sizeof cnt);
    *doubles = *orphans = *confirmed = 0;
    for (int i = 0; i < nbookings && i < MAX_BOOKINGS; i++) {
        Booking *b = &bookings[i];
        if (b->status != ST_CONFIRMED && b->status != ST_HELD) continue;
        if (b->status == ST_CONFIRMED) (*confirmed)++;
        for (int k = 0; k < b->count; k++) if (cnt[b->jidx][b->cls][b->seats[k].coach][b->seats[k].berth] < 250) cnt[b->jidx][b->cls][b->seats[k].coach][b->seats[k].berth]++;
    }
    for (int ji = 0; ji < njourneys; ji++)
        for (int c = 0; c < 3; c++) for (int k = 0; k < CLASSES[c].ncoaches; k++)
            for (int b = 1; b <= per_coach(c); b++) {
                if (cnt[ji][c][k][b] > 1) (*doubles)++;
                if (journeys[ji].owner[c][k][b] && cnt[ji][c][k][b] == 0) (*orphans)++;
            }
}

static void reset_state(void) {
    pthread_rwlock_wrlock(&db_lock);
    pthread_mutex_lock(&jmutex);
    memset(journeys, 0, sizeof journeys); njourneys = 0;
    pthread_mutex_unlock(&jmutex);
    memset(bookings, 0, sizeof bookings);
    nbookings = 0; next_pnr = 1000001;
    st_conf = st_rej = st_cancel = st_together = st_split = st_refunded = st_expired = st_meals = st_meal_rev = 0;
    memset(st_ch, 0, sizeof st_ch);
    service_rank = 0;
    pthread_rwlock_unlock(&db_lock);
    logf_("[Admin] system reset - all berths free on every train and date");
}

/* ------------------------------------------------------------------ */
/*  Burst / simulation (many passengers at once, mixed channels)       */
/*  All of them fight for the SAME train + date (Chennai -> New Delhi)  */
/* ------------------------------------------------------------------ */
typedef struct { int id; unsigned seed; int channel; Job job; } SimArg;
static Trip sim_trip;

static void *sim_passenger(void *p) {
    SimArg *a = p;
    Job *j = &a->job;
    int r = rand_r(&a->seed) % 100;
    j->type = JOB_BOOK; j->channel = a->channel; j->trip = sim_trip; j->hold = 0;
    j->cls = r < 55 ? CL_3A : r < 85 ? CL_2A : CL_1A;
    int sz = rand_r(&a->seed) % 100;
    j->count = sz < 35 ? 1 : sz < 55 ? 2 : sz < 70 ? 3 : sz < 85 ? 4 : sz < 95 ? 5 : 6;
    static const int p3[] = { -1, T_LB, T_MB, T_UB, T_SL, T_SU }, p2[] = { -1, T_LB, T_UB, T_SL, T_SU };
    j->pref = j->cls == CL_1A ? T_SB : j->cls == CL_2A ? p2[rand_r(&a->seed) % 5] : p3[rand_r(&a->seed) % 6];
    for (int k = 0; k < j->count; k++) snprintf(j->names[k], 32, "P%d-%d", a->id, k + 1);
    submit_and_wait(j);
    return NULL;
}

static void run_burst(int ndirect, int npartner, int *out_conf, double *avg_direct, double *avg_partner, void (*each)(SimArg *, void *), void *ctx) {
    int total = ndirect + npartner;
    char err[160];
    make_trip(0, station_index("MAS"), station_index("NDLS"), dn_today() + 7, &sim_trip, err, sizeof err);   /* train 12345, 7 days ahead */
    SimArg *args = calloc(total, sizeof *args);
    pthread_t *th = malloc(total * sizeof *th);
    hold_workers = 1;                                   /* let the queue fill so priority becomes visible */
    for (int i = 0; i < total; i++) {
        args[i].id = i + 1; args[i].seed = 7919u * (i + 3);
        args[i].channel = i < ndirect ? CH_DIRECT : (i % 2 ? CH_MMT : CH_GOI);
        job_init(&args[i].job);
        pthread_create(&th[i], NULL, sim_passenger, &args[i]);
    }
    for (int w = 0; w < 400; w++) {                     /* wait (max ~2s) until everything is queued */
        pthread_mutex_lock(&q_mutex); int n = pq_n; pthread_mutex_unlock(&q_mutex);
        if (n >= (total > QUEUE_CAP ? QUEUE_CAP : total)) break;
        usleep(5000);
    }
    hold_workers = 0;
    for (int i = 0; i < total; i++) pthread_join(th[i], NULL);
    double sd = 0, sp = 0; int cd = 0, cp = 0, conf = 0;
    for (int i = 0; i < total; i++) {
        if (args[i].job.ok) conf++;
        if (args[i].job.priority == 1) { sp += args[i].job.rank; cp++; } else { sd += args[i].job.rank; cd++; }
        if (each) each(&args[i], ctx);
    }
    *out_conf = conf; *avg_direct = cd ? sd / cd : 0; *avg_partner = cp ? sp / cp : 0;
    free(args); free(th);
}
/* ------------------------------------------------------------------ */
/*  JSON helpers                                                       */
/* ------------------------------------------------------------------ */
typedef struct { char *p; size_t n, cap; } SB;
static void sb_init(SB *s) { s->cap = 4096; s->p = malloc(s->cap); s->n = 0; s->p[0] = 0; }
static void sb_add(SB *s, const char *fmt, ...) {
    for (;;) {
        va_list ap; va_start(ap, fmt);
        int w = vsnprintf(s->p + s->n, s->cap - s->n, fmt, ap);
        va_end(ap);
        if (w < 0) return;
        if ((size_t)w < s->cap - s->n) { s->n += w; return; }
        s->cap = s->cap * 2 + w; s->p = realloc(s->p, s->cap);
    }
}
static void fmt_time(time_t t, char *out, size_t n) {
    struct tm tm; localtime_r(&t, &tm); strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
}

static void fmt_hm(time_t t, char *out, size_t n) {
    struct tm tm; localtime_r(&t, &tm); strftime(out, n, "%Y-%m-%d %H:%M", &tm);
}

static void booking_json(SB *s, const Booking *b) {
    char ts[32], cs[32], lab[16], dep[24], arr[24];
    fmt_time(b->booked_at, ts, sizeof ts); fmt_hm(b->dep, dep, sizeof dep); fmt_hm(b->arr, arr, sizeof arr);
    sb_add(s, "{\"pnr\":%d,\"channel\":\"%s\",\"channelName\":\"%s\",\"class\":\"%s\",\"className\":\"%s\","
              "\"pref\":\"%s\",\"count\":%d,\"priority\":%d,\"rank\":%d,"
              "\"trainNo\":%d,\"trainName\":\"%s\",\"fromCode\":\"%s\",\"fromName\":\"%s\",\"toCode\":\"%s\",\"toName\":\"%s\","
              "\"km\":%d,\"dep\":\"%s\",\"arr\":\"%s\",\"names\":[",
           b->pnr, CHANNELS[b->channel].code, CHANNELS[b->channel].name, CLASSES[b->cls].code, CLASSES[b->cls].name,
           b->pref < 0 ? "NP" : TYPE_CODE[b->pref], b->count, b->priority, b->rank,
           trains[b->train].no, trains[b->train].name, STATIONS[b->from].code, STATIONS[b->from].name,
           STATIONS[b->to].code, STATIONS[b->to].name, b->km, dep, arr);
    for (int k = 0; k < b->count; k++) sb_add(s, "%s\"%s\"", k ? "," : "", b->names[k]);
    sb_add(s, "],\"seats\":[");
    for (int k = 0; k < b->count; k++) {
        int t = berth_type(b->cls, b->seats[k].berth), m = b->meal[k];
        seat_label(b->cls, b->seats[k], lab, sizeof lab);
        sb_add(s, "%s{\"label\":\"%s\",\"type\":\"%s\",\"typeName\":\"%s\",\"mealId\":%d,\"meal\":\"%s\",\"mealPrice\":%d}",
               k ? "," : "", lab, TYPE_CODE[t], TYPE_NAME[t], m, MEALS[m].name, MEALS[m].price);
    }
    const char *note = b->count == 1 ? "Single passenger" : b->together ? "Seats allotted together"
                     : "SEATS NOT AVAILABLE TOGETHER - passengers allotted separate berths in different compartments/coaches";
    const char *st = b->status == ST_CONFIRMED ? "CONFIRMED" : b->status == ST_CANCELLED ? "CANCELLED" : b->status == ST_HELD ? "HELD" : "EXPIRED";
    sb_add(s, "],\"together\":%s,\"note\":\"%s\",\"fare\":%d,\"discount\":%d,\"discountPct\":%d,\"mealTotal\":%d,\"paid\":%d,"
              "\"status\":\"%s\",\"bookedAt\":\"%s\"",
           b->together ? "true" : "false", note, b->fare_total, b->discount, CHANNELS[b->channel].discount_pct, b->meal_total, b->paid, st, ts);
    if (b->status == ST_HELD) { long left = (long)difftime(b->expires_at, time(NULL)); sb_add(s, ",\"expiresIn\":%ld", left < 0 ? 0 : left); }
    if (b->status == ST_CANCELLED) {
        fmt_time(b->cancelled_at, cs, sizeof cs);
        sb_add(s, ",\"cancelledAt\":\"%s\",\"refund\":%d,\"charge\":%d,\"hoursBefore\":%d", cs, b->refund, b->charge, b->hours_before);
    }
    sb_add(s, "}");
}

/* ------------------------------------------------------------------ */
/*  Minimal HTTP server                                                */
/* ------------------------------------------------------------------ */
static void url_decode(char *dst, const char *src, size_t max) {
    size_t n = 0;
    while (*src && n + 1 < max) {
        if (*src == '+') { dst[n++] = ' '; src++; }
        else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char h[3] = { src[1], src[2], 0 }; dst[n++] = (char)strtol(h, NULL, 16); src += 3;
        } else dst[n++] = *src++;
    }
    dst[n] = 0;
}
static int get_param(const char *qs, const char *key, char *out, size_t max) {
    size_t kl = strlen(key);
    while (qs && *qs) {
        const char *amp = strchr(qs, '&'); size_t len = amp ? (size_t)(amp - qs) : strlen(qs);
        if (len > kl && qs[kl] == '=' && !strncmp(qs, key, kl)) {
            char tmp[600]; size_t vl = len - kl - 1; if (vl >= sizeof tmp) vl = sizeof tmp - 1;
            memcpy(tmp, qs + kl + 1, vl); tmp[vl] = 0; url_decode(out, tmp, max); return 1;
        }
        qs = amp ? amp + 1 : NULL;
    }
    return 0;
}
static void send_all(int fd, const char *p, size_t n) {
    while (n) { ssize_t w = send(fd, p, n, MSG_NOSIGNAL); if (w <= 0) return; p += w; n -= w; }
}
static void send_resp(int fd, int code, const char *ctype, const char *body, size_t len) {
    const char *txt = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : "Conflict";
    char h[300];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Headers: Content-Type\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", code, txt, ctype, len);
    send_all(fd, h, n); if (len) send_all(fd, body, len);
}
static void send_json(int fd, int code, SB *s) { send_resp(fd, code, "application/json", s->p, s->n); free(s->p); }
static void send_error(int fd, int code, const char *msg) {
    SB s; sb_init(&s); sb_add(&s, "{\"ok\":false,\"message\":\"%s\"}", msg); send_json(fd, code, &s);
}

static void serve_file(int fd, const char *path) {
    char rel[128], full[400];
    if (!strcmp(path, "/")) strcpy(rel, "index.html");
    else if (!strcmp(path, "/makemytrip")) strcpy(rel, "makemytrip.html");
    else if (!strcmp(path, "/goibibo")) strcpy(rel, "goibibo.html");
    else snprintf(rel, sizeof rel, "%.120s", path + 1);
    for (char *c = rel; *c; c++) if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_' && *c != '-') { send_error(fd, 404, "Not found"); return; }
    if (strstr(rel, "..")) { send_error(fd, 404, "Not found"); return; }
    snprintf(full, sizeof full, "%s/%s", web_dir, rel);
    FILE *f = fopen(full, "rb");
    if (!f) { send_error(fd, 404, "Not found"); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *data = malloc(sz + 1); size_t rd = fread(data, 1, sz, f); fclose(f);
    const char *ext = strrchr(rel, '.');
    const char *ct = !ext ? "text/plain" : !strcmp(ext, ".html") ? "text/html; charset=utf-8" : !strcmp(ext, ".css") ? "text/css" : !strcmp(ext, ".js") ? "application/javascript" : "text/plain";
    send_resp(fd, 200, ct, data, rd); free(data);
}


/* ------------------------------------------------------------------ */
/*  API handlers                                                       */
/* ------------------------------------------------------------------ */
/* Reads train / from / to / date from the request. Sends the error itself and returns 0 on failure. */
static int parse_trip(int fd, const char *params, Trip *t, int allow_departed) {
    char v[64], err[160];
    int tr, f, to;
    if (!get_param(params, "train", v, sizeof v) || (tr = find_train(atoi(v))) < 0) { send_error(fd, 400, "Please select a train"); return 0; }
    if (!get_param(params, "from", v, sizeof v) || (f = station_index(v)) < 0) { send_error(fd, 400, "Please select the starting station"); return 0; }
    if (!get_param(params, "to", v, sizeof v) || (to = station_index(v)) < 0) { send_error(fd, 400, "Please select the destination station"); return 0; }
    long dn;
    if (!get_param(params, "date", v, sizeof v) || (dn = dn_parse(v)) < 0) { send_error(fd, 400, "Please choose a valid travel date (YYYY-MM-DD)"); return 0; }
    int r = make_trip(tr, f, to, dn, t, err, sizeof err);
    if (r < 0) { send_error(fd, 400, err); return 0; }
    if (r == 1 && !allow_departed) { send_error(fd, 400, "This train has already departed from your boarding station"); return 0; }
    return 1;
}

static void handle_meta(int fd) {
    SB s; sb_init(&s); char today[16], maxd[16];
    dn_fmt(dn_today(), today, sizeof today); dn_fmt(dn_today() + ADVANCE_DAYS, maxd, sizeof maxd);
    sb_add(&s, "{\"today\":\"%s\",\"maxDate\":\"%s\",\"holdSeconds\":%d,\"stations\":[", today, maxd, HOLD_SECONDS);
    for (int i = 0; i < NSTATIONS; i++) sb_add(&s, "%s{\"code\":\"%s\",\"name\":\"%s\"}", i ? "," : "", STATIONS[i].code, STATIONS[i].name);
    sb_add(&s, "],\"trains\":[");
    for (int i = 0; i < ntrains; i++) {
        const Train *t = &trains[i];
        sb_add(&s, "%s{\"no\":%d,\"name\":\"%s\",\"route\":\"", i ? "," : "", t->no, t->name);
        for (int k = 0; k < t->n; k++) sb_add(&s, "%s%s", k ? " - " : "", STATIONS[t->stn[k]].code);
        sb_add(&s, "\"}");
    }
    sb_add(&s, "],\"meals\":[");
    for (int i = 0; i < NMEALS; i++) sb_add(&s, "%s{\"id\":%d,\"code\":\"%s\",\"name\":\"%s\",\"price\":%d}", i ? "," : "", i, MEALS[i].code, MEALS[i].name, MEALS[i].price);
    sb_add(&s, "]}"); send_json(fd, 200, &s);
}

/* Train search: every train that runs from -> to on the chosen date, with fare and free berths per class. */
static void handle_trains(int fd, const char *params) {
    char v[64], err[160]; int f, to; long dn;
    if (!get_param(params, "from", v, sizeof v) || (f = station_index(v)) < 0) { send_error(fd, 400, "Please select the starting station"); return; }
    if (!get_param(params, "to", v, sizeof v) || (to = station_index(v)) < 0) { send_error(fd, 400, "Please select the destination station"); return; }
    if (f == to) { send_error(fd, 400, "Starting station and destination must be different"); return; }
    if (!get_param(params, "date", v, sizeof v) || (dn = dn_parse(v)) < 0) { send_error(fd, 400, "Please choose a valid travel date"); return; }
    if (dn < dn_today()) { send_error(fd, 400, "Travel date is in the past"); return; }
    if (dn > dn_today() + ADVANCE_DAYS) { send_error(fd, 400, "Bookings open only 60 days in advance"); return; }

    Trip found[MAX_TRAINS]; int st[MAX_TRAINS], idx[MAX_TRAINS], n = 0;
    for (int i = 0; i < ntrains; i++) {
        int r = make_trip(i, f, to, dn, &found[n], err, sizeof err);
        if (r < 0) continue;
        st[n] = r; idx[n] = n; n++;
    }
    for (int a = 1; a < n; a++) { int k = idx[a], b = a - 1; while (b >= 0 && found[idx[b]].dep > found[k].dep) { idx[b + 1] = idx[b]; b--; } idx[b + 1] = k; }   /* sort by departure */

    SB s; sb_init(&s);
    sb_add(&s, "{\"ok\":true,\"from\":\"%s\",\"to\":\"%s\",\"trains\":[", STATIONS[f].name, STATIONS[to].name);
    pthread_rwlock_rdlock(&db_lock);
    for (int q = 0; q < n; q++) {
        int k = idx[q]; Trip *t = &found[k]; const Train *tr = &trains[t->train];
        char dep[24], arr[24]; fmt_hm(t->dep, dep, sizeof dep); fmt_hm(t->arr, arr, sizeof arr);
        int mins = (int)(difftime(t->arr, t->dep) / 60), ji = journey_find(t->train, t->run_dn, 0);
        char run[16]; dn_fmt(t->run_dn, run, sizeof run);
        sb_add(&s, "%s{\"no\":%d,\"name\":\"%s\",\"dep\":\"%s\",\"arr\":\"%s\",\"km\":%d,\"duration\":\"%dh %02dm\",\"runDate\":\"%s\",\"status\":\"%s\",\"classes\":[",
               q ? "," : "", tr->no, tr->name, dep, arr, t->km, mins / 60, mins % 60, run, st[k] ? "DEPARTED" : "OPEN");
        int first = 1;
        for (int c = 0; c < 3; c++) {
            if (!(tr->cmask & (1 << c))) continue;
            int total = class_total(c), free_n = total;
            if (ji >= 0) { free_n = 0; for (int co = 0; co < CLASSES[c].ncoaches; co++) for (int b = 1; b <= per_coach(c); b++) if (!journeys[ji].owner[c][co][b]) free_n++; }
            sb_add(&s, "%s{\"code\":\"%s\",\"name\":\"%s\",\"fare\":%d,\"free\":%d,\"total\":%d}", first ? "" : ",", CLASSES[c].code, CLASSES[c].name, t->fare[c], free_n, total);
            first = 0;
        }
        sb_add(&s, "]}");
    }
    pthread_rwlock_unlock(&db_lock);
    sb_add(&s, "]}"); send_json(fd, 200, &s);
}

static int find_class(const char *s) { for (int i = 0; i < 3; i++) if (!strcasecmp(s, CLASSES[i].code)) return i; return -1; }
static int find_channel(const char *s) { for (int i = 0; i < 3; i++) if (!strcasecmp(s, CHANNELS[i].code)) return i; return -1; }
static int parse_pref(const char *s, int *out) {
    if (!*s || !strcasecmp(s, "NP")) { *out = -1; return 1; }
    for (int t = 0; t < 6; t++) if (!strcasecmp(s, TYPE_CODE[t])) { *out = t; return 1; }
    return 0;
}
static int pref_valid(int cls, int pref) {
    if (pref < 0) return 1;
    if (cls == CL_1A) return pref == T_SB;
    if (pref == T_SB) return 0;
    return cls == CL_2A ? pref != T_MB : 1;
}

static int check_channel(int fd, const char *params, int *ch) {
    char v[64];
    *ch = CH_DIRECT;
    if (get_param(params, "channel", v, sizeof v) && *v) {
        *ch = find_channel(v);
        if (*ch < 0) { send_error(fd, 400, "Unknown channel"); return 0; }
    }
    if (*ch != CH_DIRECT) {
        char key[64] = "";
        get_param(params, "key", key, sizeof key);
        if (strcmp(key, CHANNEL_KEYS[*ch])) { send_error(fd, 403, "Invalid partner API key"); return 0; }
    }
    return 1;
}

static void reply_booking(int fd, Job *j, const char *extra) {       /* JSON reply carrying the booking of job j */
    SB s; sb_init(&s);
    if (j->ok) {
        pthread_rwlock_rdlock(&db_lock);
        int idx = find_booking(j->res_pnr);
        sb_add(&s, "{\"ok\":true,\"message\":\"%s\",%s\"booking\":", j->msg, extra);
        if (idx >= 0) booking_json(&s, &bookings[idx]); else sb_add(&s, "null");
        pthread_rwlock_unlock(&db_lock);
        sb_add(&s, "}"); send_json(fd, 200, &s);
    } else { sb_add(&s, "{\"ok\":false,\"message\":\"%s\"}", j->msg); send_json(fd, 409, &s); }
}

/* POST /api/book  (hold=1: reserve seats, meals chosen next;  hold=0/absent: confirm immediately, no meals) */
static void handle_book(int fd, const char *params) {
    char v[600];
    int ch, cls, pref, count = 0; Trip trip;
    if (!check_channel(fd, params, &ch)) return;
    if (!parse_trip(fd, params, &trip, 0)) return;
    if (!get_param(params, "class", v, sizeof v) || (cls = find_class(v)) < 0) { send_error(fd, 400, "Choose a class: 1A, 2A or 3A"); return; }
    if (!(trains[trip.train].cmask & (1 << cls))) { send_error(fd, 400, "That class is not available on this train"); return; }
    if (!get_param(params, "pref", v, sizeof v)) v[0] = 0;
    if (!parse_pref(v, &pref) || !pref_valid(cls, pref)) { send_error(fd, 400, "That berth type is not available in the selected class"); return; }
    Job *j = malloc(sizeof *j); job_init(j);
    if (!get_param(params, "names", v, sizeof v)) v[0] = 0;
    char *save = NULL;
    for (char *tok = strtok_r(v, ",", &save); tok && count < MAX_GROUP; tok = strtok_r(NULL, ",", &save)) {
        char clean[32]; int n = 0;
        for (; *tok == ' '; tok++);
        for (; *tok && n < 31; tok++) if (isalnum((unsigned char)*tok) || *tok == ' ' || *tok == '.' || *tok == '-') clean[n++] = *tok;
        while (n > 0 && clean[n - 1] == ' ') n--;
        clean[n] = 0;
        if (n) { strcpy(j->names[count++], clean); }
    }
    if (count < 1) { free(j); send_error(fd, 400, "Enter at least one passenger name (max 6)"); return; }
    char hv[8] = ""; get_param(params, "hold", hv, sizeof hv);
    j->type = JOB_BOOK; j->channel = ch; j->cls = cls; j->pref = pref; j->count = count; j->trip = trip; j->hold = (hv[0] == '1');
    submit_and_wait(j);
    char extra[64]; snprintf(extra, sizeof extra, "\"synced\":%s,\"holdSeconds\":%d,", ch != CH_DIRECT ? "true" : "false", HOLD_SECONDS);
    reply_booking(fd, j, extra);
    free(j);
}

/* POST /api/confirm  pnr + meals=0,1,0,...  (one meal id per passenger/seat; 0 or empty = skip meal) */
static void handle_confirm(int fd, const char *params) {
    char v[128]; int ch;
    if (!check_channel(fd, params, &ch)) return;
    if (!get_param(params, "pnr", v, sizeof v) || atoi(v) <= 0) { send_error(fd, 400, "Invalid PNR"); return; }
    Job *j = malloc(sizeof *j); job_init(j);
    j->type = JOB_CONFIRM; j->channel = ch; j->pnr = atoi(v);
    if (get_param(params, "meals", v, sizeof v)) {
        int k = 0; char *save = NULL;
        for (char *tok = strtok_r(v, ",", &save); tok && k < MAX_GROUP; tok = strtok_r(NULL, ",", &save), k++) {
            int m = atoi(tok);
            if (m < 0 || m >= NMEALS) { free(j); send_error(fd, 400, "Unknown meal option"); return; }
            j->meal[k] = m;
        }
    }
    submit_and_wait(j);
    reply_booking(fd, j, "");
    free(j);
}

static void handle_release(int fd, const char *params) {
    char v[64]; int ch;
    if (!check_channel(fd, params, &ch)) return;
    if (!get_param(params, "pnr", v, sizeof v) || atoi(v) <= 0) { send_error(fd, 400, "Invalid PNR"); return; }
    Job *j = malloc(sizeof *j); job_init(j);
    j->type = JOB_RELEASE; j->channel = ch; j->pnr = atoi(v);
    submit_and_wait(j);
    reply_booking(fd, j, "");
    free(j);
}

static void handle_cancel(int fd, const char *params) {
    char v[64]; int ch;
    if (!check_channel(fd, params, &ch)) return;
    if (!get_param(params, "pnr", v, sizeof v) || atoi(v) <= 0) { send_error(fd, 400, "Enter a valid PNR"); return; }
    Job *j = malloc(sizeof *j); job_init(j);
    j->type = JOB_CANCEL; j->channel = ch; j->pnr = atoi(v);
    if (get_param(params, "hours", v, sizeof v) && *v) j->hours = atoi(v);
    submit_and_wait(j);
    reply_booking(fd, j, "");
    free(j);
}

static void handle_bookings(int fd, const char *params) {
    char v[64]; int only = -1, pnr = 0;
    if (get_param(params, "channel", v, sizeof v)) only = find_channel(v);
    if (get_param(params, "pnr", v, sizeof v)) pnr = atoi(v);
    SB s; sb_init(&s); sb_add(&s, "{\"bookings\":[");
    pthread_rwlock_rdlock(&db_lock);                    /* readers can run in parallel */
    int first = 1, n = nbookings < MAX_BOOKINGS ? nbookings : MAX_BOOKINGS;
    for (int i = 0; i < n; i++) {
        Booking *b = &bookings[i];
        if ((b->status != ST_CONFIRMED && b->status != ST_CANCELLED) || (only >= 0 && b->channel != only) || (pnr && b->pnr != pnr)) continue;
        if (!first) sb_add(&s, ",");
        first = 0;
        booking_json(&s, b);
    }
    pthread_rwlock_unlock(&db_lock);
    sb_add(&s, "]}"); send_json(fd, 200, &s);
}

/* Berth-type availability of one journey: ?train=12345&date=<departure date from the origin station> */
static void handle_availability(int fd, const char *params) {
    char v[64]; int tr = 0; long dn = dn_today();
    if (get_param(params, "train", v, sizeof v) && *v) { tr = find_train(atoi(v)); if (tr < 0) { send_error(fd, 400, "Unknown train"); return; } }
    if (get_param(params, "date", v, sizeof v) && *v) { dn = dn_parse(v); if (dn < 0) { send_error(fd, 400, "Invalid date"); return; } }
    const Train *t = &trains[tr];
    SB s; sb_init(&s); char run[16]; dn_fmt(dn, run, sizeof run);
    sb_add(&s, "{\"ok\":true,\"trainNo\":%d,\"trainName\":\"%s\",\"runDate\":\"%s\",\"classes\":[", t->no, t->name, run);
    pthread_rwlock_rdlock(&db_lock);
    int ji = journey_find(tr, dn, 0), first_c = 1;
    for (int c = 0; c < 3; c++) {
        if (!(t->cmask & (1 << c))) continue;
        int total = class_total(c), free_n = 0, bytype[6] = { 0 };
        for (int k = 0; k < CLASSES[c].ncoaches; k++)
            for (int b = 1; b <= per_coach(c); b++) if (ji < 0 || !journeys[ji].owner[c][k][b]) { free_n++; bytype[berth_type(c, b)]++; }
        sb_add(&s, "%s{\"code\":\"%s\",\"name\":\"%s\",\"rate\":%d,\"total\":%d,\"free\":%d,\"types\":{", first_c ? "" : ",", CLASSES[c].code, CLASSES[c].name, CLASSES[c].rate, total, free_n);
        first_c = 0;
        int first = 1;
        for (int ty = 0; ty < 6; ty++) {
            if (!pref_valid(c, ty)) continue;                 /* only berth types that exist in this class */
            sb_add(&s, "%s\"%s\":{\"name\":\"%s\",\"free\":%d}", first ? "" : ",", TYPE_CODE[ty], TYPE_NAME[ty], bytype[ty]);
            first = 0;
        }
        sb_add(&s, "}}");
    }
    pthread_rwlock_unlock(&db_lock);
    sb_add(&s, "],\"channels\":[");
    for (int i = 0; i < 3; i++) sb_add(&s, "%s{\"code\":\"%s\",\"name\":\"%s\",\"discount\":%d,\"priority\":%d}", i ? "," : "", CHANNELS[i].code, CHANNELS[i].name, CHANNELS[i].discount_pct, CHANNELS[i].priority);
    sb_add(&s, "]}"); send_json(fd, 200, &s);
}

/* Pantry / catering manifest of one journey: which meal goes to which seat number. */
typedef struct { int cls, coach, berth, pnr, meal, from, to; char name[32]; } MealRow;
static int mealrow_cmp(const void *a, const void *b) {
    const MealRow *x = a, *y = b;
    if (x->cls != y->cls) return x->cls - y->cls;
    if (x->coach != y->coach) return x->coach - y->coach;
    return x->berth - y->berth;
}
static void handle_meals(int fd, const char *params) {
    char v[64]; int tr = 0; long dn = dn_today();
    if (get_param(params, "train", v, sizeof v) && *v) { tr = find_train(atoi(v)); if (tr < 0) { send_error(fd, 400, "Unknown train"); return; } }
    if (get_param(params, "date", v, sizeof v) && *v) { dn = dn_parse(v); if (dn < 0) { send_error(fd, 400, "Invalid date"); return; } }
    MealRow *rows = malloc(MAX_BOOKINGS * MAX_GROUP * sizeof *rows); int n = 0, per[8] = { 0 };
    pthread_rwlock_rdlock(&db_lock);
    int ji = journey_find(tr, dn, 0);
    for (int i = 0; ji >= 0 && i < nbookings && i < MAX_BOOKINGS; i++) {
        Booking *b = &bookings[i];
        if (b->status != ST_CONFIRMED || b->jidx != ji) continue;
        for (int k = 0; k < b->count; k++) if (b->meal[k]) {
            MealRow *r = &rows[n++];
            r->cls = b->cls; r->coach = b->seats[k].coach; r->berth = b->seats[k].berth; r->pnr = b->pnr; r->meal = b->meal[k];
            r->from = b->from; r->to = b->to; memcpy(r->name, b->names[k], 32);
            per[b->meal[k]]++;
        }
    }
    pthread_rwlock_unlock(&db_lock);
    qsort(rows, n, sizeof *rows, mealrow_cmp);
    SB s; sb_init(&s); char run[16]; dn_fmt(dn, run, sizeof run);
    sb_add(&s, "{\"ok\":true,\"trainNo\":%d,\"trainName\":\"%s\",\"runDate\":\"%s\",\"summary\":[", trains[tr].no, trains[tr].name, run);
    int first = 1;
    for (int m = 1; m < NMEALS; m++) { sb_add(&s, "%s{\"meal\":\"%s\",\"count\":%d}", first ? "" : ",", MEALS[m].name, per[m]); first = 0; }
    sb_add(&s, "],\"rows\":[");
    for (int i = 0; i < n; i++) {
        char lab[16]; SeatRef sr = { rows[i].coach, rows[i].berth }; seat_label(rows[i].cls, sr, lab, sizeof lab);
        sb_add(&s, "%s{\"seat\":\"%s\",\"class\":\"%s\",\"name\":\"%s\",\"pnr\":%d,\"meal\":\"%s\",\"price\":%d,\"route\":\"%s - %s\"}", i ? "," : "",
               lab, CLASSES[rows[i].cls].code, rows[i].name, rows[i].pnr, MEALS[rows[i].meal].name, MEALS[rows[i].meal].price, STATIONS[rows[i].from].code, STATIONS[rows[i].to].code);
    }
    sb_add(&s, "]}"); send_json(fd, 200, &s); free(rows);
}

static void handle_stats(int fd) {
    int d, o, c, held = 0;
    pthread_rwlock_rdlock(&db_lock); run_audit(&d, &o, &c);
    for (int i = 0; i < nbookings && i < MAX_BOOKINGS; i++) if (bookings[i].status == ST_HELD) held++;
    pthread_rwlock_unlock(&db_lock);
    pthread_mutex_lock(&q_mutex); int ql = pq_n; pthread_mutex_unlock(&q_mutex);
    SB s; sb_init(&s);
    sb_add(&s, "{\"confirmed\":%d,\"rejected\":%d,\"cancelled\":%d,\"together\":%d,\"split\":%d,\"refunded\":%d,\"held\":%d,\"expired\":%d,"
               "\"meals\":%d,\"mealRevenue\":%d,\"byChannel\":{\"DIRECT\":%d,\"MMT\":%d,\"GOI\":%d},\"queueLen\":%d,\"workers\":%d,\"mode\":\"%s\","
               "\"audit\":{\"doubleBooked\":%d,\"orphans\":%d,\"correct\":%s}}",
           st_conf, st_rej, st_cancel, st_together, st_split, st_refunded, held, st_expired, st_meals, st_meal_rev,
           st_ch[0], st_ch[1], st_ch[2], ql, num_workers,
           use_lock ? "safe" : "unsafe", d, o, (d == 0 && o == 0) ? "true" : "false");
    send_json(fd, 200, &s);
}

static void handle_log(int fd) {
    SB s; sb_init(&s); sb_add(&s, "{\"log\":[");
    pthread_mutex_lock(&log_mutex);
    int start = log_count > LOG_LINES ? log_count - LOG_LINES : 0;
    for (int i = start; i < log_count; i++) sb_add(&s, "%s\"%s\"", i > start ? "," : "", logbuf[i % LOG_LINES]);
    pthread_mutex_unlock(&log_mutex);
    sb_add(&s, "]}"); send_json(fd, 200, &s);
}

typedef struct { SB *s; int first; } BurstCtx;
static void burst_each(SimArg *a, void *ctx) {
    BurstCtx *c = ctx;
    sb_add(c->s, "%s{\"channel\":\"%s\",\"priority\":%d,\"rank\":%d,\"pnr\":%d,\"ok\":%s,\"pax\":%d,\"class\":\"%s\"}",
           c->first ? "" : ",", CHANNELS[a->job.channel].code, a->job.priority, a->job.rank, a->job.res_pnr,
           a->job.ok ? "true" : "false", a->job.count, CLASSES[a->job.cls].code);
    c->first = 0;
}
static void handle_burst(int fd) {
    SB s; sb_init(&s); int conf; double ad, ap;
    sb_add(&s, "{\"ok\":true,\"order\":[");
    BurstCtx ctx = { &s, 1 };
    logf_("[Admin] burst demo: 6 direct + 6 partner requests arrive together");
    run_burst(6, 6, &conf, &ad, &ap, burst_each, &ctx);
    sb_add(&s, "],\"avgRankDirect\":%.1f,\"avgRankPartner\":%.1f,\"confirmed\":%d}", ad, ap, conf);
    logf_("[Admin] burst done: avg service position partner %.1f vs direct %.1f", ap, ad);
    send_json(fd, 200, &s);
}

static void *conn_thread(void *arg) {
    int fd = *(int *)arg; free(arg);
    char *buf = malloc(REQ_CAP);
    size_t n = 0; int hdr_end = -1;
    while (n < REQ_CAP - 1) {
        ssize_t r = recv(fd, buf + n, REQ_CAP - 1 - n, 0);
        if (r <= 0) break;
        n += r; buf[n] = 0;
        char *e = strstr(buf, "\r\n\r\n");
        if (e) { hdr_end = (int)(e - buf) + 4; break; }
    }
    if (hdr_end < 0) goto done;
    int clen = 0; char *cl = strcasestr(buf, "Content-Length:");
    if (cl) clen = atoi(cl + 15);
    if (clen > REQ_CAP - hdr_end - 1) clen = REQ_CAP - hdr_end - 1;
    while ((int)n < hdr_end + clen) {
        ssize_t r = recv(fd, buf + n, REQ_CAP - 1 - n, 0);
        if (r <= 0) break;
        n += r;
    }
    buf[n] = 0;
    char method[8], target[300], path[300], query[300] = "";
    if (sscanf(buf, "%7s %299s", method, target) != 2) goto done;
    char *q = strchr(target, '?');
    if (q) { *q = 0; snprintf(query, sizeof query, "%s", q + 1); }
    snprintf(path, sizeof path, "%s", target);
    const char *body = buf + hdr_end;

    if (!strcmp(method, "OPTIONS")) { send_resp(fd, 204, "text/plain", "", 0); goto done; }
    if (!strcmp(path, "/favicon.ico")) { send_resp(fd, 204, "text/plain", "", 0); goto done; }
    if (strncmp(path, "/api/", 5)) { serve_file(fd, path); goto done; }
    int post = !strcmp(method, "POST");
    const char *params = post ? body : query;
    if      (!strcmp(path, "/api/book")         && post) handle_book(fd, params);
    else if (!strcmp(path, "/api/confirm")      && post) handle_confirm(fd, params);
    else if (!strcmp(path, "/api/release")      && post) handle_release(fd, params);
    else if (!strcmp(path, "/api/cancel")       && post) handle_cancel(fd, params);
    else if (!strcmp(path, "/api/meta"))         handle_meta(fd);
    else if (!strcmp(path, "/api/trains"))       handle_trains(fd, query);
    else if (!strcmp(path, "/api/bookings"))     handle_bookings(fd, query);
    else if (!strcmp(path, "/api/availability")) handle_availability(fd, query);
    else if (!strcmp(path, "/api/meals"))        handle_meals(fd, query);
    else if (!strcmp(path, "/api/stats"))        handle_stats(fd);
    else if (!strcmp(path, "/api/log"))          handle_log(fd);
    else if (!strcmp(path, "/api/demo/burst")    && post) handle_burst(fd);
    else if (!strcmp(path, "/api/reset")         && post) { reset_state(); send_error(fd, 200, "System reset"); }
    else send_error(fd, 404, "Unknown API");
done:
    close(fd); free(buf);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
static pthread_t workers[MAX_WORKERS];
static Job pills[MAX_WORKERS];

static void start_workers(void) {
    psem_init(&q_slots, QUEUE_CAP); psem_init(&q_items, 0);
    for (long i = 0; i < num_workers; i++) pthread_create(&workers[i], NULL, worker_thread, (void *)(i + 1));
}
static void stop_workers(void) {
    for (int i = 0; i < num_workers; i++) { job_init(&pills[i]); pills[i].type = JOB_STOP; pills[i].priority = 3; pq_enqueue(&pills[i]); }
    for (int i = 0; i < num_workers; i++) pthread_join(workers[i], NULL);
}

static void print_audit(void) {
    int d, o, c; run_audit(&d, &o, &c);
    printf("Confirmed bookings : %d\nDouble-booked berths: %d\nOrphan berths       : %d\n", c, d, o);
    puts(d == 0 && o == 0 ? "RESULT: CORRECT - every berth belongs to exactly one ticket."
                          : "RESULT: RACE CONDITION DETECTED - the same berth was sold more than once!");
}

int main(int argc, char *argv[]) {
    const char *mode = argc > 1 ? argv[1] : "serve";
    int requests = 40, dm_set = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--unsafe")) use_lock = 0;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--workers") && i + 1 < argc) num_workers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--requests") && i + 1 < argc) requests = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--delay-ms") && i + 1 < argc) { delay_ms = atoi(argv[++i]); dm_set = 1; }
        else if (!strcmp(argv[i], "--web") && i + 1 < argc) web_dir = argv[++i];
        else { fprintf(stderr, "Usage: %s [serve|simulate] [--port N] [--workers N] [--requests N] [--delay-ms N] [--unsafe] [--quiet] [--web DIR]\n", argv[0]); return 1; }
    }
    if (num_workers < 1 || num_workers > MAX_WORKERS) num_workers = 4;
    if (requests < 2 || requests > 400) requests = 40;
    build_trains();
    signal(SIGPIPE, SIG_IGN);
    start_workers();

    if (!strcmp(mode, "simulate")) {
        if (!dm_set) delay_ms = 20;
        printf("=== Railway Reservation - console simulation ===\nRequests: %d | Workers: %d | Lock: %s\nAll passengers fight for train 12345 Chennai Express (MAS -> NDLS), 7 days from today\n\n", requests, num_workers, use_lock ? "ON (safe)" : "OFF (unsafe demo)");
        int conf; double ad, ap;
        run_burst(requests / 2, requests - requests / 2, &conf, &ad, &ap, NULL, NULL);
        stop_workers();
        printf("\n--- Summary ---\nRequests            : %d (half direct, half partner)\nConfirmed           : %d\nRejected            : %d\n", requests, st_conf, st_rej);
        printf("Groups seated together: %d\nGroups split (not together): %d\n", st_together, st_split);
        printf("Avg service position: partner %.1f  vs  direct %.1f  (lower = served earlier)\n\n", ap, ad);
        print_audit();
        return 0;
    }

    pthread_t rt; pthread_create(&rt, NULL, reaper_thread, NULL); pthread_detach(rt);   /* seat-hold timeout thread */
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY); addr.sin_port = htons(port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(ls, 64) < 0) { perror("bind/listen"); return 1; }
    printf("=== Railway Reservation System (%s mode) ===\n", use_lock ? "safe" : "UNSAFE");
    printf("Railway Portal (main) : http://localhost:%d/\n", port);
    printf("MakeMyTrip partner    : http://localhost:%d/makemytrip\n", port);
    printf("Goibibo partner       : http://localhost:%d/goibibo\n", port);
    printf("Trains loaded: %d | Stations: %d | Workers: %d | Press Ctrl+C to stop\n\n", ntrains, NSTATIONS, num_workers);
    for (;;) {
        int *cfd = malloc(sizeof *cfd);
        *cfd = accept(ls, NULL, NULL);
        if (*cfd < 0) { free(cfd); continue; }
        pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, conn_thread, cfd)) { close(*cfd); free(cfd); }
        pthread_attr_destroy(&at);
    }
}
