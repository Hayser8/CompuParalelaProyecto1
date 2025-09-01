# Screensaver Mandala — Paralelo (SDL2 + OpenMP)

Versión **paralela** del generador de mandalas con SDL2. La **física** y el **pre-cálculo** por partícula se paralelizan (OpenMP opcional); el **dibujo** ocurre en el hilo principal usando sprites (texturas) para aprovechar la GPU.

Paletas soportadas: **`neon`** y **`ocean`**.

---

## 1) Requisitos

- **SDL2** y **pkg-config** (macOS/Homebrew):
  ```bash
  brew install sdl2 pkg-config
  ```
- **OpenMP** (opcional pero recomendado):
  - Apple Clang + `libomp`:
    ```bash
    brew install libomp
    ```
  - LLVM Clang (incluye OpenMP):
    ```bash
    brew install llvm
    ```

---

## 2) Compilación

Ejecutar desde la raíz del proyecto.

### A) Apple Clang + Homebrew `libomp` (recomendado)

```bash
clang -O3 -std=c11 paralelo/src/screensaver_paralelo.c \
  $(pkg-config --cflags sdl2) \
  -Xpreprocessor -fopenmp \
  -I"$(brew --prefix libomp)/include" \
  -L"$(brew --prefix libomp)/lib" -lomp \
  $(pkg-config --libs sdl2) -lm \
  -Wl,-rpath,"$(brew --prefix libomp)/lib" \
  -o paralelo/bin/screensaver_par
```

### B) Clang de LLVM con OpenMP

```bash
/opt/homebrew/opt/llvm/bin/clang -O3 -std=c11 paralelo/src/screensaver_paralelo.c \
  $(pkg-config --cflags sdl2) \
  -fopenmp \
  -L/opt/homebrew/opt/llvm/lib \
  -Wl,-rpath,/opt/homebrew/opt/llvm/lib \
  $(pkg-config --libs sdl2) -lm \
  -o paralelo/bin/screensaver_par
```

> Alternativa: `export PATH="/opt/homebrew/opt/llvm/bin:$PATH"` y usar `clang` normal.

### C) Sin OpenMP (fallback 1 hilo)

```bash
clang -O3 -std=c11 paralelo/src/screensaver_paralelo.c \
  $(pkg-config --cflags --libs sdl2) -lm \
  -o paralelo/bin/screensaver_par
```

Comprobar librería OpenMP enlazada:

```bash
otool -L paralelo/bin/screensaver_par | grep -i omp
```

---

## 3) Ejecución rápida

### Limpio, ocean, objetivo 30 FPS (adaptativo)

```bash
./paralelo/bin/screensaver_par \
  --n 1500 --width 1512 --height 982 \
  --palette ocean --ssaa 1 --vsync 0 \
  --trail 0 --adapt 1 --target-fps 30
```

```bash
./paralelo/bin/screensaver_par \
  --n 1500 --width 1512 --height 982 \
  --palette neon --ssaa 1 --vsync 0 \
  --trail 0 --adapt 1 --target-fps 30
```

### Medición “raw” para speedup + CSV

```bash
./paralelo/bin/screensaver_par \
  --n 1500 --width 1512 --height 982 \
  --seconds 30 --seed 42 --palette neon \
  --vsync 0 --ssaa 1 --threads 0 \
  --trail 0 --adapt 0 \
  --log par_neon.csv --log-every-ms 200
```

```bash
./paralelo/bin/screensaver_par \
  --n 1500 --width 1512 --height 982 \
  --seconds 30 --seed 42 --palette ocean \
  --vsync 0 --ssaa 1 --threads 0 \
  --trail 0 --adapt 0 \
  --log par_ocean.csv --log-every-ms 200
```

> Recomendado: 1512×982 (MBP 14") o 1920×1080 si el hardware lo permite.

---

## 4) Parámetros CLI

| Flag                  | Tipo  | Descripción                                                         |
| --------------------- | ----- | ------------------------------------------------------------------- |
| `--n`                 | int   | Número de partículas (≥1).                                          |
| `--width`, `--height` | int   | Tamaño ventana (mín 640×480).                                       |
| `--seconds`           | int   | Duración (≤0 hasta `ESC`).                                          |
| `--seed`              | int   | Semilla RNG (0 usa reloj).                                          |
| `--palette`           | str   | **`neon`** o **`ocean`**.                                           |
| `--vsync`             | 0/1   | VSync (1 por defecto). Para medir FPS, usar 0.                      |
| `--log`               | path  | CSV de métricas (vacío = sin log).                                  |
| `--log-every-ms`      | int   | Período de muestreo del CSV (ms).                                   |
| `--show-attractors`   | 0/1   | Dibuja guías de atractores.                                         |
| `--point-scale`       | float | Escala global del punto.                                            |
| `--sym`               | int   | Simetrías radiales (1..8).                                          |
| `--mirror`            | 0/1   | Espejo vertical.                                                    |
| `--ssaa`              | int   | Supersampling (1..4). **Costoso**.                                  |
| `--sat`               | float | Multiplicador global de saturación (0..1).                          |
| `--glow`              | 0/1   | Halo aditivo suave (0 = look “clean”).                              |
| `--bg-alpha`          | int   | Alpha del fade de fondo (0..255).                                   |
| `--threads`           | int   | Hilos OpenMP: **0 = auto** (`omp_get_max_threads()`); N>0 = manual. |
| `--trail`             | 0/1   | Estela larga por líneas (caro).                                     |
| `--render-frac`       | float | Fracción **dibujada** (física corre para todas).                    |
| `--adapt`             | 0/1   | Calidad adaptativa.                                                 |
| `--target-fps`        | int   | FPS objetivo para `--adapt 1`.                                      |

**CSV** (cabeceras):

```
time_s,smoothed_fps,fps_inst,n,width,height,palette,vsync,threads,ssaa,render_frac,sym
```

---

## 5) Performance / Calidad

- Sprites (texturas) para puntos/halos → mucho más rápido que círculos por software.
- `--adapt 1` mantiene `--target-fps` variando SSAA → render_frac → glow → simetrías.
- Evitar SSAA>1 si ya vas justo; su costo crece cuadráticamente.
- Si cae de 30 FPS: bajar `--n`, poner `--render-frac 0.8` (o 0.6), apagar `--trail` y `--glow`.

---

## 6) Control de hilos

- `--threads 0` (default): **automático** → `omp_get_max_threads()` (respeta `OMP_NUM_THREADS`).
- `--threads N`: fija N hilos.
- Ejemplo:
  ```bash
  OMP_NUM_THREADS=6 ./paralelo/bin/screensaver_par --threads 0 ...
  ```

---

## 7) Problemas comunes

**`fatal error: 'omp.h' file not found`**  
Falta `libomp` o include path. Use la receta de compilación A (Apple Clang + libomp).

**`clang: error: unsupported option '-fopenmp'`**  
Está usando Apple Clang sin `-Xpreprocessor -fopenmp`. Use opción A o el clang de LLVM (opción B).

**Corre por debajo de 30 FPS**  
Intente: `--vsync 0 --ssaa 1 --trail 0 --adapt 1 --target-fps 30` y reduzca N/`--render-frac` si es necesario.

---

## 8) Licencia

Uso académico/educativo.
