/**
 * Screensaver (secuencial, SDL2)
 * -----------------------------------------------------------
 * Hace un “screensaver” generativo basado en partículas (orbitadores)
 * que siguen atractores móviles con dinámica resorte-amortiguador,
 * simetrías radiales y opciones estéticas (paletas, glow, SSAA, etc.).
 *
 * El programa:
 *  - Parsea parámetros desde CLI (ancho/alto, N, segundos, paleta, etc.).
 *  - Inicializa SDL2 (ventana + renderer; opcional SSAA como render target).
 *  - Construye 3 atractores con movimiento senoidal y N orbitadores.
 *  - Integra la dinámica por cuadro y dibuja partículas + estelas con simetría.
 *  - Registra métricas a CSV si se especifica (--log).
 *  - Finaliza al presionar ESC, cerrar la ventana o al agotar --seconds>0.
 *
 * Dependencias: SDL2 (o SDL en Windows), math.h para trigonometría,
 * y uso de reloj de alto rendimiento para medir FPS y dt.
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
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** RGBA de 8 bits por canal.
 *  Representa un color con componentes rojo, verde, azul y alpha (opacidad). */
typedef struct
{
    Uint8 r, g, b, a; // Componentes en el rango [0,255]
} RGBA;

/** Configuración creada desde CLI. Todos los parámetros del render son parametrizables.
 *  Esta estructura almacena los “flags” y parámetros operativos que el usuario fija
 *  mediante argumentos de línea de comandos. */
typedef struct
{
    int width;           // Ancho de ventana. Se asegura mínimo >= 640.
    int height;          // Alto de ventana.  Se asegura mínimo >= 480.
    int n;               // Número de orbitadores/partículas.
    int seconds;         // Duración de ejecución; <=0 para correr hasta ESC/cerrar.
    uint32_t seed;       // Semilla del RNG; 0 => usa time(NULL).
    char palette[16];    // Paleta de color: "neon" (default), "sunset", "ocean", "candy", "mono".
    int vsync;           // 1=VSync ON (default), 0=OFF (puede aumentar FPS/tearing).
    char log_path[256];  // Ruta a CSV para log de métricas (vacío => no se loguea).
    int log_every_ms;    // Periodo de muestreo del log en milisegundos (default 500ms).
    int show_attractors; // 1=presenta halos/líneas de atractores; 0=oculta (default).
    float point_scale;   // Escala global del tamaño de puntitos (default 1.8f en banner, 1.0f aquí).
    int sym;             // Número de simetrías radiales [1..8]. 1=sin efecto.
    int mirror;          // 0/1 espejo vertical adicional por cada simetría.
    int ssaa;            // Factor de supersampling (1=apaga, 2..4=activo).
    // Controles de “clean look”:
    float sat_mul; // Multiplicador global de saturación (0..1) para paletas.
    int glow;      // 1=usa blending aditivo (glow), 0=modo limpio.
    int bg_alpha;  // Alpha del “fade” de fondo por frame (0..255): mayor => más arrastre.
} Config;

/** Muestra ayuda de CLI con valores por defecto y opciones válidas.
 *  @param exe Nombre del ejecutable (argv[0]).
 *  Imprime el uso y termina en el caller si corresponde. */
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

/** Convierte entero desde string con validación robusta.
 *  @param s   Cadena a convertir.
 *  @param out Puntero de salida para el entero.
 *  @return true si convierte correctamente en el rango de int; false en caso contrario. */
static bool parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == NULL || *end != '\0')
        return false; // Falla si hay basura al final o no hay número.
    if (v < INT_MIN || v > INT_MAX)
        return false; // Falla si excede rangos de int.
    *out = (int)v;
    return true;
}

/** Convierte float desde string con validación.
 *  @param s   Cadena a convertir.
 *  @param out Puntero de salida para el float.
 *  @return true si convierte correctamente; false en caso contrario. */
static bool parse_float(const char *s, float *out)
{
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == NULL || *end != '\0')
        return false; // Falla si hay caracteres extra.
    *out = v;
    return true;
}

/** Compara cadenas C de forma case-insensitive.
 *  @param a Cadena A.
 *  @param b Cadena B.
 *  @return true si son iguales ignorando mayúsculas/minúsculas; false si difieren. */
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

/** Parsea los argumentos CLI y aplica programación defensiva.
 *  - Valida rangos numéricos.
 *  - Aplica mínimos (resolución/N).
 *  - Normaliza paleta a {neon,ocean} si viene inválida.
 *  @param argc Número de argumentos.
 *  @param argv Vector de argumentos.
 *  @return Config final saneado.
 */
static Config parse_args(int argc, char **argv)
{
    Config cfg;
    // Valores por defecto (centrados en una estética “clean”)
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
    cfg.point_scale = 1.0f; // Tamaño base sobrio.
    cfg.sym = 6;            // Aspecto tipo “mandala”.
    cfg.mirror = 1;         // Espejo ON para reforzar simetría.
    cfg.ssaa = 2;           // SSAA por defecto para suavizar.
    cfg.sat_mul = 0.65f;    // Saturación contenida.
    cfg.glow = 0;           // Modo “limpio” por defecto.
    cfg.bg_alpha = 10;      // Fade de fondo moderado.

    // Bucle de parseo de flags
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

    // Normaliza paleta: por simplicidad sólo asegura {neon, ocean} como válidas.
    if (!str_ieq(cfg.palette, "neon") && !str_ieq(cfg.palette, "ocean"))
    {
        snprintf(cfg.palette, sizeof(cfg.palette), "%s", "neon");
    }
    // Asegura mínimos razonables y semilla por defecto.
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

/** Genera un float uniforme en [0,1]. */
static float frand01(void) { return (float)rand() / (float)RAND_MAX; }

/** Genera un float uniforme en [a,b]. */
static float frand_range(float a, float b) { return a + (b - a) * frand01(); }

/** Satura un entero a Uint8 [0,255]. */
static Uint8 clamp_u8(int v)
{
    if (v < 0)
        v = 0;
    if (v > 255)
        v = 255;
    return (Uint8)v;
}

/** Conversión HSV→RGB en 8bpc.
 *  Entradas: h en grados [0..360), s en [0..1], v en [0..1].
 *  Salida: r,g,b en [0..255]. */
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

/** Medidor de FPS con suavizado exponencial.
 *  - last_ticks: último sello temporal de alto rendimiento.
 *  - smoothed_fps: FPS suavizados (EMA).
 *  - alpha: factor de suavizado [0..1], mayor => responde más rápido. */
typedef struct
{
    uint64_t last_ticks;
    double smoothed_fps;
    double alpha;
} FPSCounter;

/** Convierte ticks de alto rendimiento a segundos. */
static double ticks_to_seconds(uint64_t ticks) { return (double)ticks / (double)SDL_GetPerformanceFrequency(); }

/** Convierte ticks a milisegundos (uint64). Hace redondeo correcto. */
static uint64_t ticks_to_ms_u64(uint64_t ticks)
{
    double ms = (double)ticks * 1000.0 / (double)SDL_GetPerformanceFrequency();
    if (ms < 0.0)
        ms = 0.0;
    return (uint64_t)(ms + 0.5);
}

/** Avanza el contador de FPS y entrega dt (segundos) desde la última llamada.
 *  @param f        Estructura FPSCounter (estado).
 *  @param fps_inst Salida opcional: FPS instantáneo (1/dt).
 *  @return dt en segundos (capaz de ser 0 si no pasó tiempo). */
static double fps_tick(FPSCounter *f, double *fps_inst)
{
    uint64_t now = SDL_GetPerformanceCounter();
    double dt = ticks_to_seconds(now - f->last_ticks);
    f->last_ticks = now;
    double inst = (dt > 0.0) ? (1.0 / dt) : 0.0;
    if (f->smoothed_fps <= 0.0)
        f->smoothed_fps = inst; // Inicializa EMA
    else
        f->smoothed_fps = f->alpha * inst + (1.0 - f->alpha) * f->smoothed_fps;
    if (fps_inst)
        *fps_inst = inst;
    return dt;
}

// ------------------------ Atractores y Orbitadores ------------------------

/** Atractor con movimiento senoidal independiente en X/Y.
 *  x,y: posición actual (se actualiza cada frame).
 *  ax,ay: amplitudes de oscilación (en px).
 *  fx,fy: frecuencias en rad/seg.
 *  phx,phy: fases en radianes (desfase inicial). */
typedef struct
{
    float x, y;
    float ax, ay;
    float fx, fy;
    float phx, phy;
} Attractor;

/** Orbitador/Partícula con dinámica resorte-amortiguador hacia una órbita
 *  desplazada alrededor de un atractor.
 *  - (x,y): posición actual, (px,py): posición previa (para trazar estela).
 *  - (vx,vy): velocidad.
 *  - att: índice del atractor que sigue [0..NUM_ATTR-1].
 *  - angle/radius/omega: parametrización polar de la órbita alrededor del atractor.
 *  - k/damping: constante de resorte y amortiguamiento para estabilidad.
 *  - size_*: controlan “pulso” y tamaño del punto renderizado. */
typedef struct
{
    float x, y;
    float px, py;
    float vx, vy;
    int att;
    float angle;
    float radius;
    float omega;
    float k;
    float damping;
    // Estética del puntito animado (respiración/pulso)
    float size_base, size_amp, size_speed, size_phase;
} Orbiter;

#define NUM_ATTR 3 // Se modelan exactamente 3 atractores coordinados.

/** Inicializa los 3 atractores centrados con amplitudes/frecuencias aleatorias.
 *  @param a  Arreglo de atractores.
 *  @param W  Ancho de la escena.
 *  @param H  Alto de la escena. */
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
        a[i].fx = 2.0f * (float)M_PI * fx_hz; // Convierte Hz a rad/s
        a[i].fy = 2.0f * (float)M_PI * fy_hz;
        a[i].phx = frand_range(0.0f, (float)M_PI * 2.0f);
        a[i].phy = frand_range(0.0f, (float)M_PI * 2.0f);
    }
}

/** Actualiza las posiciones de los atractores con movimiento senoidal.
 *  @param t  Tiempo acumulado en segundos.
 *  @param W,H Dimensiones (sólo para mantener centro de referencia). */
static void update_attractors(Attractor a[NUM_ATTR], float t, int W, int H)
{
    float cx = W * 0.5f, cy = H * 0.5f;
    for (int i = 0; i < NUM_ATTR; ++i)
    {
        a[i].x = cx + a[i].ax * sinf(a[i].fx * t + a[i].phx);
        a[i].y = cy + a[i].ay * sinf(a[i].fy * t + a[i].phy);
    }
}

/** Crea N orbitadores con radios y fases aleatorias alrededor de atractores.
 *  @param o   Arreglo de orbitadores (prealocado).
 *  @param n   Número de orbitadores.
 *  @param a   Atractores a seguir.
 *  @param W,H Dimensiones para derivar rangos de radio inicial. */
static void init_orbiters(Orbiter *o, int n, Attractor a[NUM_ATTR], int W, int H)
{
    const float minR = (float)((W < H ? W : H)) * 0.08f;
    const float maxR = (float)((W < H ? W : H)) * 0.38f;
    for (int i = 0; i < n; ++i)
    {
        o[i].att = i % NUM_ATTR;               // Reparte orbitadores sobre 3 atractores
        o[i].radius = frand_range(minR, maxR); // Radio de la órbita
        o[i].angle = frand_range(0.0f, (float)M_PI * 2.0f);
        float hz = frand_range(0.04f, 0.35f);
        o[i].omega = 2.0f * (float)M_PI * hz;   // rad/s
        o[i].k = frand_range(4.0f, 10.0f);      // Constante de resorte
        o[i].damping = frand_range(1.4f, 3.2f); // Amortiguamiento (reduce oscilaciones)
        // Posición inicial en la órbita
        float tx = a[o[i].att].x + cosf(o[i].angle) * o[i].radius;
        float ty = a[o[i].att].y + sinf(o[i].angle) * o[i].radius;
        o[i].x = o[i].px = tx;
        o[i].y = o[i].py = ty;
        o[i].vx = o[i].vy = 0.0f;
        // Parámetros estéticos de “pulso” del punto
        o[i].size_base = frand_range(2.0f, 3.5f);
        o[i].size_amp = frand_range(1.2f, 2.8f);
        o[i].size_speed = frand_range(0.6f, 1.6f) * 2.0f * (float)M_PI; // rad/s
        o[i].size_phase = frand_range(0.0f, 2.0f * (float)M_PI);
    }
}

/** Integra la dinámica de cada orbitador (resorte-amortiguador explícito).
 *  Actualiza posición previa, avanza el ángulo, calcula destino (tx,ty) orbitando
 *  el atractor, y aplica fuerzas tipo resorte con amortiguamiento.
 *  @param dt   Delta de tiempo del frame (segundos). */
static void update_orbiters(Orbiter *o, int n, Attractor a[NUM_ATTR], float dt, int W, int H)
{
    (void)W;
    (void)H; // No se usan actualmente
    for (int i = 0; i < n; ++i)
    {
        Orbiter *p = &o[i];
        p->px = p->x;
        p->py = p->y;              // Guarda la posición anterior (para estela)
        p->angle += p->omega * dt; // Avanza el ángulo de la órbita

        // Punto objetivo sobre la órbita alrededor del atractor
        float tx = a[p->att].x + cosf(p->angle) * p->radius;
        float ty = a[p->att].y + sinf(p->angle) * p->radius;

        // Aceleración tipo resorte (k*(dest - pos)) con amortiguamiento (-damping*vel)
        float ax = p->k * (tx - p->x) - p->damping * p->vx;
        float ay = p->k * (ty - p->y) - p->damping * p->vy;

        // Integración explícita (Euler)
        p->vx += ax * dt;
        p->vy += ay * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
    }
}

// ------------------------ Dibujo ------------------------

/** Dibuja un disco sólido mediante scanlines (rápido y sin texturas).
 *  @param ren Renderer de SDL.
 *  @param cx,cy Centro del círculo.
 *  @param r Radio en píxeles. */
static void draw_filled_circle(SDL_Renderer *ren, int cx, int cy, int r)
{
    for (int dy = -r; dy <= r; ++dy)
    {
        int yy = cy + dy;
        int dx = (int)floorf(sqrtf((float)(r * r - dy * dy))); // Semiancho horizontal
        SDL_RenderDrawLine(ren, cx - dx, yy, cx + dx, yy);
    }
}

/** Dibuja un halo radial con blending aditivo de baja intensidad para atractores.
 *  @param base_r Radio base del núcleo (además pinta un núcleo pequeño opaco). */
static void draw_radial_glow(SDL_Renderer *ren, int cx, int cy, int base_r, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
    for (int i = 6; i >= 1; --i)
    {
        int rr = base_r + i * 6;
        Uint8 a = (Uint8)(8 + i * 10); // Alpha creciente hacia el centro
        SDL_SetRenderDrawColor(ren, r, g, b, a);
        draw_filled_circle(ren, cx, cy, rr);
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, r, g, b, 210);
    draw_filled_circle(ren, cx, cy, 3); // Núcleo
}

// ---- Paletas / Tintes ----

/** Devuelve el color de los atractores según la paleta.
 *  - Soporta “mono”, “sunset”, “ocean”, “candy”, y por defecto base=0 (neutro).
 *  - Hace variar levemente el tono con el tiempo para dar vida. */
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

/** Calcula el color de cada partícula en función de la paleta seleccionada y del tiempo.
 *  - “ocean”: azules/teales con ligera modulación temporal de saturación.
 *  - “neon” : arcoíris suavizado basado en golden-angle.
 *  Aplica multiplicador de saturación global (sat_mul). */
static void palette_color(const Config *cfg, int i, float t, Uint8 *r, Uint8 *g, Uint8 *b)
{
    float hue, sat, val;
    if (str_ieq(cfg->palette, "ocean"))
    {
        hue = 180.0f + fmodf((float)i * 3.5f + 18.0f * sinf(0.21f * t + i * 0.05f), 40.0f);
        sat = 0.65f + 0.20f * sinf(0.13f * t + i * 0.09f);
        val = 0.95f;
    }
    else
    { // neon (default)
        hue = fmodf((float)i * 137.508f + 90.0f * sinf(0.23f * t + i * 0.031f), 360.0f);
        if (hue < 0.0f)
            hue += 360.0f;
        sat = 0.85f;
        val = 1.00f;
    }
    sat *= cfg->sat_mul; // Control global de saturación
    if (sat < 0.0f)
        sat = 0.0f;
    if (sat > 1.0f)
        sat = 1.0f;
    hsv2rgb(hue, sat, val, r, g, b);
}

/** Define el tinte de fondo y el alpha de “fade” usado para la estela global.
 *  - En “ocean” usa un azul oscuro muy sutil.
 *  - En “neon” usa un gris frío muy tenue.
 *  @return RGBA con alpha tomado de cfg->bg_alpha. */
static RGBA palette_bg_tint(const Config *cfg, float t)
{
    Uint8 r = 0, g = 0, b = 0;
    Uint8 a = (Uint8)cfg->bg_alpha;
    if (str_ieq(cfg->palette, "ocean"))
    {
        hsv2rgb(210.0f + 6.0f * sinf(0.10f * t), 0.25f, 0.16f, &r, &g, &b);
    }
    else
    {
        hsv2rgb(200.0f, 0.10f, 0.14f, &r, &g, &b);
    }
    RGBA out = {r, g, b, a};
    return out;
}

/** Dibuja un frame completo:
 *  (1) Aplica fade con tinte de fondo.
 *  (2) Dibuja partículas con estelas, colitas, halos/núcleos y simetrías (y espejo opcional).
 *  (3) Dibuja atractores (si show_attractors).
 *
 *  Variables clave:
 *   - symN: número de simetrías (capado [1..8]).
 *   - alpha_div: factor de división para repartir alpha cuando hay múltiples copias por simetría/espejo.
 *   - glow_on: activa blending aditivo para halos/estelas más brillantes.
 */
static void render_frame(SDL_Renderer *ren, Orbiter *o, int n, Attractor a[NUM_ATTR], int W, int H, float t, const Config *cfg)
{
    int symN = cfg->sym;
    if (symN < 1)
        symN = 1;
    if (symN > 8)
        symN = 8;

    float cx = W * 0.5f, cy = H * 0.5f;
    float cosA[8], sinA[8]; // Precálculo de bases trigonométricas de cada simetría.
    for (int m = 0; m < symN; ++m)
    {
        float ang = 2.0f * (float)M_PI * (float)m / (float)symN;
        cosA[m] = cosf(ang);
        sinA[m] = sinf(ang);
    }
    float alpha_div = (float)((cfg->mirror ? 2 : 1) * symN);
    int glow_on = cfg->glow ? 1 : 0;

    // (1) Fade sutil (tinte por paleta)
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_Rect full = (SDL_Rect){0, 0, W, H};
    RGBA tint = palette_bg_tint(cfg, t);
    SDL_SetRenderDrawColor(ren, tint.r, tint.g, tint.b, tint.a);
    SDL_RenderFillRect(ren, &full);

    // (2) Puntos + estelas + colitas + halos/núcleos con simetría y espejo
    for (int i = 0; i < n; ++i)
    {
        Uint8 rr, gg, bb;
        palette_color(cfg, i, t, &rr, &gg, &bb);

        // Vectores relativos al centro para aplicar rotaciones de simetría
        float dx0 = o[i].x - cx, dy0 = o[i].y - cy;
        float dxp = o[i].px - cx, dyp = o[i].py - cy;

        // Tamaño del punto: base + pulso + (ligero) escalado por velocidad
        float spd = sqrtf(o[i].vx * o[i].vx + o[i].vy * o[i].vy);
        float breath = 0.5f + 0.5f * sinf(o[i].size_speed * t + o[i].size_phase);
        float base = o[i].size_base * cfg->point_scale;
        float amp = o[i].size_amp * cfg->point_scale;
        int pr = (int)lroundf(base + amp * breath + fminf(2.0f, spd * 0.015f));
        if (pr < 1)
            pr = 1;
        if (pr > 3)
            pr = 3; // Se limita para una apariencia “HD”

        // Alphas derivados según presencia de glow y cantidad de copias (simetrías+espejo)
        float a_scale = glow_on ? 1.0f : 0.6f;
        Uint8 trailA = (Uint8)fmaxf(4.0f, (90.0f * a_scale) / alpha_div);
        Uint8 tailA0 = (Uint8)fmaxf(3.0f, (34.0f * a_scale) / alpha_div);
        Uint8 haloA = glow_on ? (Uint8)fmaxf(8.0f, 50.0f / alpha_div) : 0;
        Uint8 nucA = (Uint8)fmaxf(70.0f, (185.0f + 50.0f * breath) / alpha_div);

        // Repite para cada simetría y opcionalmente su espejo vertical
        for (int m = 0; m < symN; ++m)
        {
            // Rotación de la posición actual y previa
            float xr = cx + dx0 * cosA[m] - dy0 * sinA[m];
            float yr = cy + dx0 * sinA[m] + dy0 * cosA[m];
            float xpr = cx + dxp * cosA[m] - dyp * sinA[m];
            float ypr = cy + dxp * sinA[m] + dyp * cosA[m];

            for (int mirrorPass = 0; mirrorPass < (cfg->mirror ? 2 : 1); ++mirrorPass)
            {
                // Aplica espejo horizontal respecto al centro si corresponde
                float X = (mirrorPass ? (2.0f * cx - xr) : xr), Y = yr;
                float XP = (mirrorPass ? (2.0f * cx - xpr) : xpr), YP = ypr;

                // Estela principal
                SDL_SetRenderDrawBlendMode(ren, glow_on ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(ren, rr, gg, bb, trailA);
                SDL_RenderDrawLine(ren, (int)lroundf(XP), (int)lroundf(YP),
                                   (int)lroundf(X), (int)lroundf(Y));

                // Colitas discretas para reforzar la percepción de movimiento
                float ddx = X - XP, ddy = Y - YP;
                for (int c = 1; c <= 2; ++c)
                {
                    float tpos = (float)c / 4.0f;
                    int cxp = (int)lroundf(X - ddx * tpos);
                    int cyp = (int)lroundf(Y - ddy * tpos);
                    Uint8 ca = (Uint8)fmaxf(3.0f, (float)tailA0 / (float)c);
                    SDL_SetRenderDrawBlendMode(ren, glow_on ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(ren, rr, gg, bb, ca);
                    draw_filled_circle(ren, cxp, cyp, pr - c >= 1 ? pr - c : 1);
                }

                // Halo suave (si glow activado)
                if (haloA > 0)
                {
                    SDL_SetRenderDrawBlendMode(ren, glow_on ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(ren, rr, gg, bb, haloA);
                    draw_filled_circle(ren, (int)lroundf(X), (int)lroundf(Y), pr + 2);
                }

                // Núcleo del puntito
                SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(ren, rr, gg, bb, nucA);
                draw_filled_circle(ren, (int)lroundf(X), (int)lroundf(Y), pr);
            }
        }
    }

    // (3) Atractores (opcional)
    if (cfg->show_attractors)
    {
        for (int k = 0; k < NUM_ATTR; ++k)
        {
            Uint8 rr2, gg2, bb2;
            palette_attractor_color(cfg->palette, k, t, &rr2, &gg2, &bb2);
            draw_radial_glow(ren, (int)lroundf(a[k].x), (int)lroundf(a[k].y), 10, rr2, gg2, bb2);
        }
        // Líneas tenues que conectan los atractores
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

/** Punto de entrada:
 *  - Parsea argumentos y siembra RNG.
 *  - Inicializa SDL, ventana y renderer (con VSync si corresponde).
 *  - Configura SSAA como render target si ssaa>1.
 *  - Genera mundo (3 atractores + N orbitadores).
 *  - Bucle principal: eventos, dt/FPS, update, render, logging CSV.
 *  - Limpia recursos y cierra SDL.
 */
int main(int argc, char **argv)
{
    Config cfg = parse_args(argc, argv);
    srand((unsigned)cfg.seed); // Si seed=0, parse_args ya lo reemplazó por time(NULL)

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

    // Crea renderer acelerado y opcionalmente sincronizado a VBlank
    int rflags = SDL_RENDERER_ACCELERATED | (cfg.vsync ? SDL_RENDERER_PRESENTVSYNC : 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, rflags);
    if (!ren)
    {
        fprintf(stderr, "Error SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    // HiDPI + mejor filtro de escalado (SDL_HINT_RENDER_SCALE_QUALITY: 0/1/2)
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2"); // “best”
    int outW = cfg.width, outH = cfg.height;
    SDL_GetRendererOutputSize(ren, &outW, &outH); // Tamaño real del backbuffer (puede variar en HiDPI)

    // SSAA: crea render target más grande y luego lo reduce al presentar
    SDL_Texture *rt = NULL;
    int RW = outW, RH = outH; // Resolución de dibujo “interna”
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

    // Limpia fondo a negro inicial
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderPresent(ren);

    // Mundo: atractores + orbitadores
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

    // Medición de tiempo y FPS
    bool running = true;
    uint64_t t0 = SDL_GetPerformanceCounter(); // Para cortar por --seconds
    double t_sec = 0.0;                        // Tiempo acumulado (segundos)
    FPSCounter fpsc;
    fpsc.last_ticks = SDL_GetPerformanceCounter();
    fpsc.smoothed_fps = 0.0;
    fpsc.alpha = 0.1; // EMA con suavizado moderado

    // Logger CSV (opcional)
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

    // Bucle principal
    while (running)
    {
        // Si --seconds > 0, se corta al alcanzar esa duración
        if (cfg.seconds > 0)
        {
            uint64_t now = SDL_GetPerformanceCounter();
            double elapsed = ticks_to_seconds(now - t0);
            if (elapsed >= (double)cfg.seconds)
                running = false;
        }

        // Manejo de eventos (cerrar ventana / ESC)
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT)
                running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                running = false;
        }

        // Timestep y FPS
        double fps_inst = 0.0;
        double dt = fps_tick(&fpsc, &fps_inst); // Calcula dt desde la última iteración
        if (dt > 0.05)
            dt = 0.05; // Cap para evitar explosiones por pausas largas
        t_sec += dt;

        // Actualiza mundo físico/geométrico
        update_attractors(att, (float)t_sec, outW, outH);
        update_orbiters(orbs, cfg.n, att, (float)dt, outW, outH);

        // Logging periódico (si está activo)
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

        // Renderiza: si hay SSAA, dibuja en el render target grande y luego lo copia
        if (cfg.ssaa > 1 && rt)
        {
            SDL_SetRenderTarget(ren, rt);
            SDL_RenderSetScale(ren, (float)cfg.ssaa, (float)cfg.ssaa); // Opcional; dibuja en “escala 1” lógica
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

        // Título de la ventana con estado en vivo
        char title[320];
        snprintf(title, sizeof(title),
                 "Screensaver (secuencial) | FPS: %.1f | N=%d  win=%dx%d draw=%dx%d RT=%dx%d SSAA=%d | palette=%s | sat=%.2f bgA=%d glow=%d | pt=%.2f | attractors=%d | sym=%d | mirror=%d",
                 fpsc.smoothed_fps, cfg.n, cfg.width, cfg.height, outW, outH, RW, RH, cfg.ssaa,
                 cfg.palette, cfg.sat_mul, cfg.bg_alpha, cfg.glow, cfg.point_scale, cfg.show_attractors, cfg.sym, cfg.mirror);
        SDL_SetWindowTitle(win, title);
    }

    // Limpieza y cierre ordenado
    if (logfp)
        fclose(logfp);
    free(orbs);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}