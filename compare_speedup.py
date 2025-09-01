import csv
import sys
import os
import math
import glob
from statistics import mean, median
from collections import defaultdict, Counter

DEFAULT_FILES = ["par_neon.csv", "seq_neon.csv", "par_ocean.csv", "seq_ocean.csv"]


def read_csv_metrics(path):
    with open(path, newline='', encoding='utf-8') as f:
        reader = csv.reader(f)
        rows = list(reader)

    if not rows:
        raise ValueError(f"{path}: CSV vacío")

    header = rows[0]
    name_to_idx = {col.strip(): i for i, col in enumerate(header)}

    def idx(col):
        return name_to_idx.get(col, None)

    i_smoothed = idx("smoothed_fps")
    i_inst = idx("fps_inst")
    i_palette = idx("palette")
    i_threads = idx("threads")

    if i_smoothed is None:
        raise ValueError(f"{path}: falta columna 'smoothed_fps'")

    fps_s, fps_i, palettes = [], [], []
    any_threads_gt1 = False

    for r in rows[1:]:
        try:
            if i_smoothed is not None and r[i_smoothed] != "":
                fps_s.append(float(r[i_smoothed]))
        except Exception:
            pass
        try:
            if i_inst is not None and r[i_inst] != "":
                fps_i.append(float(r[i_inst]))
        except Exception:
            pass
        if i_palette is not None and r[i_palette] != "":
            palettes.append(r[i_palette].strip())
        if i_threads is not None:
            try:
                if int(float(r[i_threads])) > 1:
                    any_threads_gt1 = True
            except Exception:
                pass

    if not fps_s:
        raise ValueError(f"{path}: no hay datos válidos en 'smoothed_fps'")

    if palettes:
        inferred_palette = Counter(palettes).most_common(1)[0][0]
    else:
        base = os.path.basename(path).lower()
        inferred_palette = "ocean" if "ocean" in base else ("neon" if "neon" in base else "unknown")

    base = os.path.basename(path).lower()
    if base.startswith("par_") or any_threads_gt1:
        mode = "par"
    elif base.startswith("seq_"):
        mode = "seq"
    else:
        mode = "par" if any_threads_gt1 else "seq"

    stats = {
        "file": path,
        "mode": mode,                 # 'par' o 'seq'
        "palette": inferred_palette,  # 'neon' / 'ocean' / 'unknown'
        "samples": len(fps_s),
        "fps_mean": mean(fps_s),
        "fps_median": median(fps_s),
        "fps_min": min(fps_s),
        "fps_max": max(fps_s),
    }
    if fps_i:
        stats["fps_inst_mean"] = mean(fps_i)
    return stats


def format_num(x):
    return f"{x:,.3f}"


def auto_discover_files():
    defaults = [f for f in DEFAULT_FILES if os.path.exists(f)]
    if defaults:
        return defaults
    candidates = sorted(glob.glob("par_*.csv") + glob.glob("seq_*.csv"))
    return candidates


def main(argv):
    files = argv[1:]
    if not files:
        files = auto_discover_files()
        if not files:
            print("No se encontraron CSVs. Coloca en esta carpeta alguno de: "
                  f"{', '.join(DEFAULT_FILES)} o usa patrones par_*.csv / seq_*.csv", file=sys.stderr)
            return 2
    all_stats = []
    for p in files:
        try:
            st = read_csv_metrics(p)
            all_stats.append(st)
        except Exception as e:
            print(f"[WARN] {e}", file=sys.stderr)

    if not all_stats:
        print("No se pudieron leer métricas de los archivos proporcionados.", file=sys.stderr)
        return 1

    print("\n=== MÉTRICAS POR ARCHIVO ===\n")
    for st in all_stats:
        print(f"- {os.path.basename(st['file'])} [{st['mode']} | {st['palette']}] "
              f"samples={st['samples']}  fps_mean={format_num(st['fps_mean'])}  "
              f"median={format_num(st['fps_median'])}  "
              f"min={format_num(st['fps_min'])}  max={format_num(st['fps_max'])}\n")

    grouped = defaultdict(lambda: {"par": None, "seq": None})
    for st in all_stats:
        grouped[st["palette"]][st["mode"]] = st

    summary_path = "summary_speedup.csv"
    with open(summary_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["palette", "seq_fps_mean", "par_fps_mean", "speedup_par_over_seq",
                    "seq_file", "par_file"])

        print("\n=== SPEEDUP POR PALETA (par/seq) ===")
        for pal, pair in grouped.items():
            seq = pair["seq"]
            par = pair["par"]
            seq_mean = (seq or {}).get("fps_mean", math.nan)
            par_mean = (par or {}).get("fps_mean", math.nan)

            if seq and par and seq_mean > 0:
                su = par_mean / seq_mean
                print(f"* {pal:>5}: seq={format_num(seq_mean)}  par={format_num(par_mean)}  speedup={format_num(su)}\n")
                w.writerow([pal, f"{seq_mean:.6f}", f"{par_mean:.6f}", f"{su:.6f}",
                            os.path.basename(seq["file"]), os.path.basename(par["file"])])
            else:
                print(f"* {pal:>5}: datos incompletos (necesita seq y par)")
                w.writerow([
                    pal,
                    "" if math.isnan(seq_mean) else f"{seq_mean:.6f}",
                    "" if math.isnan(par_mean) else f"{par_mean:.6f}",
                    "",
                    os.path.basename(seq["file"]) if seq else "",
                    os.path.basename(par["file"]) if par else ""
                ])

    print(f"\nResumen escrito en: {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))