// screensaver_seq.c
// Screensaver secuencial en C con SDL2.
// - Órbitas con estela y “respiración” de puntos.
// - Mandala (simetrías radiales y espejo).
// - Paletas: neon, ocean; look limpio por defecto.
// - HiDPI + SSAA; FPS en título; logging CSV.
// Compilar (macOS/Linux):
//   gcc -O2 -std=c11 secuencial/src/screensaver_seq.c $(pkg-config --cflags --libs sdl2) -lm -o secuencial/bin/screensaver_seq
// Ejecutar: ver `--help`. ESC para salir.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <math.h>
#include <ctype.h>

#if defined(_WIN32)
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** RGBA de 8 bits por canal. */
typedef struct
{
    Uint8 r, g, b, a;
} RGBA;

/** Configuración creada desde CLI. Todos los parámetros del render son parametrizables */
typedef struct
{
    int width;           // >= 640
    int height;          // >= 480
    int n;               // número de orbitadores
    int seconds;         // duración; <=0 => corre hasta ESC
    uint32_t seed;       // 0 => usa reloj
    char palette[16];    // "neon" (default), "sunset", "ocean", "candy", "mono"
    int vsync;           // 1=ON (default), 0=OFF
    char log_path[256];  // CSV para métricas; vacío = sin log
    int log_every_ms;    // periodo de muestreo del log (default 500 ms)
    int show_attractors; // 1=mostrar halos y líneas de atractores, 0=ocultar (default)
    float point_scale;   // escala global del tamaño de puntitos (default 1.8f)
    int sym;             // simetrías radiales (1..8), 1 = sin efecto
    int mirror;          // 0/1 espejo vertical adicional por simetría
    int ssaa;            // factor de supersampling (1=off, 2,3,4)
    // Controles de “clean” look:
    float sat_mul; // multiplicador de saturación global (0..1)
    int glow;      // 1=ADD glow (default), 0=limpio
    int bg_alpha;  // alpha del fade de fondo (0..255)
} Config;

/** Muestra ayuda de CLI con valores por defecto. */
static void print_usage(const char *exe)
{
    fprintf(stderr,
            "Uso: %s [--n N] [--width W] [--height H] [--seconds S] [--seed SEED] "
            "[--palette NAME] [--vsync 0|1] [--log PATH] [--log-every-ms MS] "
            "[--show-attractors 0|1] [--point-scale F] [--sym K] [--mirror 0|1] [--ssaa K] "
            "[--sat F] [--glow 0|1] [--bg-alpha A]\n"
            "Defaults: N=100, W=800, H=600, S=10 (<=0 infinito), SEED=now, "
            "PALETTE=neon, VSYNC=1, LOG_EVERY_MS=500, SHOW_ATTRACTORS=0, POINT_SCALE=1.0, "
            "SYM=6, MIRROR=1, SSAA=2, SAT=0.65, GLOW=0, BG_ALPHA=10\n"
            "Paletas: neon | ocean\n",
            exe);
}

/** Convierte entero desde string con validación; devuelve false si falla. */
static bool parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == NULL || *end != '\0')
        return false;
    if (v < INT_MIN || v > INT_MAX)
        return false;
    *out = (int)v;
    return true;
}
/** Convierte float desde string con validación; devuelve false si falla. */
static bool parse_float(const char *s, float *out)
{
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == NULL || *end != '\0')
        return false;
    *out = v;
    return true;
}
/** Comparación case-insensitive de C-strings. */
static bool str_ieq(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    while (*a && *b)
    {
        char ca = (char)tolower((unsigned char)*a++);
        char cb = (char)tolower((unsigned char)*b++);
        if (ca != cb)
            return false;
    }
    return *a == '\0' && *b == '\0';
}

/**
 * Parsea argumentos de línea de comandos y aplica programación defensiva:
 * - Valida rangos numéricos y corrige a valores seguros.
 * - Asegura mínimos de resolución/N.
 * - Normaliza paleta a {neon,ocean}.
 */
static Config parse_args(int argc, char **argv)
{
    Config cfg;
    cfg.width = 800;
    cfg.height = 600;
    cfg.n = 100;
    cfg.seconds = 10;
    cfg.seed = 0;
    snprintf(cfg.palette, sizeof(cfg.palette), "%s", "neon");
    cfg.vsync = 1;
    cfg.log_path[0] = '\0';
    cfg.log_every_ms = 500;
    cfg.show_attractors = 0;
    cfg.point_scale = 1.0f; // puntos pequeños por defecto
    cfg.sym = 6;            // mandala por defecto
    cfg.mirror = 1;         // espejo ON por defecto
    cfg.ssaa = 2;           // supersampling por defecto para limpieza
    cfg.sat_mul = 0.65f;    // menos saturación por defecto
    cfg.glow = 0;           // modo limpio por defecto
    cfg.bg_alpha = 10;      // menos “smear” en el fondo

    for (int i = 1; i < argc; ++i)
    {
        const char *a = argv[i];
#define NEED()                    \
    do                            \
    {                             \
        if (i + 1 >= argc)        \
        {                         \
            print_usage(argv[0]); \
            exit(1);              \
        }                         \
    } while (0)
        if (strcmp(a, "--n") == 0)
        {
            NEED();
            if (!parse_int(argv[++i], &cfg.n))
            {
                print_usage(argv[0]);
                exit(1);
            }
        }
        else if (strcmp(a, "--width") == 0)
        {
            NEED();
            if (!parse_int(argv[++i], &cfg.width))
            {
                print_usage(argv[0]);
                exit(1);
            }
        }
        else if (strcmp(a, "--height") == 0)
        {
            NEED();
            if (!parse_int(argv[++i], &cfg.height))
            {
                print_usage(argv[0]);
                exit(1);
            }
        }
        else if (strcmp(a, "--seconds") == 0)
        {
            NEED();
            if (!parse_int(argv[++i], &cfg.seconds))
            {
                print_usage(argv[0]);
                exit(1);
            }
        }
        else if (strcmp(a, "--seed") == 0)
        {
            int s;
            NEED();
            if (!parse_int(argv[++i], &s))
            {
                print_usage(argv[0]);
                exit(1);
            }
            cfg.seed = (uint32_t)s;
        }
        else if (strcmp(a, "--palette") == 0)
        {
            NEED();
            snprintf(cfg.palette, sizeof(cfg.palette), "%s", argv[++i]);
        }
        else if (strcmp(a, "--vsync") == 0)
        {
            int v;
            NEED();
            if (!parse_int(argv[++i], &v))
            {
                print_usage(argv[0]);
                exit(1);
            }
            cfg.vsync = v ? 1 : 0;
        }
        else if (strcmp(a, "--log") == 0)
        {
            NEED();
            snprintf(cfg.log_path, sizeof(cfg.log_path), "%s", argv[++i]);
        }
        else if (strcmp(a, "--log-every-ms") == 0)
        {
            NEED();
            if (!parse_int(argv[++i], &cfg.log_every_ms))
            {
                print_usage(argv[0]);
                exit(1);
            }
            if (cfg.log_every_ms < 1)
                cfg.log_every_ms = 1;
        }
        else if (strcmp(a, "--show-attractors") == 0)
        {
            int v;
            NEED();
            if (!parse_int(argv[++i], &v))
            {
                print_usage(argv[0]);
                exit(1);
            }
            cfg.show_attractors = v ? 1 : 0;
        }
        else if (strcmp(a, "--point-scale") == 0)
        {
            NEED();
            if (!parse_float(argv[++i], &cfg.point_scale))
            {
                print_usage(argv[0]);
                exit(1);
            }
            if (cfg.point_scale < 0.1f)
                cfg.point_scale = 0.1f;
        }
        else if (strcmp(a, "--sym") == 0)
        {
            int s;
            NEED();
            if (!parse_int(argv[++i], &s))
            {
                print_usage(argv[0]);
                exit(1);
            }
            if (s < 1)
                s = 1;
            if (s > 8)
                s = 8;
            cfg.sym = s;
        }
        else if (strcmp(a, "--mirror") == 0)
        {
            int v;
            NEED();
            if (!parse_int(argv[++i], &v))
            {
                print_usage(argv[0]);
                exit(1);
            }
            cfg.mirror = v ? 1 : 0;
        }
        else if (strcmp(a, "--ssaa") == 0)
        {
            int k;
            NEED();
            if (!parse_int(argv[++i], &k))
            {
                print_usage(argv[0]);
                exit(1);
            }
            if (k < 1)
                k = 1;
            if (k > 4)
                k = 4;
            cfg.ssaa = k;
        }
        else if (strcmp(a, "--sat") == 0)
        {
            float s;
            NEED();
            if (!parse_float(argv[++i], &s))
            {
                print_usage(argv[0]);
                exit(1);
            }
            if (s < 0)
                s = 0;
            if (s > 1)
                s = 1;
            cfg.sat_mul = s;
        }
        else if (strcmp(a, "--glow") == 0)
        {
            int v;
            NEED();
            if (!parse_int(argv[++i], &v))
            {
                print_usage(argv[0]);
                exit(1);
            }
            cfg.glow = v ? 1 : 0;
        }
        else if (strcmp(a, "--bg-alpha") == 0)
        {
            int aint;
            NEED();
            if (!parse_int(argv[++i], &aint))
            {
                print_usage(argv[0]);
                exit(1);
            }
            if (aint < 0)
                aint = 0;
            if (aint > 255)
                aint = 255;
            cfg.bg_alpha = aint;
        }
        else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0)
        {
            print_usage(argv[0]);
            exit(0);
        }
        else
        {
            fprintf(stderr, "Argumento no reconocido: %s\n", a);
            print_usage(argv[0]);
            exit(1);
        }
#undef NEED
    }

    // Normaliza paleta a {neon,ocean}
    if (!str_ieq(cfg.palette, "neon") && !str_ieq(cfg.palette, "ocean"))
    {
        snprintf(cfg.palette, sizeof(cfg.palette), "%s", "neon");
    }
    if (cfg.width < 640)
        cfg.width = 640;
    if (cfg.height < 480)
        cfg.height = 480;
    if (cfg.n < 1)
        cfg.n = 1;
    if (cfg.seed == 0)
        cfg.seed = (uint32_t)time(NULL);
    return cfg;
}

// ------------------------ Utilidades ------------------------
static float frand01(void) { return (float)rand() / (float)RAND_MAX; }
static float frand_range(float a, float b) { return a + (b - a) * frand01(); }
static Uint8 clamp_u8(int v)
{
    if (v < 0)
        v = 0;
    if (v > 255)
        v = 255;
    return (Uint8)v;
}

/** Conversión HSV→RGB en 8bpc. */
// HSV (0..360, 0..1, 0..1) a RGB 0..255
static void hsv2rgb(float h, float s, float v, Uint8 *r, Uint8 *g, Uint8 *b)
{
    float C = v * s, X = C * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - C;
    float R = 0, G = 0, B = 0;
    if (h < 60)
    {
        R = C;
        G = X;
        B = 0;
    }
    else if (h < 120)
    {
        R = X;
        G = C;
        B = 0;
    }
    else if (h < 180)
    {
        R = 0;
        G = C;
        B = X;
    }
    else if (h < 240)
    {
        R = 0;
        G = X;
        B = C;
    }
    else if (h < 300)
    {
        R = X;
        G = 0;
        B = C;
    }
    else
    {
        R = C;
        G = 0;
        B = X;
    }
    *r = clamp_u8((int)((R + m) * 255.0f));
    *g = clamp_u8((int)((G + m) * 255.0f));
    *b = clamp_u8((int)((B + m) * 255.0f));
}

/** Medidor de FPS con suavizado exponencial. */
typedef struct
{
    uint64_t last_ticks;
    double smoothed_fps;
    double alpha;
} FPSCounter;
static double ticks_to_seconds(uint64_t ticks) { return (double)ticks / (double)SDL_GetPerformanceFrequency(); }
static uint64_t ticks_to_ms_u64(uint64_t ticks)
{
    double ms = (double)ticks * 1000.0 / (double)SDL_GetPerformanceFrequency();
    if (ms < 0.0)
        ms = 0.0;
    return (uint64_t)(ms + 0.5);
}
/** Avanza el contador de FPS y retorna dt en segundos. */
static double fps_tick(FPSCounter *f, double *fps_inst)
{
    uint64_t now = SDL_GetPerformanceCounter();
    double dt = ticks_to_seconds(now - f->last_ticks);
    f->last_ticks = now;
    double inst = (dt > 0.0) ? (1.0 / dt) : 0.0;
    if (f->smoothed_fps <= 0.0)
        f->smoothed_fps = inst;
    else
        f->smoothed_fps = f->alpha * inst + (1.0 - f->alpha) * f->smoothed_fps;
    if (fps_inst)
        *fps_inst = inst;
    return dt;
}

// ------------------------ Atractores y Orbitadores ------------------------
/** Atractor con movimiento senoidal independiente en X/Y. */
typedef struct
{
    float x, y;     // posición actual
    float ax, ay;   // amplitudes
    float fx, fy;   // frecuencias (rad/seg)
    float phx, phy; // fases
} Attractor;

/** Partícula/Orbitador con dinámica resorte-amortiguador y parámetros estéticos. */
typedef struct
{
    float x, y;    // posición
    float px, py;  // posición previa (para trazar la estela)
    float vx, vy;  // velocidad
    int att;       // índice del atractor que sigue
    float angle;   // ángulo de la órbita (rad)
    float radius;  // radio de la órbita (px)
    float omega;   // velocidad angular (rad/seg)
    float k;       // constante de resorte
    float damping; // amortiguamiento
    // estética/pulso del puntito
    float size_base, size_amp, size_speed, size_phase;
} Orbiter;

#define NUM_ATTR 3

/** Inicializa los 3 atractores en el centro con amplitudes/fases aleatorias. */
static void init_attractors(Attractor a[NUM_ATTR], int W, int H)
{
    float cx = W * 0.5f, cy = H * 0.5f;
    for (int i = 0; i < NUM_ATTR; ++i)
    {
        a[i].x = cx;
        a[i].y = cy;
        a[i].ax = frand_range(W * 0.20f, W * 0.35f);
        a[i].ay = frand_range(H * 0.20f, H * 0.35f);
        float fx_hz = frand_range(0.05f, 0.15f), fy_hz = frand_range(0.05f, 0.15f);
        a[i].fx = 2.0f * (float)M_PI * fx_hz;
        a[i].fy = 2.0f * (float)M_PI * fy_hz;
        a[i].phx = frand_range(0.0f, (float)M_PI * 2.0f);
        a[i].phy = frand_range(0.0f, (float)M_PI * 2.0f);
    }
}
/** Actualiza el movimiento senoidal de los atractores. */
static void update_attractors(Attractor a[NUM_ATTR], float t, int W, int H)
{
    float cx = W * 0.5f, cy = H * 0.5f;
    for (int i = 0; i < NUM_ATTR; ++i)
    {
        a[i].x = cx + a[i].ax * sinf(a[i].fx * t + a[i].phx);
        a[i].y = cy + a[i].ay * sinf(a[i].fy * t + a[i].phy);
    }
}
/** Crea N orbitadores distribuidos en radios aleatorios y fases distintas. */
static void init_orbiters(Orbiter *o, int n, Attractor a[NUM_ATTR], int W, int H)
{
    const float minR = (float)((W < H ? W : H)) * 0.08f, maxR = (float)((W < H ? W : H)) * 0.38f;
    for (int i = 0; i < n; ++i)
    {
        o[i].att = i % NUM_ATTR;
        o[i].radius = frand_range(minR, maxR);
        o[i].angle = frand_range(0.0f, (float)M_PI * 2.0f);
        float hz = frand_range(0.04f, 0.35f);
        o[i].omega = 2.0f * (float)M_PI * hz;
        o[i].k = frand_range(4.0f, 10.0f);
        o[i].damping = frand_range(1.4f, 3.2f);
        float tx = a[o[i].att].x + cosf(o[i].angle) * o[i].radius;
        float ty = a[o[i].att].y + sinf(o[i].angle) * o[i].radius;
        o[i].x = o[i].px = tx;
        o[i].y = o[i].py = ty;
        o[i].vx = o[i].vy = 0.0f;
        o[i].size_base = frand_range(2.0f, 3.5f);
        o[i].size_amp = frand_range(1.2f, 2.8f);
        o[i].size_speed = frand_range(0.6f, 1.6f) * 2.0f * (float)M_PI;
        o[i].size_phase = frand_range(0.0f, 2.0f * (float)M_PI);
    }
}
/** Integra explícitamente la dinámica resorte-amortiguador. */
static void update_orbiters(Orbiter *o, int n, Attractor a[NUM_ATTR], float dt, int W, int H)
{
    (void)W;
    (void)H;
    for (int i = 0; i < n; ++i)
    {
        Orbiter *p = &o[i];
        p->px = p->x;
        p->py = p->y;
        p->angle += p->omega * dt;
        float tx = a[p->att].x + cosf(p->angle) * p->radius;
        float ty = a[p->att].y + sinf(p->angle) * p->radius;
        float ax = p->k * (tx - p->x) - p->damping * p->vx;
        float ay = p->k * (ty - p->y) - p->damping * p->vy;
        p->vx += ax * dt;
        p->vy += ay * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
    }
}

// ------------------------ Dibujo ------------------------
/** Disco sólido por scanlines (rápido y sin texturas). */
static void draw_filled_circle(SDL_Renderer *ren, int cx, int cy, int r)
{
    for (int dy = -r; dy <= r; ++dy)
    {
        int yy = cy + dy;
        int dx = (int)floorf(sqrtf((float)(r * r - dy * dy)));
        SDL_RenderDrawLine(ren, cx - dx, yy, cx + dx, yy);
    }
}
/** Halo radial aditivo de baja intensidad para atractores. */
static void draw_radial_glow(SDL_Renderer *ren, int cx, int cy, int base_r, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
    for (int i = 6; i >= 1; --i)
    {
        int rr = base_r + i * 6;
        Uint8 a = (Uint8)(8 + i * 10);
        SDL_SetRenderDrawColor(ren, r, g, b, a);
        draw_filled_circle(ren, cx, cy, rr);
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, r, g, b, 210);
    draw_filled_circle(ren, cx, cy, 3);
}

// ---- Paletas / Tintes ----
/** Color de atractores acorde a la paleta. */
static void palette_attractor_color(const char *pal, int k, float t, Uint8 *r, Uint8 *g, Uint8 *b)
{
    if (str_ieq(pal, "mono"))
    {
        Uint8 v = (Uint8)(180 + 40 * (k % 3));
        *r = *g = *b = v;
        return;
    }
    float base;
    if (str_ieq(pal, "sunset"))
        base = 20.0f;
    else if (str_ieq(pal, "ocean"))
        base = 190.0f;
    else if (str_ieq(pal, "candy"))
        base = 290.0f;
    else
        base = 0.0f;
    float hue = base + 20.0f * k + 10.0f * sinf(0.37f * t + k);
    hsv2rgb(fmodf(hue, 360.0f), 0.40f, 0.90f, r, g, b);
}

/** Color de partículas (neon u ocean) con control global de saturación. */
static void palette_color(const Config *cfg, int i, float t, Uint8 *r, Uint8 *g, Uint8 *b)
{
    float hue, sat, val;
    if (str_ieq(cfg->palette, "ocean"))
    {
        // Azules/teales con ligera variación temporal
        hue = 180.0f + fmodf((float)i * 3.5f + 18.0f * sinf(0.21f * t + i * 0.05f), 40.0f);
        sat = 0.65f + 0.20f * sinf(0.13f * t + i * 0.09f);
        val = 0.95f;
    }
    else
    { // neon (default)
        // Arco iris con golden-angle pero suavizado
        hue = fmodf((float)i * 137.508f + 90.0f * sinf(0.23f * t + i * 0.031f), 360.0f);
        if (hue < 0.0f)
            hue += 360.0f;
        sat = 0.85f;
        val = 1.00f;
    }
    sat *= cfg->sat_mul;
    if (sat < 0.0f)
        sat = 0.0f;
    if (sat > 1.0f)
        sat = 1.0f;
    hsv2rgb(hue, sat, val, r, g, b);
}

/** Tinte de fondo + alpha de “fade” para la estela. */
static RGBA palette_bg_tint(const Config *cfg, float t)
{
    Uint8 r = 0, g = 0, b = 0;
    Uint8 a = (Uint8)cfg->bg_alpha; // menor alpha => menos smear
    if (str_ieq(cfg->palette, "ocean"))
    {
        // azul oscuro muy sutil
        hsv2rgb(210.0f + 6.0f * sinf(0.10f * t), 0.25f, 0.16f, &r, &g, &b);
    }
    else
    {
        // neon: gris oscuro ligeramente frío
        hsv2rgb(200.0f, 0.10f, 0.14f, &r, &g, &b);
    }
    RGBA out = {r, g, b, a};
    return out;
}

/** Dibuja un frame completo: fade, partículas con simetría, y atractores opcionales. */
static void render_frame(SDL_Renderer *ren, Orbiter *o, int n, Attractor a[NUM_ATTR], int W, int H, float t, const Config *cfg)
{
    int symN = cfg->sym;
    if (symN < 1)
        symN = 1;
    if (symN > 8)
        symN = 8;
    float cx = W * 0.5f, cy = H * 0.5f;
    float cosA[8], sinA[8];
    for (int m = 0; m < symN; ++m)
    {
        float ang = 2.0f * (float)M_PI * (float)m / (float)symN;
        cosA[m] = cosf(ang);
        sinA[m] = sinf(ang);
    }
    float alpha_div = (float)((cfg->mirror ? 2 : 1) * symN);
    int glow_on = cfg->glow ? 1 : 0;

    // 1) Fade sutil (tinte por paleta)
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_Rect full = (SDL_Rect){0, 0, W, H};
    RGBA tint = palette_bg_tint(cfg, t);
    SDL_SetRenderDrawColor(ren, tint.r, tint.g, tint.b, tint.a);
    SDL_RenderFillRect(ren, &full);

    // 2) Puntos + colita + mandala
    for (int i = 0; i < n; ++i)
    {
        Uint8 rr, gg, bb;
        palette_color(cfg, i, t, &rr, &gg, &bb);
        float dx0 = o[i].x - cx, dy0 = o[i].y - cy, dxp = o[i].px - cx, dyp = o[i].py - cy;
        float spd = sqrtf(o[i].vx * o[i].vx + o[i].vy * o[i].vy);
        float breath = 0.5f + 0.5f * sinf(o[i].size_speed * t + o[i].size_phase);
        float base = o[i].size_base * cfg->point_scale, amp = o[i].size_amp * cfg->point_scale;
        int pr = (int)lroundf(base + amp * breath + fminf(2.0f, spd * 0.015f));
        if (pr < 1)
            pr = 1;
        if (pr > 3)
            pr = 3; // clamp pequeño => se ve HD

        float a_scale = glow_on ? 1.0f : 0.6f;
        Uint8 trailA = (Uint8)fmaxf(4.0f, (90.0f * a_scale) / alpha_div);
        Uint8 tailA0 = (Uint8)fmaxf(3.0f, (34.0f * a_scale) / alpha_div);
        Uint8 haloA = glow_on ? (Uint8)fmaxf(8.0f, 50.0f / alpha_div) : 0;
        Uint8 nucA = (Uint8)fmaxf(70.0f, (185.0f + 50.0f * breath) / alpha_div);

        for (int m = 0; m < symN; ++m)
        {
            float xr = cx + dx0 * cosA[m] - dy0 * sinA[m];
            float yr = cy + dx0 * sinA[m] + dy0 * cosA[m];
            float xpr = cx + dxp * cosA[m] - dyp * sinA[m];
            float ypr = cy + dxp * sinA[m] + dyp * cosA[m];

            for (int mirrorPass = 0; mirrorPass < (cfg->mirror ? 2 : 1); ++mirrorPass)
            {
                float X = (mirrorPass ? (2.0f * cx - xr) : xr), Y = yr;
                float XP = (mirrorPass ? (2.0f * cx - xpr) : xpr), YP = ypr;

                // estela
                SDL_SetRenderDrawBlendMode(ren, glow_on ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(ren, rr, gg, bb, trailA);
                SDL_RenderDrawLine(ren, (int)lroundf(XP), (int)lroundf(YP), (int)lroundf(X), (int)lroundf(Y));

                // colita (2)
                float ddx = X - XP, ddy = Y - YP;
                for (int c = 1; c <= 2; ++c)
                {
                    float tpos = (float)c / 4.0f;
                    int cxp = (int)lroundf(X - ddx * tpos), cyp = (int)lroundf(Y - ddy * tpos);
                    Uint8 ca = (Uint8)fmaxf(3.0f, (float)tailA0 / (float)c);
                    SDL_SetRenderDrawBlendMode(ren, glow_on ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(ren, rr, gg, bb, ca);
                    draw_filled_circle(ren, cxp, cyp, pr - c >= 1 ? pr - c : 1);
                }

                // halo
                if (haloA > 0)
                {
                    SDL_SetRenderDrawBlendMode(ren, glow_on ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(ren, rr, gg, bb, haloA);
                    draw_filled_circle(ren, (int)lroundf(X), (int)lroundf(Y), pr + 2);
                }

                // núcleo
                SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(ren, rr, gg, bb, nucA);
                draw_filled_circle(ren, (int)lroundf(X), (int)lroundf(Y), pr);
            }
        }
    }

    // 3) Atractores
    if (cfg->show_attractors)
    {
        for (int k = 0; k < NUM_ATTR; ++k)
        {
            Uint8 rr2, gg2, bb2;
            palette_attractor_color(cfg->palette, k, t, &rr2, &gg2, &bb2);
            draw_radial_glow(ren, (int)lroundf(a[k].x), (int)lroundf(a[k].y), 10, rr2, gg2, bb2);
        }
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
        for (int i = 0; i < NUM_ATTR; ++i)
        {
            int j = (i + 1) % NUM_ATTR;
            SDL_SetRenderDrawColor(ren, 255, 255, 255, 18);
            SDL_RenderDrawLine(ren, (int)lroundf(a[i].x), (int)lroundf(a[i].y),
                               (int)lroundf(a[j].x), (int)lroundf(a[j].y));
        }
    }
}

/** Punto de entrada: inicialización SDL, bucle principal, logging CSV y limpieza. */
int main(int argc, char **argv)
{
    Config cfg = parse_args(argc, argv);
    srand((unsigned)cfg.seed);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
    {
        fprintf(stderr, "Error SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow(
        "Screensaver (secuencial) - Inicializando…",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        cfg.width, cfg.height, SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win)
    {
        fprintf(stderr, "Error SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    int rflags = SDL_RENDERER_ACCELERATED | (cfg.vsync ? SDL_RENDERER_PRESENTVSYNC : 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, rflags);
    if (!ren)
    {
        fprintf(stderr, "Error SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    // HiDPI + mejor filtro
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2"); // best filtering
    int outW = cfg.width, outH = cfg.height;
    SDL_GetRendererOutputSize(ren, &outW, &outH);

    // SSAA render target
    SDL_Texture *rt = NULL;
    int RW = outW, RH = outH;
    if (cfg.ssaa > 1)
    {
        RW = outW * cfg.ssaa;
        RH = outH * cfg.ssaa;
        rt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, RW, RH);
        if (!rt)
        {
            fprintf(stderr, "No se pudo crear render target SSAA=%d (%dx%d). Continuo sin SSAA.\n", cfg.ssaa, RW, RH);
            cfg.ssaa = 1;
            RW = outW;
            RH = outH;
        }
    }

    // Fondo inicial negro
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderPresent(ren);

    // Mundo
    Attractor att[NUM_ATTR];
    init_attractors(att, outW, outH);
    Orbiter *orbs = (Orbiter *)malloc(sizeof(Orbiter) * (size_t)cfg.n);
    if (!orbs)
    {
        fprintf(stderr, "Sin memoria para %d orbitadores\n", cfg.n);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    init_orbiters(orbs, cfg.n, att, outW, outH);

    // Tiempo y FPS
    bool running = true;
    uint64_t t0 = SDL_GetPerformanceCounter();
    double t_sec = 0.0;
    FPSCounter fpsc;
    fpsc.last_ticks = SDL_GetPerformanceCounter();
    fpsc.smoothed_fps = 0.0;
    fpsc.alpha = 0.1;

    // Logger CSV
    FILE *logfp = NULL;
    uint64_t start_ticks = SDL_GetPerformanceCounter(), last_log_ms = 0;
    if (cfg.log_path[0] != '\0')
    {
        logfp = fopen(cfg.log_path, "w");
        if (!logfp)
            fprintf(stderr, "No se pudo abrir log '%s' para escritura.\n", cfg.log_path);
        else
        {
            fprintf(logfp, "time_s,smoothed_fps,fps_inst,n,width,height,palette,vsync\n");
            fflush(logfp);
        }
    }

    while (running)
    {
        if (cfg.seconds > 0)
        {
            uint64_t now = SDL_GetPerformanceCounter();
            double elapsed = ticks_to_seconds(now - t0);
            if (elapsed >= (double)cfg.seconds)
                running = false;
        }

        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT)
                running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                running = false;
        }

        double fps_inst = 0.0;
        double dt = fps_tick(&fpsc, &fps_inst);
        if (dt > 0.05)
            dt = 0.05;
        t_sec += dt;

        update_attractors(att, (float)t_sec, outW, outH);
        update_orbiters(orbs, cfg.n, att, (float)dt, outW, outH);

        if (logfp)
        {
            uint64_t now_ticks = SDL_GetPerformanceCounter();
            uint64_t elapsed_ms = ticks_to_ms_u64(now_ticks - start_ticks);
            if (elapsed_ms >= last_log_ms + (uint64_t)cfg.log_every_ms)
            {
                fprintf(logfp, "%.3f,%.3f,%.3f,%d,%d,%d,%s,%d\n",
                        t_sec, fpsc.smoothed_fps, fps_inst, cfg.n, cfg.width, cfg.height, cfg.palette, cfg.vsync);
                fflush(logfp);
                last_log_ms = elapsed_ms;
            }
        }

        if (cfg.ssaa > 1 && rt)
        {
            SDL_SetRenderTarget(ren, rt);
            SDL_RenderSetScale(ren, (float)cfg.ssaa, (float)cfg.ssaa);
            render_frame(ren, orbs, cfg.n, att, outW, outH, (float)t_sec, &cfg);
            SDL_RenderSetScale(ren, 1.0f, 1.0f);
            SDL_SetRenderTarget(ren, NULL);
            SDL_RenderCopy(ren, rt, NULL, NULL);
            SDL_RenderPresent(ren);
        }
        else
        {
            render_frame(ren, orbs, cfg.n, att, outW, outH, (float)t_sec, &cfg);
            SDL_RenderPresent(ren);
        }

        char title[320];
        snprintf(title, sizeof(title),
                 "Screensaver (secuencial) | FPS: %.1f | N=%d  win=%dx%d draw=%dx%d RT=%dx%d SSAA=%d | palette=%s | sat=%.2f bgA=%d glow=%d | pt=%.2f | attractors=%d | sym=%d | mirror=%d",
                 fpsc.smoothed_fps, cfg.n, cfg.width, cfg.height, outW, outH, RW, RH, cfg.ssaa,
                 cfg.palette, cfg.sat_mul, cfg.bg_alpha, cfg.glow, cfg.point_scale, cfg.show_attractors, cfg.sym, cfg.mirror);
        SDL_SetWindowTitle(win, title);
    }

    if (logfp)
        fclose(logfp);
    free(orbs);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}