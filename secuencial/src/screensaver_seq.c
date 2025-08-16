// screensaver_seq.c
// Screensaver secuencial en C con SDL2: "Órbitas con estela"
// - N orbitadores giran alrededor de 3 atractores que se mueven con curvas seno.
// - Física simple: resorte-amortiguador hacia la posición objetivo de cada órbita.
// - Estelas con fade (overlay semitransparente) y líneas aditivas para efecto glow.
// - Colores pseudoaleatorios y FPS en el título.
//
// Compilar (Linux/macOS):
//   gcc -O2 -std=c11 secuencial/src/screensaver_seq.c $(pkg-config --cflags --libs sdl2) -lm -o secuencial/bin/screensaver_seq
// Ejecutar:
//   ./secuencial/bin/screensaver_seq --n 1200 --width 1024 --height 768 --seconds 0 --seed 42
//
// Teclas: ESC para salir.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <math.h>

#if defined(_WIN32)
  #include <SDL.h>
#else
  #include <SDL2/SDL.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ------------------------ Config y CLI ------------------------
typedef struct {
    int width;      // >= 640
    int height;     // >= 480
    int n;          // número de orbitadores
    int seconds;    // duración; <=0 => corre hasta ESC
    uint32_t seed;  // 0 => usa reloj
} Config;

static void print_usage(const char* exe) {
    fprintf(stderr,
        "Uso: %s [--n N] [--width W] [--height H] [--seconds S] [--seed SEED]\n"
        "Defaults: N=100, W=800, H=600, S=10 (<=0 infinito), SEED=now\n", exe);
}

static bool parse_int(const char* s, int* out) {
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == NULL || *end != '\0') return false;
    if (v < INT_MIN || v > INT_MAX)  return false;
    *out = (int)v;
    return true;
}

static Config parse_args(int argc, char** argv) {
    Config cfg;
    cfg.width = 800;
    cfg.height = 600;
    cfg.n = 100;
    cfg.seconds = 10;
    cfg.seed = 0;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        #define NEED() do{ if (i+1>=argc) { print_usage(argv[0]); exit(1);} }while(0)
        if (strcmp(a, "--n") == 0) {
            NEED(); if (!parse_int(argv[++i], &cfg.n)) { print_usage(argv[0]); exit(1); }
        } else if (strcmp(a, "--width") == 0) {
            NEED(); if (!parse_int(argv[++i], &cfg.width)) { print_usage(argv[0]); exit(1); }
        } else if (strcmp(a, "--height") == 0) {
            NEED(); if (!parse_int(argv[++i], &cfg.height)) { print_usage(argv[0]); exit(1); }
        } else if (strcmp(a, "--seconds") == 0) {
            NEED(); if (!parse_int(argv[++i], &cfg.seconds)) { print_usage(argv[0]); exit(1); }
        } else if (strcmp(a, "--seed") == 0) {
            int s; NEED(); if (!parse_int(argv[++i], &s)) { print_usage(argv[0]); exit(1); }
            cfg.seed = (uint32_t)s;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_usage(argv[0]); exit(0);
        } else {
            fprintf(stderr, "Argumento no reconocido: %s\n", a);
            print_usage(argv[0]); exit(1);
        }
        #undef NEED
    }

    if (cfg.width  < 640) cfg.width  = 640;
    if (cfg.height < 480) cfg.height = 480;
    if (cfg.n < 1)        cfg.n = 1;
    if (cfg.seed == 0)    cfg.seed = (uint32_t)time(NULL);
    return cfg;
}

// ------------------------ Utilidades ------------------------
static float frand01(void) {
    return (float)rand() / (float)RAND_MAX;
}
static float frand_range(float a, float b) {
    return a + (b - a) * frand01();
}
static Uint8 clamp_u8(int v) {
    if (v < 0) v = 0; if (v > 255) v = 255;
    return (Uint8)v;
}

// HSV (0..360, 0..1, 0..1) a RGB 0..255
static void hsv2rgb(float h, float s, float v, Uint8* r, Uint8* g, Uint8* b) {
    float C = v * s;
    float X = C * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - C;
    float R=0, G=0, B=0;
    if      (h < 60)  { R=C; G=X; B=0; }
    else if (h < 120) { R=X; G=C; B=0; }
    else if (h < 180) { R=0; G=C; B=X; }
    else if (h < 240) { R=0; G=X; B=C; }
    else if (h < 300) { R=X; G=0; B=C; }
    else              { R=C; G=0; B=X; }
    *r = clamp_u8((int)((R + m) * 255.0f));
    *g = clamp_u8((int)((G + m) * 255.0f));
    *b = clamp_u8((int)((B + m) * 255.0f));
}

// ------------------------ FPS ------------------------
typedef struct {
    uint64_t last_ticks;
    double   smoothed_fps;
    double   alpha;
} FPSCounter;

static double ticks_to_seconds(uint64_t ticks) {
    return (double)ticks / (double)SDL_GetPerformanceFrequency();
}

static double fps_tick(FPSCounter* f, double* fps_inst) {
    uint64_t now = SDL_GetPerformanceCounter();
    double dt = ticks_to_seconds(now - f->last_ticks);
    f->last_ticks = now;
    double inst = (dt > 0.0) ? (1.0 / dt) : 0.0;
    if (f->smoothed_fps <= 0.0) f->smoothed_fps = inst;
    else f->smoothed_fps = f->alpha * inst + (1.0 - f->alpha) * f->smoothed_fps;
    if (fps_inst) *fps_inst = inst;
    return dt;
}

// ------------------------ Atractores y Orbitadores ------------------------
typedef struct {
    float x, y;        // posición actual
    // parámetros de movimiento sinusoidal
    float ax, ay;      // amplitudes
    float fx, fy;      // frecuencias (rad/seg)
    float phx, phy;    // fases
} Attractor;

typedef struct {
    float x, y;        // posición
    float px, py;      // posición previa (para trazar la estela)
    float vx, vy;      // velocidad
    int   att;         // índice del atractor que sigue
    float angle;       // ángulo de la órbita (rad)
    float radius;      // radio de la órbita (px)
    float omega;       // velocidad angular (rad/seg)
    float k;           // constante de resorte
    float damping;     // amortiguamiento
    Uint8 r,g,b;       // color base
} Orbiter;

#define NUM_ATTR 3

static void init_attractors(Attractor a[NUM_ATTR], int W, int H) {
    float cx = W * 0.5f, cy = H * 0.5f;
    for (int i = 0; i < NUM_ATTR; ++i) {
        a[i].x = cx; a[i].y = cy;
        a[i].ax = frand_range(W*0.20f, W*0.35f);
        a[i].ay = frand_range(H*0.20f, H*0.35f);
        // frecuencias bajas (0.05 a 0.15 Hz) => en rad/seg
        float fx_hz = frand_range(0.05f, 0.15f);
        float fy_hz = frand_range(0.05f, 0.15f);
        a[i].fx = 2.0f*(float)M_PI * fx_hz;
        a[i].fy = 2.0f*(float)M_PI * fy_hz;
        a[i].phx = frand_range(0.0f, (float)M_PI*2.0f);
        a[i].phy = frand_range(0.0f, (float)M_PI*2.0f);
    }
}

static void update_attractors(Attractor a[NUM_ATTR], float t, int W, int H) {
    float cx = W * 0.5f, cy = H * 0.5f;
    for (int i = 0; i < NUM_ATTR; ++i) {
        a[i].x = cx + a[i].ax * sinf(a[i].fx * t + a[i].phx);
        a[i].y = cy + a[i].ay * sinf(a[i].fy * t + a[i].phy);
    }
}

static void init_orbiters(Orbiter* o, int n, Attractor a[NUM_ATTR], int W, int H) {
    const float minR = (float)( (W<H?W:H) ) * 0.08f;
    const float maxR = (float)( (W<H?W:H) ) * 0.38f;

    for (int i = 0; i < n; ++i) {
        o[i].att = i % NUM_ATTR;
        o[i].radius = frand_range(minR, maxR);
        o[i].angle  = frand_range(0.0f, (float)M_PI*2.0f);
        float hz = frand_range(0.04f, 0.35f);    // rotaciones/seg
        o[i].omega = 2.0f*(float)M_PI * hz;

        // resorte y amortiguamiento suaves
        o[i].k = frand_range(4.0f, 10.0f);       // ~ aceleración hacia objetivo
        o[i].damping = frand_range(1.4f, 3.2f);  // freno proporcional a v

        // posición inicial
        float tx = a[o[i].att].x + cosf(o[i].angle) * o[i].radius;
        float ty = a[o[i].att].y + sinf(o[i].angle) * o[i].radius;
        o[i].x = o[i].px = tx;
        o[i].y = o[i].py = ty;
        o[i].vx = o[i].vy = 0.0f;

        // color: repartir tonos con el ángulo áureo para buena variedad
        float hue = fmodf((float)i * 137.508f, 360.0f);
        hsv2rgb(hue, 0.85f, 1.0f, &o[i].r, &o[i].g, &o[i].b);
    }
}

static void update_orbiters(Orbiter* o, int n, Attractor a[NUM_ATTR], float dt, int W, int H) {
    (void)W; (void)H; // no necesitamos bordes: el resorte los centra
    for (int i = 0; i < n; ++i) {
        Orbiter* p = &o[i];
        p->px = p->x; p->py = p->y; // guarda posición anterior para dibujar estela

        // objetivo en órbita
        p->angle += p->omega * dt;
        float tx = a[p->att].x + cosf(p->angle) * p->radius;
        float ty = a[p->att].y + sinf(p->angle) * p->radius;

        // resorte-amortiguador: a = k*(target-pos) - c*vel
        float ax = p->k * (tx - p->x) - p->damping * p->vx;
        float ay = p->k * (ty - p->y) - p->damping * p->vy;

        // integración explícita simple
        p->vx += ax * dt;
        p->vy += ay * dt;
        p->x  += p->vx * dt;
        p->y  += p->vy * dt;
    }
}

// ------------------------ Dibujo ------------------------
static void draw_filled_circle(SDL_Renderer* ren, int cx, int cy, int r) {
    for (int dy = -r; dy <= r; ++dy) {
        int yy = cy + dy;
        int dx = (int)floorf(sqrtf((float)(r*r - dy*dy)));
        int x1 = cx - dx;
        int x2 = cx + dx;
        SDL_RenderDrawLine(ren, x1, yy, x2, yy);
    }
}

static void render_frame(SDL_Renderer* ren, Orbiter* o, int n, Attractor a[NUM_ATTR], int W, int H) {
    // 1) Fade sutil para la estela
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 28); // alpha bajo => persistencia de la estela
    SDL_Rect full = {0,0,W,H};
    SDL_RenderFillRect(ren, &full);

    // 2) Líneas aditivas para "glow" de la estela
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
    for (int i = 0; i < n; ++i) {
        SDL_SetRenderDrawColor(ren, o[i].r, o[i].g, o[i].b, 64);
        SDL_RenderDrawLine(ren, (int)lroundf(o[i].px), (int)lroundf(o[i].py),
                                 (int)lroundf(o[i].x),  (int)lroundf(o[i].y));
        // puntito en la cabeza de la estela
        SDL_SetRenderDrawColor(ren, o[i].r, o[i].g, o[i].b, 200);
        draw_filled_circle(ren, (int)lroundf(o[i].x), (int)lroundf(o[i].y), 2);
    }

    // 3) Atractores (suaves halos)
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (int k = 0; k < NUM_ATTR; ++k) {
        Uint8 rr, gg, bb;
        float hue = fmodf(60.0f * k + 20.0f, 360.0f);
        hsv2rgb(hue, 0.4f, 0.9f, &rr, &gg, &bb);
        SDL_SetRenderDrawColor(ren, rr, gg, bb, 50);
        draw_filled_circle(ren, (int)lroundf(a[k].x), (int)lroundf(a[k].y), 18);
        SDL_SetRenderDrawColor(ren, rr, gg, bb, 140);
        draw_filled_circle(ren, (int)lroundf(a[k].x), (int)lroundf(a[k].y), 3);
    }

    SDL_RenderPresent(ren);
}

// ------------------------ Main ------------------------
int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    srand((unsigned)cfg.seed);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "Error SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow(
        "Screensaver (secuencial) - Inicializando…",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        cfg.width, cfg.height, SDL_WINDOW_SHOWN
    );
    if (!win) { fprintf(stderr, "Error SDL_CreateWindow: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

    // VSync ON para suavidad; si quieres medir “raw” quita PRESENTVSYNC
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { fprintf(stderr, "Error SDL_CreateRenderer: %s\n", SDL_GetError()); SDL_DestroyWindow(win); SDL_Quit(); return 1; }

    // Fondo inicial negro
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderPresent(ren);

    // Mundo
    Attractor att[NUM_ATTR];
    init_attractors(att, cfg.width, cfg.height);

    Orbiter* orbs = (Orbiter*)malloc(sizeof(Orbiter) * (size_t)cfg.n);
    if (!orbs) { fprintf(stderr, "Sin memoria para %d orbitadores\n", cfg.n); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit(); return 1; }
    init_orbiters(orbs, cfg.n, att, cfg.width, cfg.height);

    // Tiempo y FPS
    bool running = true;
    uint64_t t0 = SDL_GetPerformanceCounter();
    double t_sec = 0.0;

    FPSCounter fpsc;
    fpsc.last_ticks = SDL_GetPerformanceCounter();
    fpsc.smoothed_fps = 0.0;
    fpsc.alpha = 0.1;

    while (running) {
        // Salir por tiempo si seconds > 0
        if (cfg.seconds > 0) {
            uint64_t now = SDL_GetPerformanceCounter();
            double elapsed = ticks_to_seconds(now - t0);
            if (elapsed >= (double)cfg.seconds) running = false;
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        double fps_inst = 0.0;
        double dt = fps_tick(&fpsc, &fps_inst);
        if (dt > 0.05) dt = 0.05; // clamp para evitar saltos si el sistema se pausa
        t_sec += dt;

        update_attractors(att, (float)t_sec, cfg.width, cfg.height);
        update_orbiters(orbs, cfg.n, att, (float)dt, cfg.width, cfg.height);
        render_frame(ren, orbs, cfg.n, att, cfg.width, cfg.height);

        // Título con FPS y parámetros
        char title[256];
        snprintf(title, sizeof(title),
                 "Screensaver (secuencial) | FPS: %.1f | N=%d  %dx%d",
                 fpsc.smoothed_fps, cfg.n, cfg.width, cfg.height);
        SDL_SetWindowTitle(win, title);
    }

    free(orbs);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
