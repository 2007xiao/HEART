/*
 * heart.c - 会呼吸、会闪烁的彩色爱心（纯 C，无第三方库）
 *
 * 平台：Windows 10+ / Windows Terminal（推荐），Linux / macOS 终端。
 * 编译（MinGW-w64 / GCC）：
 *     gcc -std=c11 -O2 -Wall -Wextra -pedantic heart.c -o heart.exe -lm
 * 运行：
 *     heart.exe
 *     heart.exe --duration 0          # 无限循环，Ctrl+C 退出
 *     heart.exe --static              # 只画一帧
 *     heart.exe --static --ascii --no-color --no-cjk   # 纯 ASCII 降级
 *
 * 视觉实现：
 *   - 心形由隐函数 (x^2+y^2-1)^3 - x^2*y^3 <= 0 生成；
 *   - 每个字符单元格使用 Unicode 上半块 "▀"，前后景色各表示一个像素，
 *     相当于把终端纵向分辨率翻倍；
 *   - 颜色采用 24 位真彩色渐变 + 边界霓虹 + 心腔高光；
 *   - 心跳用“lub-dub”双脉冲包络驱动整体缩放、亮度和光晕；
 *   - 星尘、闪烁粒子与深色渐变背景增强氛围；
 *   - 爱心中间嵌入 “欣心相印” 铭牌，可通过 HEART_TEXT 修改。
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#  ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#    define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#  endif
#else
#  include <unistd.h>
#  include <sys/ioctl.h>
#endif

/* ============================ 可调参数 ============================ */

#define HEART_TEXT              "欣心相印"
#define HEART_TEXT_FALLBACK     "XIN XIN"

#define DEFAULT_DURATION        12.0    /* 秒；0 表示无限循环 */
#define DEFAULT_FPS             24.0    /* 帧率；只影响流畅度，不影响节奏 */
#define DEFAULT_BPM             72.0    /* 每分钟心跳次数 */
#define DEFAULT_SPARKLES        28      /* 闪烁粒子数量 */
#define MAX_SPARKLES            200

#define HEART_CENTER_Y          0.118   /* 隐函数心形的几何中心略偏上 */
#define HEART_BEAT_SCALE_X      0.070   /* 收缩/扩张幅度：横向 */
#define HEART_BEAT_SCALE_Y      0.052   /* 收缩/扩张幅度：纵向 */
#define HEART_RIM_WIDTH         0.055   /* 心形边缘霓虹宽度（归一化坐标） */

#define MAX_AUTO_WIDTH          120
#define MAX_AUTO_HEIGHT         44
#define MIN_AUTO_WIDTH          24
#define MIN_AUTO_HEIGHT         8

#define HEART_PI                3.14159265358979323846
#define HEART_TAU               (2.0 * HEART_PI)

/* ============================== 基础类型 ============================== */

typedef enum {
    COLOR_TRUECOLOR = 0,
    COLOR_256,
    COLOR_16,
    COLOR_NONE
} ColorMode;

typedef struct {
    double r, g, b;       /* 0..255 浮点颜色，便于混合和光晕叠加 */
} Color;

typedef struct {
    unsigned char r, g, b;
} RGB;

typedef struct {
    double duration;
    double fps;
    double bpm;
    int sparkles;
    int width;
    int height;
    int ascii;
    int static_only;
    int no_cjk;
    int color_explicit;
    ColorMode color;
} Options;

static double clampd(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static Color color_make(double r, double g, double b)
{
    Color c;
    c.r = r;
    c.g = g;
    c.b = b;
    return c;
}

static Color color_add(Color a, Color b)
{
    return color_make(a.r + b.r, a.g + b.g, a.b + b.b);
}

static Color color_mul(Color a, double k)
{
    return color_make(a.r * k, a.g * k, a.b * k);
}

static Color color_mix(Color a, Color b, double t)
{
    t = clampd(t, 0.0, 1.0);
    return color_make(a.r + (b.r - a.r) * t,
                      a.g + (b.g - a.g) * t,
                      a.b + (b.b - a.b) * t);
}

static unsigned char color_to_u8(double v)
{
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return (unsigned char)(v + 0.5);
}

static RGB color_to_rgb(Color c)
{
    RGB out;
    out.r = color_to_u8(c.r);
    out.g = color_to_u8(c.g);
    out.b = color_to_u8(c.b);
    return out;
}

static double smoothstep(double edge0, double edge1, double x)
{
    double t;
    if (edge0 == edge1) return x < edge0 ? 0.0 : 1.0;
    t = clampd((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/* ============================== 输出缓冲 ============================== */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int failed;
} Buf;

static void buf_init(Buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->failed = 0;
}

static void buf_free(Buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int buf_reserve(Buf *b, size_t extra)
{
    size_t need;
    size_t next;
    char *p;

    if (b->failed) return 0;
    if (extra > (size_t)-1 - b->len - 1) {
        b->failed = 1;
        return 0;
    }
    need = b->len + extra + 1;
    if (need <= b->cap) return 1;

    next = b->cap ? b->cap : 65536;
    while (next < need) {
        if (next > ((size_t)-1) / 2) {
            next = need;
            break;
        }
        next *= 2;
    }
    p = (char *)realloc(b->data, next);
    if (!p) {
        b->failed = 1;
        return 0;
    }
    b->data = p;
    b->cap = next;
    return 1;
}

static void buf_putn(Buf *b, const char *s, size_t n)
{
    if (!buf_reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_puts(Buf *b, const char *s)
{
    buf_putn(b, s, strlen(s));
}

static void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_list ap2;
    int needed;
    char stack_buf[160];

    va_start(ap, fmt);
    va_copy(ap2, ap);
    needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);

    if (needed < 0) {
        b->failed = 1;
        va_end(ap2);
        return;
    }
    if ((size_t)needed < sizeof(stack_buf)) {
        buf_putn(b, stack_buf, (size_t)needed);
    } else {
        if (buf_reserve(b, (size_t)needed)) {
            vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap2);
            b->len += (size_t)needed;
        }
    }
    va_end(ap2);
}

/* ============================== 终端控制 ============================== */

typedef struct {
    int ansi;
    int tty;
    int console;
#ifdef _WIN32
    HANDLE out;
    DWORD saved_mode;
    UINT saved_cp;
    int saved_stdout_mode;
    int have_mode;
    int have_cp;
    int have_stdout_mode;
#else
    int unused;
#endif
} Terminal;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void term_init(Terminal *t)
{
    memset(t, 0, sizeof(*t));
#ifdef _WIN32
    {
        int fd = _fileno(stdout);
        DWORD mode = 0;

        t->out = GetStdHandle(STD_OUTPUT_HANDLE);
        t->saved_stdout_mode = _setmode(fd, _O_BINARY);
        if (t->saved_stdout_mode != -1) t->have_stdout_mode = 1;
        t->tty = (_isatty(fd) != 0);

        if (t->out != INVALID_HANDLE_VALUE &&
            GetConsoleMode(t->out, &mode)) {
            t->console = 1;
            t->saved_mode = mode;
            t->have_mode = 1;
            if (SetConsoleMode(t->out, mode | ENABLE_PROCESSED_OUTPUT |
                                           ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                t->ansi = 1;
            } else {
                t->ansi = 0;
            }
            t->saved_cp = GetConsoleOutputCP();
            if (SetConsoleOutputCP(CP_UTF8)) t->have_cp = 1;
        } else {
            /* 被重定向到管道/文件：仍然输出 ANSI，便于录屏或日志复现。 */
            t->console = 0;
            t->ansi = 1;
        }
    }
#else
    t->tty = (isatty(STDOUT_FILENO) != 0);
    t->ansi = 1;
#endif
}

static void term_restore(Terminal *t)
{
#ifdef _WIN32
    if (t->have_cp) SetConsoleOutputCP(t->saved_cp);
    if (t->have_mode) SetConsoleMode(t->out, t->saved_mode);
    if (t->have_stdout_mode) _setmode(_fileno(stdout), t->saved_stdout_mode);
#else
    (void)t;
#endif
}

static void term_write(const char *s, size_t n)
{
    size_t off = 0;
    while (off < n) {
        size_t written = fwrite(s + off, 1, n - off, stdout);
        if (written == 0) break;
        off += written;
    }
    fflush(stdout);
}

static double now_seconds(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int have_freq = 0;
    LARGE_INTEGER counter;
    if (!have_freq) {
        QueryPerformanceFrequency(&freq);
        have_freq = 1;
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9;
#endif
}

static void sleep_ms(int ms)
{
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    {
        struct timespec req;
        req.tv_sec = ms / 1000;
        req.tv_nsec = (long)(ms % 1000) * 1000000L;
        nanosleep(&req, NULL);
    }
#endif
}

static int env_int(const char *name, int fallback)
{
    const char *s = getenv(name);
    char *end = NULL;
    long v;
    if (!s || !*s) return fallback;
    v = strtol(s, &end, 10);
    if (end == s || v <= 0 || v > 100000) return fallback;
    return (int)v;
}

static void get_terminal_size(const Options *opt, Terminal *term, int *out_w, int *out_h)
{
    int cols = 0;
    int rows = 0;

    (void)term;

    if (opt->width > 0 && opt->height > 0) {
        *out_w = opt->width;
        *out_h = opt->height;
        return;
    }

#ifdef _WIN32
    if (term->console) {
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(term->out, &info)) {
            cols = info.srWindow.Right - info.srWindow.Left + 1;
            rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        }
    }
#else
    {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
            cols = ws.ws_col;
            rows = ws.ws_row;
        }
    }
#endif

    if (cols <= 0) cols = env_int("COLUMNS", 0);
    if (rows <= 0) rows = env_int("LINES", 0);
    if (cols <= 0) cols = 100;
    if (rows <= 0) rows = 36;

    /* 留一列/一行余量，避免刚好写满终端时触发自动换行和滚动。 */
    cols -= 1;
    rows -= 1;
    cols = clampi(cols, MIN_AUTO_WIDTH, MAX_AUTO_WIDTH);
    rows = clampi(rows, MIN_AUTO_HEIGHT, MAX_AUTO_HEIGHT);
    *out_w = cols;
    *out_h = rows;
}

/* ============================== 随机与粒子 ============================== */

typedef struct {
    uint64_t state;
} Rng;

static uint64_t rng_next(Rng *r)
{
    uint64_t x = r->state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->state = x;
    return x * UINT64_C(2685821657736338717);
}

static double rng_double(Rng *r)
{
    return (double)(rng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

typedef struct {
    double x, y;          /* 归一化心形坐标 */
    double phase;
    double speed;
    double power;
    Color tint;
} Sparkle;

typedef struct {
    double x, y;          /* 屏幕归一化坐标 0..1 */
    double phase;
    double speed;
    double power;
    Color tint;
} Star;

/* ============================== 场景与单元格 ============================== */

typedef enum {
    CELL_HALF = 0,        /* 上半块，前后景各一个像素 */
    CELL_ASCII,           /* 彩色/单色 ASCII 字符，背景透明 */
    CELL_SOLID,           /* 纯背景色块（爱心中间的铭牌） */
    CELL_TEXT,            /* UTF-8 文本起始单元格 */
    CELL_CONT,            /* 宽字符占用的第二列 */
    CELL_MARK             /* 闪烁粒子叠加字符 */
} CellKind;

typedef struct {
    RGB fg;
    RGB bg;
    char glyph[8];
    unsigned char kind;
    unsigned char bg_default;
} Cell;

typedef struct {
    int w;
    int h;
    int ph;               /* 像素高 = 2 * 字符行高 */
    double scale;
    double cx;
    double cy;
    Cell *cells;          /* w * h */
    Color *pix;           /* w * ph */
    float *cover;         /* w * ph，心形覆盖率，用于星尘遮挡 */
    Buf out;
} Scene;

static int scene_alloc(Scene *s, int w, int h)
{
    size_t cell_count;
    size_t pix_count;

    if (w < 1 || h < 1) return 0;
    cell_count = (size_t)w * (size_t)h;
    pix_count = (size_t)w * (size_t)h * 2u;

    free(s->cells);
    free(s->pix);
    free(s->cover);
    s->cells = (Cell *)calloc(cell_count, sizeof(Cell));
    s->pix = (Color *)calloc(pix_count, sizeof(Color));
    s->cover = (float *)calloc(pix_count, sizeof(float));
    if (!s->cells || !s->pix || !s->cover) {
        free(s->cells);
        free(s->pix);
        free(s->cover);
        s->cells = NULL;
        s->pix = NULL;
        s->cover = NULL;
        return 0;
    }

    s->w = w;
    s->h = h;
    s->ph = h * 2;
    s->cx = (double)(w - 1) * 0.5;
    s->cy = (double)(s->ph - 1) * 0.5;

    {
        double sx = (double)(w > 3 ? w - 2 : 1) / 2.42;
        double sy = (double)(s->ph > 3 ? s->ph - 2 : 1) / 2.40;
        s->scale = (sx < sy) ? sx : sy;
        if (s->scale < 2.0) s->scale = 2.0;
    }
    return 1;
}

static void scene_free(Scene *s)
{
    free(s->cells);
    free(s->pix);
    free(s->cover);
    buf_free(&s->out);
    memset(s, 0, sizeof(*s));
}

/* ============================== 心形数学 ============================== */

static double heart_field(double x, double y)
{
    double a = x * x + y * y - 1.0;
    return a * a * a - x * x * y * y * y;
}

/* 用 |f| / |grad f| 近似到边界的归一化距离（内部为正）。 */
static double heart_sdf(double x, double y)
{
    const double e = 0.0015;
    double f0 = heart_field(x, y);
    double dx = (heart_field(x + e, y) - heart_field(x - e, y)) / (2.0 * e);
    double dy = (heart_field(x, y + e) - heart_field(x, y - e)) / (2.0 * e);
    double g = sqrt(dx * dx + dy * dy);
    if (g < 1.0e-9) return 0.0;
    return -f0 / g;
}

/* 双脉冲“lub-dub”包络，0..1。 */
static double heartbeat(double t, double bpm)
{
    double phase = t * (bpm / 60.0);
    double a;
    double b;
    phase -= floor(phase);
    a = exp(-pow((phase - 0.075) / 0.046, 2.0));
    b = 0.56 * exp(-pow((phase - 0.235) / 0.060, 2.0));
    if (a + b > 1.0) return 1.0;
    return a + b;
}

/* ============================== 着色 ============================== */

static Color shade_pixel(double x0, double y0, double beat, double t,
                         double vignette, double *coverage)
{
    double sx = 1.0 + HEART_BEAT_SCALE_X * beat;
    double sy = 1.0 + HEART_BEAT_SCALE_Y * beat;
    double x = x0 / sx;
    double y = (y0 - HEART_CENTER_Y) / sy + HEART_CENTER_Y;
    double dist;
    double aa;
    double yy;
    Color bg;
    Color heart;
    Color result;
    double halo_out;
    double wide_out;
    double rim;
    double center_glow;
    double spec;
    double sheen;

    dist = heart_sdf(x, y);     /* 内部 > 0，外部 < 0 */

    /* 边界抗锯齿：一个像素宽左右。 */
    aa = smoothstep(-0.018, 0.018, dist);

    /* 深紫黑背景 + 径向微光。 */
    bg = color_make(0.7, 0.15, 3.1);
    bg = color_add(bg, color_mul(color_make(15.0, 2.0, 30.0),
                                 exp(-(x0 * x0 * 0.65 +
                                       (y0 - HEART_CENTER_Y) *
                                       (y0 - HEART_CENTER_Y) * 0.55) * 1.6)));
    bg = color_mul(bg, 0.25 + 0.75 * vignette);

    /* 心形外侧的光晕和一道细霓虹边。 */
    halo_out = exp(-fabs(dist) * 5.3);
    wide_out = exp(-fabs(dist) * 2.0);
    bg = color_add(bg, color_mul(color_make(150.0, 9.0, 58.0),
                                 0.42 * halo_out * (0.50 + 0.50 * beat)));
    bg = color_add(bg, color_mul(color_make(52.0, 1.0, 46.0),
                                 0.30 * wide_out));
    bg = color_add(bg, color_mul(color_make(255.0, 64.0, 132.0),
                                 0.22 * exp(-fabs(dist) * 24.0)));

    /* 垂直方向的基础渐变：底部深红 -> 中部洋红 -> 顶部珊瑚粉。 */
    yy = clampd((y + 0.94) / 2.15, 0.0, 1.0);
    heart = color_mix(color_make(112.0, 0.0, 34.0),
                      color_make(224.0, 17.0, 70.0),
                      smoothstep(0.00, 0.46, yy));
    heart = color_mix(heart,
                      color_make(255.0, 96.0, 136.0),
                      smoothstep(0.56, 1.00, yy));

    /* 心腔中心的暖光。 */
    center_glow = exp(-(x * x * 1.55 +
                       (y - HEART_CENTER_Y) * (y - HEART_CENTER_Y) * 2.20) * 5.0);
    heart = color_add(heart, color_mul(color_make(255.0, 92.0, 108.0),
                                       0.27 * center_glow));

    /* 下半部压暗，让心尖更沉、更有体积感。 */
    heart = color_mul(heart,
                      1.0 - 0.10 * (1.0 - smoothstep(-0.85, 0.15, y)));

    /* 边缘亮线。 */
    rim = exp(-fmax(dist, 0.0) / HEART_RIM_WIDTH);
    heart = color_add(heart, color_mul(color_make(255.0, 104.0, 146.0),
                                       0.92 * rim));

    /* 左上角高光，让心形有“湿润/发光”的质感。 */
    spec = exp(-((x + 0.38) * (x + 0.38) * 16.0 +
                 (y - 0.56) * (y - 0.56) * 20.0));
    heart = color_add(heart, color_mul(color_make(255.0, 205.0, 214.0),
                                       0.68 * spec));

    /* 缓慢流动的微光。 */
    sheen = 0.5 + 0.5 * sin((x * 2.6 - y * 1.8) * 3.0 + t * 1.3);
    heart = color_mul(heart, 0.92 + 0.10 * sheen + 0.16 * beat);

    result = color_mix(bg, heart, aa);
    *coverage = (double)aa;
    return result;
}

/* ============================== UTF-8 文本 ============================== */

static int utf8_decode(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t c;
    int len;
    int i;

    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    } else if ((p[0] & 0xE0) == 0xC0) {
        c = p[0] & 0x1Fu;
        len = 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        c = p[0] & 0x0Fu;
        len = 3;
    } else if ((p[0] & 0xF8) == 0xF0) {
        c = p[0] & 0x07u;
        len = 4;
    } else {
        *cp = 0xFFFD;
        return 1;
    }

    for (i = 1; i < len; ++i) {
        if ((p[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return i;
        }
        c = (c << 6) | (uint32_t)(p[i] & 0x3F);
    }
    *cp = c;
    return len;
}

static int codepoint_width(uint32_t cp)
{
    if (cp == 0) return 0;
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if (cp >= 0x300 && cp <= 0x36F) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

static int text_width(const char *s)
{
    int width = 0;
    while (*s) {
        uint32_t cp;
        int n = utf8_decode(s, &cp);
        if (n <= 0) break;
        width += codepoint_width(cp);
        s += n;
    }
    return width;
}

/* ============================== 场景绘制 ============================== */

static void make_sparkles(Sparkle *sp, int count)
{
    Rng rng;
    int i;
    rng.state = UINT64_C(0x9E3779B97F4A7C15);

    for (i = 0; i < count; ++i) {
        int tries = 0;
        double x, y;
        do {
            x = -1.16 + 2.32 * rng_double(&rng);
            y = -1.02 + 2.26 * rng_double(&rng);
            ++tries;
        } while (heart_field(x, y) > 0.20 && tries < 80);

        sp[i].x = x;
        sp[i].y = y;
        sp[i].phase = rng_double(&rng) * HEART_TAU;
        sp[i].speed = 0.8 + 2.4 * rng_double(&rng);
        sp[i].power = 0.45 + 0.55 * rng_double(&rng);
        {
            double pick = rng_double(&rng);
            if (pick < 0.26) {
                sp[i].tint = color_make(255.0, 240.0, 214.0);
            } else if (pick < 0.72) {
                sp[i].tint = color_make(255.0, 182.0, 208.0);
            } else {
                sp[i].tint = color_make(255.0, 220.0, 145.0);
            }
        }
    }
}

static void make_stars(Star *st, int count)
{
    Rng rng;
    int i;
    rng.state = UINT64_C(0xD1B54A32D192ED03);

    for (i = 0; i < count; ++i) {
        st[i].x = 0.02 + 0.96 * rng_double(&rng);
        st[i].y = 0.02 + 0.96 * rng_double(&rng);
        st[i].phase = rng_double(&rng) * HEART_TAU;
        st[i].speed = 0.4 + 1.6 * rng_double(&rng);
        st[i].power = 14.0 + 42.0 * rng_double(&rng);
        if (rng_double(&rng) < 0.60) {
            st[i].tint = color_make(255.0, 215.0, 225.0);
        } else if (rng_double(&rng) < 0.55) {
            st[i].tint = color_make(205.0, 210.0, 255.0);
        } else {
            st[i].tint = color_make(255.0, 235.0, 190.0);
        }
    }
}

static void add_star_pixels(Scene *s, int px, int py, Color tint, double amount)
{
    static const int dxs[5] = { 0, -1, 1, 0, 0 };
    static const int dys[5] = { 0, 0, 0, -1, 1 };
    static const double weights[5] = { 1.0, 0.02, 0.02, 0.015, 0.015 };
    int i;

    for (i = 0; i < 5; ++i) {
        int x = px + dxs[i];
        int y = py + dys[i];
        size_t idx;
        if (x < 0 || x >= s->w || y < 0 || y >= s->ph) continue;
        idx = (size_t)y * (size_t)s->w + (size_t)x;
        if (s->cover[idx] > 0.15) continue;
        s->pix[idx] = color_add(s->pix[idx],
                                color_mul(tint, amount * weights[i]));
    }
}

static void draw_stars(Scene *s, const Star *stars, int count, double t)
{
    int i;
    for (i = 0; i < count; ++i) {
        double twinkle = pow(0.5 + 0.5 * sin(t * stars[i].speed + stars[i].phase), 4.0);
        double amount = stars[i].power * (0.22 + 0.78 * twinkle);
        int px = (int)floor(stars[i].x * (double)(s->w - 1) + 0.5);
        int py = (int)floor(stars[i].y * (double)(s->ph - 1) + 0.5);
        add_star_pixels(s, px, py, stars[i].tint, amount);
    }
}

static const char *ascii_ramp(void)
{
    return " .`:-=+*#%@";
}

static void build_cells(Scene *s, int ascii_render)
{
    int row, col;

    for (row = 0; row < s->h; ++row) {
        for (col = 0; col < s->w; ++col) {
            size_t top_idx = ((size_t)row * 2u) * (size_t)s->w + (size_t)col;
            size_t bot_idx = top_idx + (size_t)s->w;
            Color top = s->pix[top_idx];
            Color bot = s->pix[bot_idx];
            RGB top_rgb = color_to_rgb(top);
            RGB bot_rgb = color_to_rgb(bot);
            Cell *cell = &s->cells[(size_t)row * (size_t)s->w + (size_t)col];

            memset(cell, 0, sizeof(*cell));
            cell->bg_default = 0;
            if (ascii_render) {
                double lum = (0.2126 * top.r + 0.7152 * top.g + 0.0722 * top.b +
                              0.2126 * bot.r + 0.7152 * bot.g + 0.0722 * bot.b) * 0.5;
                const char *ramp = ascii_ramp();
                int ramp_len = (int)strlen(ramp);
                int idx = (int)(lum / 255.0 * (double)(ramp_len - 1) + 0.5);
                idx = clampi(idx, 0, ramp_len - 1);
                cell->kind = CELL_ASCII;
                cell->bg_default = 1;
                cell->glyph[0] = ramp[idx];
                cell->glyph[1] = '\0';
                if (lum < 0.45) {
                    cell->glyph[0] = ' ';
                }
                cell->fg.r = color_to_u8((top.r + bot.r) * 0.5);
                cell->fg.g = color_to_u8((top.g + bot.g) * 0.5);
                cell->fg.b = color_to_u8((top.b + bot.b) * 0.5);
                cell->bg = cell->fg;
            } else {
                cell->kind = CELL_HALF;
                strcpy(cell->glyph, "\xE2\x96\x80"); /* U+2580 上半块 */
                cell->fg = top_rgb;
                cell->bg = bot_rgb;
            }
        }
    }
}

static void set_solid_cell(Cell *cell, Color c)
{
    RGB rgb = color_to_rgb(c);
    cell->kind = CELL_SOLID;
    cell->fg = rgb;
    cell->bg = rgb;
    cell->glyph[0] = ' ';
    cell->glyph[1] = '\0';
    cell->bg_default = 0;
}

static void draw_sparkles(Scene *s, const Sparkle *sp, int count,
                          double t, double beat, int ascii_render)
{
    static const char *spark_chars = "*+.";
    int i;

    for (i = 0; i < count; ++i) {
        double raw = 0.5 + 0.5 * sin(t * sp[i].speed + sp[i].phase);
        double intensity = sp[i].power * pow(raw, 4.0);
        double x;
        double y;
        int px;
        int py;
        int col;
        int row;
        Cell *cell;
        RGB fg;

        if (intensity < 0.18) continue;
        x = sp[i].x * (1.0 + HEART_BEAT_SCALE_X * beat) +
            0.020 * sin(t * 0.85 + sp[i].phase * 1.7);
        y = (sp[i].y - HEART_CENTER_Y) *
            (1.0 + HEART_BEAT_SCALE_Y * beat) + HEART_CENTER_Y +
            0.018 * cos(t * 0.72 + sp[i].phase);
        px = (int)floor(s->cx + x * s->scale + 0.5);
        py = (int)floor(s->cy - (y - HEART_CENTER_Y) * s->scale + 0.5);
        if (px < 0 || px >= s->w || py < 0 || py >= s->ph) continue;
        col = px;
        row = py / 2;
        if (col < 0 || col >= s->w || row < 0 || row >= s->h) continue;

        cell = &s->cells[(size_t)row * (size_t)s->w + (size_t)col];
        if (cell->kind == CELL_TEXT || cell->kind == CELL_CONT) continue;

        fg = color_to_rgb(color_mul(sp[i].tint, 0.65 + 0.35 * intensity));
        cell->kind = CELL_MARK;
        cell->fg = fg;
        cell->bg_default = ascii_render ? 1 : 0;
        if (intensity > 0.72) {
            cell->glyph[0] = spark_chars[0];
        } else if (intensity > 0.40) {
            cell->glyph[0] = spark_chars[1];
        } else {
            cell->glyph[0] = spark_chars[2];
        }
        cell->glyph[1] = '\0';
    }
}

static void draw_text_plate(Scene *s, const char *txt)
{
    int tw = text_width(txt);
    int banner_w = tw + 6;
    int col0;
    int rowc;
    int top;
    int bottom;
    int r;
    int c;
    Color border_top = color_make(112.0, 12.0, 50.0);
    Color border_bottom = color_make(58.0, 4.0, 27.0);
    Color fill = color_make(24.0, 1.0, 11.0);
    RGB text_fg = color_to_rgb(color_make(255.0, 234.0, 198.0));

    if (tw <= 0) return;
    if (banner_w > s->w - 2) banner_w = s->w - 2;
    if (banner_w < tw) return;         /* 极端窄屏时宁可不破坏图案 */
    if (s->h < 3) return;

    col0 = (s->w - banner_w) / 2;
    rowc = s->h / 2;
    top = clampi(rowc - 1, 0, s->h - 1);
    bottom = clampi(rowc + 1, 0, s->h - 1);

    for (r = top; r <= bottom; ++r) {
        for (c = col0; c < col0 + banner_w; ++c) {
            Cell *cell;
            Color plate;
            if (c < 0 || c >= s->w) continue;
            /* 圆角：上下两行两端留空，让铭牌像一块软胶标签。 */
            if ((r == top || r == bottom) &&
                (c == col0 || c == col0 + banner_w - 1)) {
                continue;
            }
            cell = &s->cells[(size_t)r * (size_t)s->w + (size_t)c];
            if (r == top) plate = border_top;
            else if (r == bottom) plate = border_bottom;
            else plate = fill;
            set_solid_cell(cell, plate);
        }
    }

    {
        int start_col = col0 + (banner_w - tw) / 2;
        int at = start_col;
        const char *p = txt;
        while (*p) {
            uint32_t cp;
            int n = utf8_decode(p, &cp);
            int cw = codepoint_width(cp);
            if (n <= 0) break;
            if (cw <= 0) {
                p += n;
                continue;
            }
            if (at + cw > col0 + banner_w || at >= s->w) break;
            if (at >= 0) {
                Cell *cell = &s->cells[(size_t)rowc * (size_t)s->w + (size_t)at];
                size_t copy = (size_t)n;
                if (copy > sizeof(cell->glyph) - 1) copy = sizeof(cell->glyph) - 1;
                set_solid_cell(cell, fill);
                cell->kind = CELL_TEXT;
                cell->fg = text_fg;
                cell->bg = color_to_rgb(fill);
                cell->bg_default = 0;
                memset(cell->glyph, 0, sizeof(cell->glyph));
                memcpy(cell->glyph, p, copy);
                if (cw == 2 && at + 1 < s->w) {
                    Cell *cont = &s->cells[(size_t)rowc * (size_t)s->w +
                                          (size_t)(at + 1)];
                    set_solid_cell(cont, fill);
                    cont->kind = CELL_CONT;
                    cont->glyph[0] = '\0';
                }
            }
            at += cw;
            p += n;
        }
    }
}

/* ============================== ANSI 颜色 ============================== */

static int nearest_xterm256(RGB c)
{
    int maxv = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    int minv = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);

    if (maxv - minv < 20 && c.r < 85) {
        int gray = (int)(((int)c.r * 299 + (int)c.g * 587 + (int)c.b * 114) /
                         1000.0 / 255.0 * 23.0 + 0.5);
        return 232 + clampi(gray, 0, 23);
    } else {
        static const int levels[6] = { 0, 95, 135, 175, 215, 255 };
        int ri = 0, gi = 0, bi = 0;
        int bestr = 1 << 30, bestg = 1 << 30, bestb = 1 << 30;
        int i;
        for (i = 0; i < 6; ++i) {
            int dr = (int)c.r - levels[i];
            int dg = (int)c.g - levels[i];
            int db = (int)c.b - levels[i];
            if (dr * dr < bestr) { bestr = dr * dr; ri = i; }
            if (dg * dg < bestg) { bestg = dg * dg; gi = i; }
            if (db * db < bestb) { bestb = db * db; bi = i; }
        }
        return 16 + 36 * ri + 6 * gi + bi;
    }
}

static int nearest_ansi16(RGB c)
{
    static const int pal[16][3] = {
        {   0,   0,   0 }, { 128,   0,   0 }, {   0, 128,   0 }, { 128, 128,   0 },
        {   0,   0, 128 }, { 128,   0, 128 }, {   0, 128, 128 }, { 192, 192, 192 },
        { 128, 128, 128 }, { 255,   0,   0 }, {   0, 255,   0 }, { 255, 255,   0 },
        {   0,   0, 255 }, { 255,   0, 255 }, {   0, 255, 255 }, { 255, 255, 255 }
    };
    int best = 0;
    int best_dist = 1 << 30;
    int i;
    for (i = 0; i < 16; ++i) {
        int dr = (int)c.r - pal[i][0];
        int dg = (int)c.g - pal[i][1];
        int db = (int)c.b - pal[i][2];
        int d = dr * dr + dg * dg + db * db;
        if (d < best_dist) {
            best_dist = d;
            best = i;
        }
    }
    return best;
}

static void append_fg(Buf *b, RGB c, ColorMode mode)
{
    if (mode == COLOR_TRUECOLOR) {
        buf_printf(b, "\x1b[38;2;%d;%d;%dm", c.r, c.g, c.b);
    } else if (mode == COLOR_256) {
        buf_printf(b, "\x1b[38;5;%dm", nearest_xterm256(c));
    } else if (mode == COLOR_16) {
        int idx = nearest_ansi16(c);
        if (idx < 8) buf_printf(b, "\x1b[%dm", 30 + idx);
        else buf_printf(b, "\x1b[%dm", 90 + (idx - 8));
    }
}

static void append_bg(Buf *b, RGB c, ColorMode mode)
{
    if (mode == COLOR_TRUECOLOR) {
        buf_printf(b, "\x1b[48;2;%d;%d;%dm", c.r, c.g, c.b);
    } else if (mode == COLOR_256) {
        buf_printf(b, "\x1b[48;5;%dm", nearest_xterm256(c));
    } else if (mode == COLOR_16) {
        int idx = nearest_ansi16(c);
        if (idx < 8) buf_printf(b, "\x1b[%dm", 40 + idx);
        else buf_printf(b, "\x1b[%dm", 100 + (idx - 8));
    }
}

static int rgb_equal(RGB a, RGB b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static void emit_frame(Buf *out, const Scene *s, const Options *opt,
                       const Terminal *term, int animate)
{
    int row;
    int col;
    int color_enabled = (opt->color != COLOR_NONE) && term->ansi;

    buf_puts(out, animate ? "\x1b[H" : "");
    if (animate) buf_puts(out, "\x1b[?2026h");

    for (row = 0; row < s->h; ++row) {
        RGB last_fg = { 0, 0, 0 };
        RGB last_bg = { 0, 0, 0 };
        int have_fg = 0;
        int have_bg = 0;
        int bg_default = 1;

        if (color_enabled) buf_puts(out, "\x1b[0m");

        for (col = 0; col < s->w; ++col) {
            const Cell *cell = &s->cells[(size_t)row * (size_t)s->w + (size_t)col];
            int is_space;

            if (cell->kind == CELL_CONT) continue;

            is_space = (cell->glyph[0] == ' ');
            if (color_enabled) {
                if (!is_space || cell->kind == CELL_SOLID) {
                    if (!have_fg || !rgb_equal(last_fg, cell->fg)) {
                        append_fg(out, cell->fg, opt->color);
                        last_fg = cell->fg;
                        have_fg = 1;
                    }
                }
                if (cell->kind == CELL_SOLID || cell->kind == CELL_TEXT ||
                    (cell->kind == CELL_HALF) ||
                    (cell->kind == CELL_MARK && !cell->bg_default)) {
                    if (!have_bg || !rgb_equal(last_bg, cell->bg)) {
                        append_bg(out, cell->bg, opt->color);
                        last_bg = cell->bg;
                        have_bg = 1;
                        bg_default = 0;
                    }
                } else if (!bg_default) {
                    buf_puts(out, "\x1b[49m");
                    bg_default = 1;
                    have_bg = 0;
                }
            }
            buf_puts(out, cell->glyph);
        }

        if (term->ansi) {
            buf_puts(out, "\x1b[0m");
            if (row != s->h - 1) {
                buf_puts(out, "\x1b[K\r\n");
            } else {
                if (!animate) buf_puts(out, "\x1b[K");
            }
        } else {
            if (row != s->h - 1) buf_puts(out, "\r\n");
        }
    }

    if (animate) {
        buf_puts(out, "\x1b[?2026l");
    } else {
        buf_puts(out, "\r\n");
    }
}

/* ============================== 主渲染 ============================== */

static void render_scene(Scene *s, const Options *opt,
                         const Sparkle *sparkles, int sparkle_count,
                         const Star *stars, int star_count,
                         double t)
{
    double beat = heartbeat(t, opt->bpm);
    int ascii_render = opt->ascii || opt->color == COLOR_NONE;
    int row, col, half;
    int py;

    for (row = 0; row < s->h; ++row) {
        for (half = 0; half < 2; ++half) {
            py = row * 2 + half;
            for (col = 0; col < s->w; ++col) {
                double x0 = ((double)col - s->cx) / s->scale;
                double y0 = (s->cy - (double)py) / s->scale + HEART_CENTER_Y;
                double nx = ((double)col - s->cx) / (0.5 * (double)s->w);
                double ny = ((double)py - s->cy) / (0.5 * (double)s->ph);
                double r = sqrt(nx * nx + ny * ny);
                double vignette = 1.0 - smoothstep(0.48, 1.08, r);
                double coverage = 0.0;
                Color c = shade_pixel(x0, y0, beat, t, vignette, &coverage);
                size_t idx = (size_t)py * (size_t)s->w + (size_t)col;
                s->pix[idx] = c;
                s->cover[idx] = (float)coverage;
            }
        }
    }

    draw_stars(s, stars, star_count, t);
    build_cells(s, ascii_render);
    draw_sparkles(s, sparkles, sparkle_count, t, beat, ascii_render);
    {
        const char *label = opt->no_cjk ? HEART_TEXT_FALLBACK : HEART_TEXT;
        draw_text_plate(s, label);
    }
}

static void draw_and_emit(Scene *s, const Options *opt,
                          const Sparkle *sparkles, int sparkle_count,
                          const Star *stars, int star_count,
                          const Terminal *term, double t, int animate)
{
    render_scene(s, opt, sparkles, sparkle_count, stars, star_count, t);
    s->out.len = 0;
    emit_frame(&s->out, s, opt, term, animate);
}

/* ============================== 命令行 ============================== */

static void print_usage(FILE *fp)
{
    fprintf(fp,
        "heart - a glowing, beating heart for your terminal\n"
        "\n"
        "Usage: heart [options]\n"
        "  -d, --duration SEC   run time in seconds (default %.1f; 0 = forever)\n"
        "  -f, --fps N          target frames per second (default %.0f)\n"
        "  -b, --bpm N          heartbeats per minute (default %.0f)\n"
        "  -p, --sparkles N     number of twinkling particles (default %d)\n"
        "  -s, --size WxH       force the drawing size in character cells\n"
        "  -c, --color MODE     truecolor | 256 | 16 | none\n"
        "      --no-color       same as --color none\n"
        "  -a, --ascii          ASCII characters instead of Unicode half blocks\n"
        "      --static         render a single frame and exit\n"
        "      --no-cjk         use \"XIN XIN\" instead of the Chinese phrase\n"
        "      --plain          static + ascii + no color + no CJK\n"
        "      --loop           infinite animation (same as --duration 0)\n"
        "  -h, --help           show this help\n"
        "\n"
        "Examples:\n"
        "  heart\n"
        "  heart --duration 0 --bpm 84\n"
        "  heart --static --size 100x36\n"
        "  heart --static --ascii --no-color --no-cjk\n",
        DEFAULT_DURATION, DEFAULT_FPS, DEFAULT_BPM, DEFAULT_SPARKLES);
}

static int parse_color(const char *s, ColorMode *mode)
{
    if (strcmp(s, "truecolor") == 0 || strcmp(s, "true") == 0 ||
        strcmp(s, "24bit") == 0 || strcmp(s, "24") == 0) {
        *mode = COLOR_TRUECOLOR;
    } else if (strcmp(s, "256") == 0) {
        *mode = COLOR_256;
    } else if (strcmp(s, "16") == 0) {
        *mode = COLOR_16;
    } else if (strcmp(s, "none") == 0 || strcmp(s, "off") == 0) {
        *mode = COLOR_NONE;
    } else {
        return 0;
    }
    return 1;
}

static int parse_double_value(const char *s, double *out)
{
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || *end != '\0' || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

static int parse_int_value(const char *s, int *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < -100000 || v > 100000) return 0;
    *out = (int)v;
    return 1;
}

static int parse_options(int argc, char **argv, Options *opt)
{
    int i;

    opt->duration = DEFAULT_DURATION;
    opt->fps = DEFAULT_FPS;
    opt->bpm = DEFAULT_BPM;
    opt->sparkles = DEFAULT_SPARKLES;
    opt->width = 0;
    opt->height = 0;
    opt->ascii = 0;
    opt->static_only = 0;
    opt->no_cjk = 0;
    opt->color_explicit = 0;
    opt->color = COLOR_TRUECOLOR;

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage(stdout);
            return 1;
        } else if (strcmp(a, "-d") == 0 || strcmp(a, "--duration") == 0) {
            if (++i >= argc) goto need_value;
            if (!parse_double_value(argv[i], &opt->duration) ||
                opt->duration < 0.0 || opt->duration > 86400.0) {
                fprintf(stderr, "heart: invalid duration: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(a, "-f") == 0 || strcmp(a, "--fps") == 0) {
            if (++i >= argc) goto need_value;
            if (!parse_double_value(argv[i], &opt->fps) ||
                opt->fps < 1.0 || opt->fps > 120.0) {
                fprintf(stderr, "heart: fps must be between 1 and 120\n");
                return -1;
            }
        } else if (strcmp(a, "-b") == 0 || strcmp(a, "--bpm") == 0) {
            if (++i >= argc) goto need_value;
            if (!parse_double_value(argv[i], &opt->bpm) ||
                opt->bpm < 20.0 || opt->bpm > 240.0) {
                fprintf(stderr, "heart: bpm must be between 20 and 240\n");
                return -1;
            }
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--sparkles") == 0) {
            if (++i >= argc) goto need_value;
            if (!parse_int_value(argv[i], &opt->sparkles) ||
                opt->sparkles < 0 || opt->sparkles > MAX_SPARKLES) {
                fprintf(stderr, "heart: sparkles must be 0..%d\n", MAX_SPARKLES);
                return -1;
            }
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--size") == 0) {
            int w, h;
            char tail;
            if (++i >= argc) goto need_value;
            if (sscanf(argv[i], "%dx%d%c", &w, &h, &tail) != 2 ||
                w < 20 || w > 200 || h < 6 || h > 80) {
                fprintf(stderr, "heart: size must look like WxH (20..200 x 6..80)\n");
                return -1;
            }
            opt->width = w;
            opt->height = h;
        } else if (strcmp(a, "-c") == 0 || strcmp(a, "--color") == 0) {
            if (++i >= argc) goto need_value;
            if (!parse_color(argv[i], &opt->color)) {
                fprintf(stderr, "heart: color must be truecolor, 256, 16 or none\n");
                return -1;
            }
            opt->color_explicit = 1;
        } else if (strcmp(a, "--no-color") == 0) {
            opt->color = COLOR_NONE;
            opt->color_explicit = 1;
        } else if (strcmp(a, "-a") == 0 || strcmp(a, "--ascii") == 0) {
            opt->ascii = 1;
        } else if (strcmp(a, "--static") == 0) {
            opt->static_only = 1;
        } else if (strcmp(a, "--no-cjk") == 0) {
            opt->no_cjk = 1;
        } else if (strcmp(a, "--loop") == 0) {
            opt->duration = 0.0;
        } else if (strcmp(a, "--plain") == 0) {
            opt->static_only = 1;
            opt->ascii = 1;
            opt->color = COLOR_NONE;
            opt->color_explicit = 1;
            opt->no_cjk = 1;
        } else {
            fprintf(stderr, "heart: unknown option: %s\n", a);
            fprintf(stderr, "Try 'heart --help'.\n");
            return -1;
        }
    }
    return 0;

need_value:
    fprintf(stderr, "heart: option %s needs a value\n", argv[i - 1]);
    return -1;
}

/* ============================== 入口 ============================== */

int main(int argc, char **argv)
{
    Options opt;
    Terminal term;
    Scene scene;
    Sparkle *sparkles = NULL;
    Star *stars = NULL;
    const int star_count = 86;
    int parse_result;
    double start_time;
    double t = 0.0;
    int exit_code = 0;

    parse_result = parse_options(argc, argv, &opt);
    if (parse_result > 0) return 0;
    if (parse_result < 0) return 2;

    if (!opt.color_explicit && getenv("NO_COLOR") && *getenv("NO_COLOR")) {
        opt.color = COLOR_NONE;
    }

    signal(SIGINT, on_signal);
#ifdef SIGTERM
    signal(SIGTERM, on_signal);
#endif

    term_init(&term);

    /*
     * TERM=dumb 或旧 Windows 控制台不支持 VT 时，降级为静态 ASCII。
     * 这条路径不输出任何光标/颜色控制字符。
     */
    if (!term.ansi) {
        fprintf(stderr,
                "heart: terminal has no ANSI/VT support; falling back to a static ASCII frame.\n");
        opt.static_only = 1;
        opt.ascii = 1;
        opt.color = COLOR_NONE;
        opt.no_cjk = 1;
        term.ansi = 0;
    } else {
        const char *term_env = getenv("TERM");
        if (term_env && strcmp(term_env, "dumb") == 0) {
            fprintf(stderr,
                    "heart: TERM=dumb; using the static ASCII fallback.\n");
            opt.static_only = 1;
            opt.ascii = 1;
            opt.color = COLOR_NONE;
            opt.no_cjk = 1;
        }
    }

    /*
     * 静态且关闭颜色时输出纯文本：不发送任何 ANSI 序列，
     * 可直接重定向到文件或在极简终端中查看。
     */
    if (opt.static_only && opt.color == COLOR_NONE) {
        term.ansi = 0;
    }
    scene.cells = NULL;
    scene.pix = NULL;
    scene.cover = NULL;
    buf_init(&scene.out);

    if (opt.sparkles > 0) {
        sparkles = (Sparkle *)calloc((size_t)opt.sparkles, sizeof(Sparkle));
        if (!sparkles) {
            fprintf(stderr, "heart: out of memory\n");
            term_restore(&term);
            return 1;
        }
        make_sparkles(sparkles, opt.sparkles);
    }
    stars = (Star *)calloc((size_t)star_count, sizeof(Star));
    if (!stars) {
        fprintf(stderr, "heart: out of memory\n");
        free(sparkles);
        term_restore(&term);
        return 1;
    }
    make_stars(stars, star_count);

    if (opt.static_only) {
        int w, h;
        get_terminal_size(&opt, &term, &w, &h);
        if (!scene_alloc(&scene, w, h)) {
            fprintf(stderr, "heart: out of memory\n");
            exit_code = 1;
            goto done;
        }
        render_scene(&scene, &opt, sparkles, opt.sparkles, stars, star_count,
                     0.21);
        scene.out.len = 0;
        emit_frame(&scene.out, &scene, &opt, &term, 0);
        term_write(scene.out.data, scene.out.len);
        goto done;
    }

    /* ---------- 动画模式 ---------- */
    {
        int w, h;
        get_terminal_size(&opt, &term, &w, &h);
        if (!scene_alloc(&scene, w, h)) {
            fprintf(stderr, "heart: out of memory\n");
            exit_code = 1;
            goto done;
        }

        buf_puts(&scene.out, "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H");
        term_write(scene.out.data, scene.out.len);
        scene.out.len = 0;

        start_time = now_seconds();
        while (!g_stop) {
            double current = now_seconds();
            double elapsed = current - start_time;
            int w2, h2;
            int target_frame;

            if (opt.duration > 0.0 && elapsed >= opt.duration) break;

            get_terminal_size(&opt, &term, &w2, &h2);
            if (w2 != scene.w || h2 != scene.h) {
                if (!scene_alloc(&scene, w2, h2)) {
                    fprintf(stderr, "heart: out of memory after resize\n");
                    exit_code = 1;
                    break;
                }
                scene.out.len = 0;
                buf_puts(&scene.out, "\x1b[2J\x1b[H");
                term_write(scene.out.data, scene.out.len);
                scene.out.len = 0;
            }

            t = elapsed;
            draw_and_emit(&scene, &opt, sparkles, opt.sparkles, stars,
                          star_count, &term, t, 1);
            term_write(scene.out.data, scene.out.len);

            target_frame = (int)floor(elapsed * opt.fps) + 1;
            while (!g_stop) {
                double target = start_time + (double)target_frame / opt.fps;
                double left = target - now_seconds();
                if (left <= 0.0) break;
                if (left > 0.010) left = 0.010;
                sleep_ms((int)(left * 1000.0) > 0 ? (int)(left * 1000.0) : 1);
            }
        }

        scene.out.len = 0;
        buf_puts(&scene.out, "\x1b[0m\x1b[?25h\x1b[?1049l");
        term_write(scene.out.data, scene.out.len);
    }

done:
    scene_free(&scene);
    free(sparkles);
    free(stars);
    term_restore(&term);
    return exit_code;
}
