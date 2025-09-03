/**
 * Screensaver (paralelo con OpenMP y sprites GPU via SDL2)
 * --------------------------------------------------------
 * Realiza un “screensaver” generativo con partículas (orbitadores) que siguen
 * atractores móviles mediante una dinámica resorte–amortiguador.
 * Optimiza el cálculo con paralelismo (OpenMP) y acelera el dibujo con sprites
 * (texturas de disco y halo) en GPU. Incluye:
 *   - Simetrías radiales y espejo para estética tipo mandala.
 *   - Paletas “neon” y “ocean” con control global de saturación.
 *   - SSAA (supersampling) opcional vía render target escalado.
 *   - Calidad adaptativa: ajusta SSAA, fracción de render y simetrías para
 *     intentar mantener un FPS objetivo.
 *   - Logging CSV de métricas de desempeño.
 *
 * Entrada por CLI (ver print_usage) y validación robusta (parse_args).
 * Requiere SDL2; usa OpenMP si está disponible (_OPENMP).
 */

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
#include <SDL.h> // En Windows suele instalarse como SDL.h
#else
#include <SDL2/SDL.h> // En Linux/macOS suele usarse <SDL2/SDL.h>
#endif

#ifdef _OPENMP
#include <omp.h> // Paralelismo de la física si está disponible
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* RGBA en 8 bits por canal. Representa color + opacidad. */
typedef struct
{
    Uint8 r, g, b, a; // Componentes en [0,255]
} RGBA;

/*
 * Config: parámetros leídos desde CLI y normalizados en parse_args().
 * Cumple con asegurar rangos mínimos/máximos, valores por defecto y
 * opciones estéticas y de rendimiento.
 */
typedef struct
{
    int width, height;   // Tamaño de ventana (mín 640x480)
    int n;               // Número de partículas/orbitadores
    int seconds;         // Duración; <=0 ejecuta hasta ESC/cerrar
    uint32_t seed;       // Semilla RNG; 0 => usa reloj
    char palette[16];    // "neon" | "ocean"
    int vsync;           // 1=ON, 0=OFF (tearing vs latencia)
    char log_path[256];  // Ruta a CSV para métricas (vacío => sin log)
    int log_every_ms;    // Período de muestreo del log (ms)
    int show_attractors; // 1=guías de atractores visibles
    float point_scale;   // Escala global del tamaño del punto
    int sym;             // Número de simetrías radiales [1..8]
    int mirror;          // 0/1 espejo vertical adicional
    int ssaa;            // Factor de supersampling [1..4]
    float sat_mul;       // Multiplicador de saturación [0..1]
    int glow;            // 0/1 halo aditivo (glow)
    int bg_alpha;        // Alpha del “fade” de fondo [0..255]
    int threads;         // Hilos OpenMP (0 => auto)
    // Controles extra de rendimiento/claridad:
    int trail;         // 0/1 traza línea de estela larga
    float render_frac; // Fracción de partículas a DIBUJAR (física corre para todas)
    int adapt;         // 0/1 calidad adaptativa para mantener FPS
    int target_fps;    // FPS objetivo para adaptación
} Config;

/** Muestra ayuda de CLI con defaults y opciones válidas. */
static void print_usage(const char *exe)
{
    fprintf(stderr,
            "Uso: %s [--n N] [--width W] [--height H] [--seconds S] [--seed SEED] "
            "[--palette NAME] [--vsync 0|1] [--log PATH] [--log-every-ms MS] "
            "[--show-attractors 0|1] [--point-scale F] [--sym K] [--mirror 0|1] [--ssaa K] "
            "[--sat F] [--glow 0|1] [--bg-alpha A] [--threads T] [--trail 0|1] "
            "[--render-frac F] [--adapt 0|1] [--target-fps FPS]\n"
            "Defaults: N=100, W=800, H=600, S=10, SEED=now, PALETTE=neon, VSYNC=1, "
            "LOG_EVERY_MS=500, SHOW_ATTRACTORS=0, POINT_SCALE=1.0, SYM=6, MIRROR=1, "
            "SSAA=2, SAT=0.65, GLOW=0, BG_ALPHA=10, THREADS=0(auto), TRAIL=0, "
            "RENDER_FRAC=1.0, ADAPT=0, TARGET_FPS=30\n"
            "Paletas: neon | ocean\n",
            exe);
}

/** Convierte string a int con validación; retorna true si es válido. */
static bool parse_int(const char *s, int *out)
{
    char *e = 0;
    long v = strtol(s, &e, 10);
    if (!e || *e)
        return false;
    if (v < INT_MIN || v > INT_MAX)
        return false;
    *out = (int)v;
    return true;
}

/** Convierte string a float con validación; retorna true si es válido. */
static bool parse_float(const char *s, float *out)
{
    char *e = 0;
    float v = strtof(s, &e);
    if (!e || *e)
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
        char ca = (char)tolower((unsigned char)*a++),
             cb = (char)tolower((unsigned char)*b++);
        if (ca != cb)
            return false;
    }
    return *a == '\0' && *b == '\0';
}

/**
 * Parsea y valida CLI. Aplica “clamps” de seguridad y normaliza paleta.
 * Asegura mínimos de resolución y partículas; inyecta semilla si es 0.
 */
static Config parse_args(int argc, char **argv)
{
    Config cfg;
    // Defaults
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
    cfg.point_scale = 1.0f;
    cfg.sym = 6;
    cfg.mirror = 1;
    cfg.ssaa = 2;
    cfg.sat_mul = 0.65f;
    cfg.glow = 0;
    cfg.bg_alpha = 10;
    cfg.threads = 0;
    cfg.trail = 0;
    cfg.render_frac = 1.0f;
    cfg.adapt = 0;
    cfg.target_fps = 30;

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
            cfg.sym = (s < 1 ? 1 : (s > 8 ? 8 : s));
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
            cfg.ssaa = (k < 1 ? 1 : (k > 4 ? 4 : k));
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
            cfg.sat_mul = (s < 0 ? 0 : (s > 1 ? 1 : s));
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
            cfg.bg_alpha = (aint < 0 ? 0 : (aint > 255 ? 255 : aint));
        }
        else if (strcmp(a, "--threads") == 0)
        {
            int t;
            NEED();
            if (!parse_int(argv[++i], &t))
            {
                print_usage(argv[0]);
                exit(1);
            }
            cfg.threads = (t < 0 ? 0 : t);
        }
        else if (strcmp(a, "--trail") == 0)
        {
            int v;
            NEED();
            if (!parse_int(argv[++i], &v))
            {
                print_usage(argv[0]);
                exit(1);
            }
            cfg.trail = v ? 1 : 0;
        }
        else if (strcmp(a, "--render-frac") == 0)
        {
            NEED();
            if (!parse_float(argv[++i], &cfg.render_frac))
            {
                print_usage(argv[0]);
                exit(1);
            }
            if (cfg.render_frac <= 0.0f)
                cfg.render_frac = 0.05f;
            if (cfg.render_frac > 1.0f)
                cfg.render_frac = 1.0f;
        }
        else if (strcmp(a, "--adapt") == 0)
        {
            int v;
            NEED();
            if (!parse_int(argv[++i], &v))
            {
                print_usage(argv[0]);
                exit(1);
            }
            cfg.adapt = v ? 1 : 0;
        }
        else if (strcmp(a, "--target-fps") == 0)
        {
            int v;
            NEED();
            if (!parse_int(argv[++i], &v))
            {
                print_usage(argv[0]);
                exit(1);
            }
            if (v < 10)
                v = 10;
            if (v > 144)
                v = 144;
            cfg.target_fps = v;
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
    // Normalización final
    if (!str_ieq(cfg.palette, "neon") && !str_ieq(cfg.palette, "ocean"))
        snprintf(cfg.palette, sizeof(cfg.palette), "%s", "neon");
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

// ------------------------ Utilidades numéricas ------------------------

/** Retorna uniforme en [0,1]. */
static float frand01(void) { return (float)rand() / (float)RAND_MAX; }

/** Retorna uniforme en [a,b]. */
static float frand_range(float a, float b) { return a + (b - a) * frand01(); }

/** Satura entero a [0,255] y lo castea a Uint8. */
static Uint8 clamp_u8(int v)
{
    if (v < 0)
        v = 0;
    if (v > 255)
        v = 255;
    return (Uint8)v;
}

/** Conversión HSV→RGB (h:0..360, s:0..1, v:0..1) a 8bpc. */
static void hsv2rgb(float h, float s, float v, Uint8 *r, Uint8 *g, Uint8 *b)
{
    float C = v * s;
    float X = C * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - C;
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

// ------------------------ FPS (tiempo y medición) ------------------------

/** Medición de FPS con suavizado exponencial (EMA). */
typedef struct
{
    uint64_t last_ticks; // Última marca de tiempo de alta resolución
    double smoothed_fps; // FPS suavizado
    double alpha;        // Factor EMA [0..1]
} FPSCounter;

/** Convierte ticks de alto rendimiento a segundos. */
static double ticks_to_seconds(uint64_t t) { return (double)t / (double)SDL_GetPerformanceFrequency(); }

/** Convierte ticks a milisegundos enteros con redondeo correcto. */
static uint64_t ticks_to_ms_u64(uint64_t t)
{
    double ms = (double)t * 1000.0 / (double)SDL_GetPerformanceFrequency();
    if (ms < 0.0)
        ms = 0.0;
    return (uint64_t)(ms + 0.5);
}

/** Avanza contador de FPS; retorna dt (s) y actualiza instantáneo/suavizado. */
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

// ------------------------ Mundo: Atractores y Orbitadores ------------------------

/** Atractor con movimiento senoidal independiente en X e Y. */
typedef struct
{
    float x, y;     // Posición actual
    float ax, ay;   // Amplitudes
    float fx, fy;   // Frecuencias (rad/s)
    float phx, phy; // Fases iniciales (rad)
} Attractor;

/**
 * Orbiter (partícula):
 *   - (x,y) posición; (px,py) posición previa para estela
 *   - (vx,vy) velocidad
 *   - att índice de atractor seguido
 *   - angle/radius/omega parametrización de la órbita alrededor del atractor
 *   - k/damping constantes del resorte y amortiguamiento
 *   - size_* control del “pulso” del punto (estética)
 */
typedef struct
{
    float x, y, px, py, vx, vy;
    int att;
    float angle, radius, omega, k, damping;
    float size_base, size_amp, size_speed, size_phase;
} Orbiter;

#define NUM_ATTR 3 // Se utilizan 3 atractores coordinados

/** Inicializa 3 atractores centrados con amplitudes/frecuencias/fases aleatorias. */
static void init_attractors(Attractor a[NUM_ATTR], int W, int H)
{
    float cx = W * 0.5f, cy = H * 0.5f;
    for (int i = 0; i < NUM_ATTR; ++i)
    {
        a[i].x = cx;
        a[i].y = cy;
        a[i].ax = frand_range(W * 0.20f, W * 0.35f);
        a[i].ay = frand_range(H * 0.20f, H * 0.35f);
        float fx_hz = frand_range(0.05f, 0.15f),
              fy_hz = frand_range(0.05f, 0.15f);
        a[i].fx = 2.0f * (float)M_PI * fx_hz; // Hz → rad/s
        a[i].fy = 2.0f * (float)M_PI * fy_hz;
        a[i].phx = frand_range(0.0f, (float)M_PI * 2.0f);
        a[i].phy = frand_range(0.0f, (float)M_PI * 2.0f);
    }
}

/** Actualiza posiciones senoidales de los atractores en el tiempo t (s). */
static void update_attractors(Attractor a[NUM_ATTR], float t, int W, int H)
{
    float cx = W * 0.5f, cy = H * 0.5f;
    for (int i = 0; i < NUM_ATTR; ++i)
    {
        a[i].x = cx + a[i].ax * sinf(a[i].fx * t + a[i].phx);
        a[i].y = cy + a[i].ay * sinf(a[i].fy * t + a[i].phy);
    }
}

/** Inicializa N orbitadores con radios/fases aleatorias y parámetros de pulso. */
static void init_orbiters(Orbiter *o, int n, Attractor a[NUM_ATTR], int W, int H)
{
    float minR = (float)((W < H ? W : H)) * 0.08f;
    float maxR = (float)((W < H ? W : H)) * 0.38f;
    for (int i = 0; i < n; ++i)
    {
        o[i].att = i % NUM_ATTR;
        o[i].radius = frand_range(minR, maxR);
        o[i].angle = frand_range(0.0f, (float)M_PI * 2.0f);
        float hz = frand_range(0.04f, 0.35f);
        o[i].omega = 2.0f * (float)M_PI * hz; // rad/s
        o[i].k = frand_range(4.0f, 10.0f);
        o[i].damping = frand_range(1.4f, 3.2f);
        float tx = a[o[i].att].x + cosf(o[i].angle) * o[i].radius;
        float ty = a[o[i].att].y + sinf(o[i].angle) * o[i].radius;
        o[i].x = o[i].px = tx;
        o[i].y = o[i].py = ty;
        o[i].vx = o[i].vy = 0.0f;
        // Parámetros estéticos de respiración del punto
        o[i].size_base = frand_range(2.0f, 3.5f);
        o[i].size_amp = frand_range(1.2f, 2.8f);
        o[i].size_speed = frand_range(0.6f, 1.6f) * 2.0f * (float)M_PI;
        o[i].size_phase = frand_range(0.0f, 2.0f * (float)M_PI);
    }
}

/**
 * Integra la física de N partículas en paralelo (OpenMP si está disponible).
 * Cada partícula realiza:
 *   - Avance de ángulo de órbita (omega*dt).
 *   - Cálculo de objetivo (tx,ty) en la órbita del atractor.
 *   - Fuerza de resorte k*(dest - pos) y amortiguamiento -damping*vel.
 *   - Integración explícita de velocidad y posición.
 */
static void update_orbiters_parallel(Orbiter *o, int n, Attractor a[NUM_ATTR], float dt, int threads)
{
#ifdef _OPENMP
    if (threads > 0)
        omp_set_num_threads(threads);
    else
        omp_set_num_threads(omp_get_max_threads());
#pragma omp parallel for schedule(static)
#endif
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

// ------------------------ Pre-cálculo para dibujo ------------------------

/** Adelanto de firma: calcula color de partícula según paleta y tiempo. */
static void particle_color(const Config *cfg, int i, float t, Uint8 *r, Uint8 *g, Uint8 *b);

/**
 * Precomp: datos precomputados por partícula para reducir trabajo en el bucle
 * de dibujo. Incluye:
 *   - deltas relativos al centro (actual y previo),
 *   - radio del punto (pr),
 *   - color actual (r,g,b).
 */
typedef struct
{
    float dx0, dy0, dxp, dyp; // Pos actuales y previas relativas al centro
    int pr;                   // Radio de punto [1..3]
    Uint8 r, g, b;            // Color actual
} Precomp;

/**
 * Precalcula deltas, radios y colores por partícula (paralelizable).
 * Reduce el costo durante el render al reusar estos valores.
 */
static void precalc_particles(const Config *cfg, const Orbiter *o, int n, float t, float cx, float cy, Precomp *out, int threads)
{
#ifdef _OPENMP
    if (threads > 0)
        omp_set_num_threads(threads);
#pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < n; ++i)
    {
        float dx0 = o[i].x - cx, dy0 = o[i].y - cy;
        float dxp = o[i].px - cx, dyp = o[i].py - cy;
        float spd = sqrtf(o[i].vx * o[i].vx + o[i].vy * o[i].vy);
        float breath = 0.5f + 0.5f * sinf(o[i].size_speed * t + o[i].size_phase);
        float base = o[i].size_base * cfg->point_scale;
        float amp = o[i].size_amp * cfg->point_scale;
        int pr = (int)lroundf(base + amp * breath + fminf(2.0f, spd * 0.015f));
        if (pr < 1)
            pr = 1;
        if (pr > 3)
            pr = 3;
        Uint8 rr, gg, bb;
        particle_color(cfg, i, t, &rr, &gg, &bb);
        out[i].dx0 = dx0;
        out[i].dy0 = dy0;
        out[i].dxp = dxp;
        out[i].dyp = dyp;
        out[i].pr = pr;
        out[i].r = rr;
        out[i].g = gg;
        out[i].b = bb;
    }
}

// ------------------------ Sprites: discos y halos ------------------------

/**
 * Genera textura de disco RGBA (radio r) para dibujar puntos vía GPU.
 * Usa superficie temporal y la convierte en textura, con blend habilitado.
 */
static SDL_Texture *make_disc_texture(SDL_Renderer *ren, int r)
{
    int D = r * 2 + 1;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, D, D, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s)
        return NULL;
    Uint32 *p = (Uint32 *)s->pixels;
    int pitch = s->pitch / 4;
    Uint32 on = SDL_MapRGBA(s->format, 255, 255, 255, 255);
    Uint32 off = SDL_MapRGBA(s->format, 255, 255, 255, 0);
    for (int y = 0; y < D; ++y)
    {
        for (int x = 0; x < D; ++x)
        {
            int dx = x - r, dy = y - r;
            int d2 = dx * dx + dy * dy;
            p[y * pitch + x] = (d2 <= r * r) ? on : off;
        }
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
    if (tex)
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

/**
 * Genera textura radial RGBA (D x D) para halos suaves (falloff ~ t^1.8).
 * Se colorea y modula alpha por partícula en tiempo de dibujo.
 */
static SDL_Texture *make_radial_texture(SDL_Renderer *ren, int D)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, D, D, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s)
        return NULL;
    Uint32 *p = (Uint32 *)s->pixels;
    int pitch = s->pitch / 4;
    Uint32 pix;
    for (int y = 0; y < D; ++y)
    {
        for (int x = 0; x < D; ++x)
        {
            float dx = x - (D - 1) * 0.5f;
            float dy = y - (D - 1) * 0.5f;
            float r = sqrtf(dx * dx + dy * dy);
            float t = fmaxf(0.0f, 1.0f - r / (D * 0.5f));
            Uint8 a = (Uint8)(255.0f * powf(t, 1.8f));
            pix = SDL_MapRGBA(s->format, 255, 255, 255, a);
            p[y * pitch + x] = pix;
        }
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
    if (tex)
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

// ------------------------ Paletas y colores ------------------------

/** Color de atractores según paleta (neon/ocean) con leve modulación temporal. */
static void palette_attractor_color(const char *pal, int k, float t, Uint8 *r, Uint8 *g, Uint8 *b)
{
    float base = str_ieq(pal, "ocean") ? 190.0f : 0.0f;
    float hue = base + 20.0f * k + 10.0f * sinf(0.37f * t + k);
    hsv2rgb(fmodf(hue, 360.0f), 0.40f, 0.90f, r, g, b);
}

/** Tinte de fondo por paleta + alpha de “fade” para arrastre global. */
static RGBA palette_bg_tint(const Config *cfg, float t)
{
    Uint8 r = 0, g = 0, b = 0, a = (Uint8)cfg->bg_alpha;
    if (str_ieq(cfg->palette, "ocean"))
        hsv2rgb(210.0f + 6.0f * sinf(0.10f * t), 0.25f, 0.16f, &r, &g, &b);
    else
        hsv2rgb(200.0f, 0.10f, 0.14f, &r, &g, &b);
    RGBA out = {r, g, b, a};
    return out;
}

/** Color de partícula por paleta con control de saturación global. */
static void particle_color(const Config *cfg, int i, float t, Uint8 *r, Uint8 *g, Uint8 *b)
{
    float hue, sat, val;
    if (str_ieq(cfg->palette, "ocean"))
    {
        hue = 180.0f + fmodf((float)i * 3.5f + 18.0f * sinf(0.21f * t + i * 0.05f), 40.0f);
        sat = 0.65f + 0.20f * sinf(0.13f * t + i * 0.09f);
        val = 0.95f;
    }
    else // neon
    {
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

// ------------------------ Dibujo de partículas (GPU) ------------------------

/**
 * Dibuja todas las partículas con simetría y espejo, usando:
 *   - sprites de discos (núcleo y colitas),
 *   - sprite radial para halo (si glow),
 *   - líneas de estela larga opcionales.
 * Respeta cfg.render_frac (salta partículas para acelerar) y ajusta alphas
 * según la cantidad de copias (simetrías * espejos).
 */
static void draw_particles(SDL_Renderer *ren, const Config *cfg, const Precomp *pc, int n, int symN, int mirror, float cx, float cy, SDL_Texture **discs, SDL_Texture *radial)
{
    float cosA[8], sinA[8];
    if (symN < 1)
        symN = 1;
    if (symN > 8)
        symN = 8;
    for (int m = 0; m < symN; ++m)
    {
        float ang = 2.0f * (float)M_PI * (float)m / (float)symN;
        cosA[m] = cosf(ang);
        sinA[m] = sinf(ang);
    }
    float alpha_div = (float)((mirror ? 2 : 1) * symN);
    int glow_on = cfg->glow ? 1 : 0;

    // Paso de muestreo para omitir partículas (solo dibujo) manteniendo la física completa
    int step = (cfg->render_frac >= 0.999f) ? 1 : (int)lroundf(1.0f / cfg->render_frac);
    if (step < 1)
        step = 1;

    for (int i = 0; i < n; i += step)
    {
        Uint8 rr = pc[i].r, gg = pc[i].g, bb = pc[i].b;
        float dx0 = pc[i].dx0, dy0 = pc[i].dy0;
        float dxp = pc[i].dxp, dyp = pc[i].dyp;
        int pr = pc[i].pr;
        if (pr < 1)
            pr = 1;
        if (pr > 3)
            pr = 3;

        SDL_Texture *dot = discs[pr];

        // Alphas base ajustados por cantidad de copias y glow
        float a_scale = glow_on ? 1.0f : 0.6f;
        Uint8 trailA = (Uint8)fmaxf(4.0f, (90.0f * a_scale) / alpha_div);
        Uint8 tailA0 = (Uint8)fmaxf(3.0f, (34.0f * a_scale) / alpha_div);
        Uint8 haloA = glow_on ? (Uint8)fmaxf(8.0f, 50.0f / alpha_div) : 0;
        Uint8 nucA = (Uint8)fmaxf(70.0f, (185.0f + 50.0f * 0.5f) / alpha_div);

        // Repite por cada rotación de simetría y espejo vertical opcional
        for (int m = 0; m < symN; ++m)
        {
            float xr = cx + dx0 * cosA[m] - dy0 * sinA[m];
            float yr = cy + dx0 * sinA[m] + dy0 * cosA[m];
            float xpr = cx + dxp * cosA[m] - dyp * sinA[m];
            float ypr = cy + dxp * sinA[m] + dyp * cosA[m];

            for (int mir = 0; mir < (mirror ? 2 : 1); ++mir)
            {
                float X = mir ? (2.0f * cx - xr) : xr;
                float Y = yr;
                float XP = mir ? (2.0f * cx - xpr) : xpr;
                float YP = ypr;

                // Estela larga opcional (línea)
                if (cfg->trail)
                {
                    SDL_SetRenderDrawBlendMode(ren, glow_on ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(ren, rr, gg, bb, trailA);
                    SDL_RenderDrawLine(ren, (int)lroundf(XP), (int)lroundf(YP), (int)lroundf(X), (int)lroundf(Y));
                }

                // Colitas de 2 discos (puntos intermedios entre pos previa y actual)
                float ddx = X - XP, ddy = Y - YP;
                for (int c = 1; c <= 2; ++c)
                {
                    float tpos = (float)c / 4.0f;
                    float cxp = X - ddx * tpos, cyp = Y - ddy * tpos;
                    Uint8 ca = (Uint8)fmaxf(3.0f, (float)tailA0 / (float)c);
                    int pr2 = (pr - c >= 1) ? (pr - c) : 1;
                    SDL_SetTextureColorMod(discs[pr2], rr, gg, bb);
                    SDL_SetTextureAlphaMod(discs[pr2], ca);
                    SDL_FRect rct = {(float)(cxp - pr2), (float)(cyp - pr2), (float)(pr2 * 2 + 1), (float)(pr2 * 2 + 1)};
                    SDL_RenderCopyF(ren, discs[pr2], NULL, &rct);
                }

                // Halo radial (si glow activo)
                if (haloA > 0)
                {
                    SDL_SetTextureColorMod(radial, rr, gg, bb);
                    SDL_SetTextureAlphaMod(radial, haloA);
                    float hr = (float)(pr + 2);
                    SDL_FRect hrct = {(float)(X - hr), (float)(Y - hr), hr * 2.0f, hr * 2.0f};
                    SDL_RenderCopyF(ren, radial, NULL, &hrct);
                }

                // Núcleo del punto
                SDL_SetTextureColorMod(dot, rr, gg, bb);
                SDL_SetTextureAlphaMod(dot, nucA);
                SDL_FRect rct = {(float)(X - pr), (float)(Y - pr), (float)(pr * 2 + 1), (float)(pr * 2 + 1)};
                SDL_RenderCopyF(ren, dot, NULL, &rct);
            }
        }
    }
}

/**
 * Renderiza un frame completo:
 *  1) Aplica fade con tinte de fondo por paleta.
 *  2) Dibuja partículas con simetrías y espejo.
 *  3) Opcional: dibuja guías/rectángulos de atractores.
 */
static void render_frame(SDL_Renderer *ren, const Config *cfg, const Precomp *pc, int n, Attractor a[NUM_ATTR], int W, int H, float t, int draw_sym, SDL_Texture **discs, SDL_Texture *radial)
{
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_Rect full = {0, 0, W, H};
    RGBA tint = palette_bg_tint(cfg, t);
    SDL_SetRenderDrawColor(ren, tint.r, tint.g, tint.b, tint.a);
    SDL_RenderFillRect(ren, &full);

    draw_particles(ren, cfg, pc, n, draw_sym, cfg->mirror, W * 0.5f, H * 0.5f, discs, radial);

    if (cfg->show_attractors)
    {
        for (int k = 0; k < NUM_ATTR; ++k)
        {
            Uint8 rr, gg, bb;
            palette_attractor_color(cfg->palette, k, t, &rr, &gg, &bb);
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
            SDL_SetRenderDrawColor(ren, rr, gg, bb, 24);
            SDL_FRect rct = {(float)a[k].x - 14, (float)a[k].y - 14, 28, 28};
            SDL_RenderDrawRectF(ren, &rct);
        }
    }
}

// ------------------------ SSAA (Render Target escalado) ------------------------

/**
 * Configura (crea/destruye) el render target para SSAA al cambiar el factor.
 * Ajusta RW,RH según outW/outH y ssaa. Si falla creación, cae a ssaa=1.
 */
static void set_ssaa(SDL_Renderer *ren, int outW, int outH, int newk,
                     int *ssaa, SDL_Texture **rt, int *RW, int *RH)
{
    if (newk < 1)
        newk = 1;
    if (newk > 4)
        newk = 4;
    if (*rt && newk == *ssaa)
        return; // No hace nada si ya coincide

    if (*rt) // Destruye RT previo si existía
    {
        SDL_DestroyTexture(*rt);
        *rt = NULL;
    }
    *ssaa = newk;
    *RW = outW * (*ssaa);
    *RH = outH * (*ssaa);
    if (*ssaa > 1)
    {
        *rt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, *RW, *RH);
        if (!*rt)
        {
            fprintf(stderr, "No se pudo crear RT SSAA=%d (%dx%d). Sin SSAA.\n", *ssaa, *RW, *RH);
            *ssaa = 1;
            *RW = outW;
            *RH = outH;
        }
    }
}

// ------------------------ Programa principal ------------------------

/**
 * main() realiza:
 *  - Inicialización de SDL (ventana, renderer, hints).
 *  - Creación de recursos (render target SSAA, sprites).
 *  - Construcción del mundo (3 atractores + N orbitadores).
 *  - Bucle principal: eventos, dt/FPS, update en paralelo, precálculo, render.
 *  - Calidad adaptativa (si cfg.adapt): reduce SSAA, fracción de render, glow
 *    o simetrías cuando FPS cae por debajo del objetivo; los eleva si sobra margen.
 *  - Logging periódico de métricas a CSV.
 *  - Liberación ordenada de recursos.
 */
int main(int argc, char **argv)
{
    Config cfg = parse_args(argc, argv);
    srand((unsigned)cfg.seed);

    // SDL: video + timer
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
    {
        fprintf(stderr, "Error SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    // Hints: intenta usar “metal” y batching en plataformas compatibles
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");

    // Ventana
    SDL_Window *win = SDL_CreateWindow(
        "Screensaver (paralelo) - Inicializando…",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        cfg.width, cfg.height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win)
    {
        fprintf(stderr, "Error SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Renderer acelerado; VSYNC según config
    int rflags = SDL_RENDERER_ACCELERATED | (cfg.vsync ? SDL_RENDERER_PRESENTVSYNC : 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, rflags);
    if (!ren)
    {
        fprintf(stderr, "Error SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2"); // Mejor filtrado de escalado
    int outW = cfg.width, outH = cfg.height;
    SDL_GetRendererOutputSize(ren, &outW, &outH); // Tamaño real del backbuffer (HiDPI)

    // Render target para SSAA
    SDL_Texture *rt = NULL;
    int RW = outW, RH = outH;
    set_ssaa(ren, outW, outH, cfg.ssaa, &cfg.ssaa, &rt, &RW, &RH);

    // Sprites de discos (radios 1..5) y halo radial 32x32
    SDL_Texture *discs[6] = {0};
    for (int r = 1; r <= 5; ++r)
        discs[r] = make_disc_texture(ren, r);
    SDL_Texture *radial = make_radial_texture(ren, 32);

    // Mundo: atractores + partículas
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

    Precomp *pc = (Precomp *)malloc(sizeof(Precomp) * (size_t)cfg.n);
    if (!pc)
    {
        fprintf(stderr, "Sin memoria para pre-cálculo\n");
        free(orbs);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    // Tiempo / FPS / Logging
    bool running = true;
    uint64_t t0 = SDL_GetPerformanceCounter();
    double t_sec = 0.0;

    FPSCounter fpsc;
    fpsc.last_ticks = SDL_GetPerformanceCounter();
    fpsc.smoothed_fps = 0.0;
    fpsc.alpha = 0.1;

    FILE *logfp = NULL;
    uint64_t start_ticks = SDL_GetPerformanceCounter(), last_log_ms = 0;
    if (cfg.log_path[0] != '\0')
    {
        logfp = fopen(cfg.log_path, "w");
        if (logfp)
        {
            fprintf(logfp, "time_s,smoothed_fps,fps_inst,n,width,height,palette,vsync,threads,ssaa,render_frac,sym\n");
            fflush(logfp);
        }
        else
        {
            fprintf(stderr, "No se pudo abrir log '%s'\n", cfg.log_path);
        }
    }

    int eff_threads = 1;
    (void)eff_threads;
#ifdef _OPENMP
    eff_threads = (cfg.threads > 0 ? cfg.threads : omp_get_max_threads()); // Hilos efectivos
#endif

    int draw_sym = cfg.sym;    // Simetrías efectivas (pueden bajar en adaptación)
    float last_adapt_t = 0.0f; // Histeresis temporal para no “parpadear” ajustes

    // Bucle principal
    while (running)
    {
        // Corte por tiempo si --seconds > 0
        if (cfg.seconds > 0)
        {
            uint64_t now = SDL_GetPerformanceCounter();
            double elapsed = ticks_to_seconds(now - t0);
            if (elapsed >= (double)cfg.seconds)
                running = false;
        }

        // Eventos: cerrar ventana / ESC
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT)
                running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                running = false;
        }

        // Avance temporal y medición de FPS
        double fps_inst = 0.0;
        double dt = fps_tick(&fpsc, &fps_inst);
        if (dt > 0.05)
            dt = 0.05; // Cap para estabilidad si hubo pausa larga
        t_sec += dt;

        // Actualización del mundo
        update_attractors(att, (float)t_sec, outW, outH);
        update_orbiters_parallel(orbs, cfg.n, att, (float)dt, cfg.threads);
        precalc_particles(&cfg, orbs, cfg.n, (float)t_sec, outW * 0.5f, outH * 0.5f, pc, cfg.threads);

        // Calidad adaptativa para intentar mantener >= target_fps
        if (cfg.adapt)
        {
            if ((float)t_sec - last_adapt_t > 0.7f) // Evita ajustar cada frame
            {
                last_adapt_t = (float)t_sec;
                if (fpsc.smoothed_fps > 0.0)
                {
                    if (fpsc.smoothed_fps < cfg.target_fps - 1)
                    {
                        // Baja calidad por pasos: SSAA -> render_frac -> glow -> simetrías
                        if (cfg.ssaa > 1)
                        {
                            set_ssaa(ren, outW, outH, cfg.ssaa - 1, &cfg.ssaa, &rt, &RW, &RH);
                        }
                        else if (cfg.render_frac > 0.6f)
                        {
                            cfg.render_frac -= 0.1f;
                        }
                        else if (cfg.glow)
                        {
                            cfg.glow = 0;
                        }
                        else if (draw_sym > 4)
                        {
                            draw_sym--;
                        }
                    }
                    else if (fpsc.smoothed_fps > cfg.target_fps + 8)
                    {
                        // Sube calidad: simetrías y luego render_frac
                        if (draw_sym < cfg.sym)
                        {
                            draw_sym++;
                        }
                        else if (cfg.render_frac < 1.0f)
                        {
                            cfg.render_frac = fminf(1.0f, cfg.render_frac + 0.1f);
                        }
                    }
                }
            }
        }

        // Render con o sin SSAA (RT escalado)
        if (cfg.ssaa > 1 && rt)
        {
            SDL_SetRenderTarget(ren, rt);
            SDL_RenderSetScale(ren, (float)cfg.ssaa, (float)cfg.ssaa);
            render_frame(ren, &cfg, pc, cfg.n, att, outW, outH, (float)t_sec, draw_sym, discs, radial);
            SDL_RenderSetScale(ren, 1.0f, 1.0f);
            SDL_SetRenderTarget(ren, NULL);
            SDL_RenderCopy(ren, rt, NULL, NULL);
            SDL_RenderPresent(ren);
        }
        else
        {
            render_frame(ren, &cfg, pc, cfg.n, att, outW, outH, (float)t_sec, draw_sym, discs, radial);
            SDL_RenderPresent(ren);
        }

        // Logging periódico
        if (logfp)
        {
            uint64_t now_ticks = SDL_GetPerformanceCounter();
            uint64_t elapsed_ms = ticks_to_ms_u64(now_ticks - start_ticks);
            if (elapsed_ms >= last_log_ms + (uint64_t)cfg.log_every_ms)
            {
                fprintf(logfp, "%.3f,%.3f,%.3f,%d,%d,%d,%s,%d,%d,%d,%.2f,%d\n",
                        t_sec, fpsc.smoothed_fps, fps_inst,
                        cfg.n, cfg.width, cfg.height, cfg.palette, cfg.vsync,
                        eff_threads, cfg.ssaa, cfg.render_frac, draw_sym);
                fflush(logfp);
                last_log_ms = elapsed_ms;
            }
        }

        // Título de ventana con estado en vivo
        char title[420];
        snprintf(title, sizeof(title),
                 "Screensaver (paralelo%s) | FPS: %.1f | thr=%d | N=%d win=%dx%d draw=%dx%d RT=%dx%d SSAA=%d | "
                 "palette=%s sat=%.2f bgA=%d glow=%d trail=%d | pt=%.2f | sym=%d mir=%d frac=%.2f",
#ifdef _OPENMP
                 " OMP"
#else
                 " SEQ-fallback"
#endif
                 ,
                 fpsc.smoothed_fps,
#ifdef _OPENMP
                 eff_threads
#else
                 1
#endif
                 ,
                 cfg.n, cfg.width, cfg.height, outW, outH, RW, RH, cfg.ssaa,
                 cfg.palette, cfg.sat_mul, cfg.bg_alpha, cfg.glow, cfg.trail, cfg.point_scale,
                 draw_sym, cfg.mirror, cfg.render_frac);
        SDL_SetWindowTitle(win, title);
    }

    // Liberación ordenada de recursos
    if (logfp)
        fclose(logfp);
    for (int r = 1; r <= 5; ++r)
        if (discs[r])
            SDL_DestroyTexture(discs[r]);
    if (radial)
        SDL_DestroyTexture(radial);
    if (rt)
        SDL_DestroyTexture(rt);
    free(pc);
    free(orbs);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}