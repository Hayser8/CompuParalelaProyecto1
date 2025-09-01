# Screensaver Secuencial (SDL2) — Mandalas **neon** y **ocean**

Efecto visual con órbitas y estelas, compuesto en **mandala** (simetrías radiales + espejo).  
Se prioriza una estética **limpia y nítida** en pantallas HiDPI/Retina mediante **SSAA** (supersampling).  
Incluye **logging a CSV** para comparar FPS con la versión paralela (speedup).

---

## 1) Características

- Mandala configurable con `--sym` (sectores radiales) y `--mirror` (espejo).
- Paletas: **neon** y **ocean** (si llega otra, se normaliza a `neon`).
- Look limpio por defecto: saturación moderada, sin glow fuerte, SSAA activo.
- HiDPI-aware (macOS/Retina): usa el tamaño **drawable** real para evitar blur.
- CSV de métricas (tiempo, FPS) para benchmarking y speedup.
- Programación defensiva en CLI: validación/clamp de rangos; parámetros sin _hard-codes_.

---

## 2) Requisitos

- macOS (Homebrew) o Linux
- `SDL2`
- `gcc` o `clang`
- (macOS) `pkg-config` recomendado

Instalación (macOS):

```bash
brew install sdl2 pkg-config
```

---

## 3) Compilación

```bash
# crear carpeta bin
mkdir -p secuencial/bin

# Opción A (recomendada): con pkg-config
gcc -O2 -std=c11 secuencial/src/screensaver_seq.c   $(pkg-config --cflags --libs sdl2) -lm   -o secuencial/bin/screensaver_seq

# Opción B: rutas típicas de Homebrew (si no usas pkg-config)
gcc -O2 -std=c11 secuencial/src/screensaver_seq.c   -I/opt/homebrew/include/SDL2 -L/opt/homebrew/lib -lSDL2 -lm   -o secuencial/bin/screensaver_seq
```

---

## 4) Ejecución rápida (presets nítidos)

**Neon (limpio)**

```bash
./secuencial/bin/screensaver_seq --n 1400 --width 1512 --height 982  --palette neon --ssaa 3 --sat 0.65 --point-scale 1.0   --sym 6 --mirror 1 --glow 0 --vsync 1
```

**Ocean (limpio)**

```bash
./secuencial/bin/screensaver_seq --n 1400 --width 1512 --height 982  --palette ocean --ssaa 3 --sat 0.65 --point-scale 1.0   --sym 6 --mirror 1 --glow 0 --vsync 1
```

Salir: **ESC**.

---

## 5) Parámetros (CLI)

- `--n N` · Número de partículas. **Def:** `100`.
- `--width W --height H` · Tamaño de ventana. **Def:** `800×600` (mín.: `640×480`).
- `--seconds S` · Duración (`<=0` = infinito). **Def:** `10`.
- `--seed SEED` · Semilla RNG (`0` usa reloj). **Def:** `0`.
- `--palette neon|ocean` · Paleta (otras → `neon`). **Def:** `neon`.
- `--vsync 0|1` · V-Sync (para medir FPS usar `0`). **Def:** `1`.
- `--ssaa K` · Supersampling `1..4`. **Def:** `2`.
- `--sat F` · Saturación global `0..1`. **Def:** `0.65`.
- `--glow 0|1` · Halo opcional (resplandor aditivo). **Def:** `0` limpio.
- `--bg-alpha A` · Alpha del fade `0..255`. **Def:** `10`.
- `--sym K` · Simetrías radiales `1..8`. **Def:** `6`.
- `--mirror 0|1` · Espejo horizontal. **Def:** `1`.
- `--point-scale F` · Tamaño base de puntos. **Def:** `1.0`.
- `--show-attractors 0|1` · Dibujo opcional de atractores (halos/guías). **Def:** `0`.
- `--log PATH --log-every-ms MS` · CSV de métricas (periodo def.: `500 ms`).

**Nota sobre “halo” y “atractores” opcionales**

- _Halo_ (`--glow 0|1`): resplandor aditivo. No cambia la física, solo el dibujo.
- _Atractores_ (`--show-attractors 0|1`): muestra/oculta los centros con un halo y líneas guía.

---

## 6) CSV

**Generar CSV** con **misma configuración** y **VSync OFF**:

```bash
./secuencial/bin/screensaver_seq --n 1400 --width 1512 --height 982  --seconds 30 --seed 42 --palette ocean --vsync 0 --ssaa 2   --log seq_ocean.csv --log-every-ms 200

./secuencial/bin/screensaver_seq --n 1400 --width 1512 --height 982  --seconds 30 --seed 42 --palette neon --vsync 0 --ssaa 2   --log seq_neon.csv --log-every-ms 200

```

---

## 7) Diseño y programación defensiva

- Sin hard-coded en parámetros visibles: todo ajustable por CLI (N, resolución, paleta, simetrías, saturación, SSAA, glow, etc.).
- Validación/clamps en `parse_args`:
  - `width/height` mínimos garantizados (`>= 640×480`).
  - `n >= 1`.
  - `ssaa` restringido a `1..4`.
  - `sat` forzado a `[0..1]`.
  - `bg-alpha` a `[0..255]`.
  - Paleta normalizada a `{neon, ocean}`.
- HiDPI-aware: se consulta el tamaño de render real (`draw=...`) para evitar escalado borroso.
- Título con estado: FPS y parámetros clave para diagnóstico/benchmarking.

---

## 8) Problemas comunes y tips

- **Imagen “pixelada/empastada”**

  - Sube `--ssaa` a `3`.
  - Baja `--point-scale` (p. ej. `0.9–1.2`).
  - Reduce `--sym` (4–6) o `--glow 0`.
  - Verifica en Retina que `draw=` ≈ `2× win=`.

- **FPS bajos**
  - Reduce `--n`, `--ssaa` o `--sym`.
  - Desactiva `--show-attractors`.

---

## 9) Controles

- **ESC** → salir.

---

## 10) Licencia

Uso académico/educativo.
