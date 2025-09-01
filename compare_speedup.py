import os, glob
from collections import Counter, defaultdict
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
WARMUP_SECONDS = 1.0
OUT_DIR = "analysis_output"
SEQ_DIR = os.path.join("secuencial", "runs")
PAR_DIR = os.path.join("paralelo", "runs")

def ensure_outdir(p): os.makedirs(p, exist_ok=True)
def find_csvs(d): return sorted(glob.glob(os.path.join(d, "*.csv")))

def infer_palette_from_df_or_name(df, path):
    if "palette" in df.columns and df["palette"].notna().any():
        v = str(df["palette"].dropna().iloc[-1]).strip().lower()
        if v: return v
    name = os.path.basename(path).lower()
    for pal in ["neon","ocean"]:
        if pal in name: return pal
    return "unknown"

def safe_q(s, q): 
    try: return float(s.quantile(q))
    except: return float("nan")

def summarize_one_run(path, variant):
    try: df = pd.read_csv(path)
    except Exception as e:
        return {"ok": False, "path": path, "error": f"read_error: {e}"}

    for c in ["time_s","fps_inst","smoothed_fps","n","width","height","vsync",
              "threads","ssaa","render_frac","sym","palette"]:
        if c not in df.columns: df[c] = np.nan

    if df["time_s"].notna().any():
        t0 = float(df["time_s"].min())
        df = df[df["time_s"] >= t0 + WARMUP_SECONDS].copy()

    df = df.replace([np.inf,-np.inf], np.nan)
    if "fps_inst" in df.columns:
        df = df[df["fps_inst"].notna() & (df["fps_inst"] > 0)]

    if df.empty:
        return {"ok": False, "path": path, "error": "empty_after_clean"}

    last = df.iloc[-1]
    palette = infer_palette_from_df_or_name(df, path)

    fps = df["fps_inst"].astype(float)
    sm  = df["smoothed_fps"].astype(float)
    frame_ms = 1000.0 / fps.replace(0, np.nan)

    def sstat(s, f, default=np.nan):
        try: return float(f(s)) if len(s) else float(default)
        except: return float(default)

    return {
        "ok": True, "path": path, "file": os.path.basename(path),
        "variant": variant, "palette": palette,
        "rows": int(len(df)),
        "duration_s": float(df["time_s"].max() - df["time_s"].min()) if df["time_s"].notna().any() else np.nan,

        "fps_inst_mean": sstat(fps, pd.Series.mean),
        "fps_inst_median": sstat(fps, pd.Series.median),
        "fps_inst_p05": safe_q(fps, 0.05),
        "fps_inst_p95": safe_q(fps, 0.95),

        "smoothed_fps_mean": sstat(sm, pd.Series.mean),
        "smoothed_fps_median": sstat(sm, pd.Series.median),

        "frame_ms_mean": sstat(frame_ms, pd.Series.mean),
        "frame_ms_median": sstat(frame_ms, pd.Series.median),
        "frame_ms_p95": safe_q(frame_ms, 0.95),
        "frame_ms_p99": safe_q(frame_ms, 0.99),

        "throughput_particles_per_s": sstat(fps, pd.Series.median) * (float(last.get("n", np.nan)) if pd.notna(last.get("n", np.nan)) else 0.0),

        "n": last.get("n", np.nan), "width": last.get("width", np.nan),
        "height": last.get("height", np.nan), "vsync": last.get("vsync", np.nan),
        "threads": last.get("threads", np.nan), "ssaa": last.get("ssaa", np.nan),
        "render_frac": last.get("render_frac", np.nan), "sym": last.get("sym", np.nan),
    }

def summarize_many(csvs, variant):
    rows = [summarize_one_run(p, variant) for p in csvs]
    ok = [r for r in rows if r.get("ok")]
    bad = [r for r in rows if not r.get("ok")]
    if bad: print("⚠️  Ignorando:", [os.path.basename(b["path"]) for b in bad])
    return pd.DataFrame(ok)

def aggregate_by_variant_palette(df):
    if df.empty: return pd.DataFrame()
    def mode_or_nan(s):
        s = s.dropna()
        return float(Counter(s).most_common(1)[0][0]) if len(s) else np.nan
    keys = ["variant","palette"]
    metrics = ["fps_inst_median","fps_inst_mean","smoothed_fps_median","smoothed_fps_mean",
               "frame_ms_median","frame_ms_mean","frame_ms_p95","frame_ms_p99",
               "throughput_particles_per_s","n","width","height","vsync","ssaa","render_frac","sym"]
    agg = df.groupby(keys).agg({m:"median" for m in metrics}).reset_index()
    agg["runs"] = df.groupby(keys)["file"].count().values
    agg["threads_mode"] = df.groupby(keys)["threads"].apply(mode_or_nan).values
    return agg

def aggregate_by_variant(df):
    if df.empty: return pd.DataFrame()
    def mode_or_nan(s):
        s = s.dropna()
        return float(Counter(s).most_common(1)[0][0]) if len(s) else np.nan
    metrics = ["fps_inst_median","fps_inst_mean","smoothed_fps_median","smoothed_fps_mean",
               "frame_ms_median","frame_ms_mean","frame_ms_p95","frame_ms_p99",
               "throughput_particles_per_s","n","width","height","vsync","ssaa","render_frac","sym"]
    agg = df.groupby(["variant"]).agg({m:"median" for m in metrics}).reset_index()
    agg["runs"] = df.groupby("variant")["file"].count().values
    agg["threads_mode"] = df.groupby("variant")["threads"].apply(mode_or_nan).values
    return agg

def compute_speedup_by_variant(agg_var_df):
    if agg_var_df.empty or set(agg_var_df["variant"]) != {"sequential","parallel"}:
        return pd.DataFrame()
    seq = agg_var_df.loc[agg_var_df["variant"]=="sequential","fps_inst_median"].iloc[0]
    par = agg_var_df.loc[agg_var_df["variant"]=="parallel","fps_inst_median"].iloc[0]
    thr = agg_var_df.loc[agg_var_df["variant"]=="parallel","threads_mode"].iloc[0]
    sp = float(par)/float(seq) if float(seq) else np.nan
    eff = sp/float(thr) if (pd.notna(thr) and thr>0) else np.nan
    amd = np.nan
    try:
        p = float(thr)
        if p>1 and sp>0: amd = (p/sp - 1.0)/(p-1.0)
    except: pass
    return pd.DataFrame([{
        "speedup_fps_inst_median": sp,
        "threads_mode": thr,
        "efficiency": eff,
        "amdahl_serial_fraction_est": amd
    }])


def plot_boxplots_runs(df_runs, out_path):
    if df_runs.empty: return
    plt.figure(figsize=(10,5))
    order = []
    for pal in sorted(df_runs["palette"].dropna().unique()):
        order.append((pal,"sequential")); order.append((pal,"parallel"))
    labels, vals = [], []
    for pal,var in order:
        sub = df_runs[(df_runs["palette"]==pal)&(df_runs["variant"]==var)]
        if len(sub):
            labels.append(f"{pal}-{var[:3]}")
            vals.append(sub["fps_inst_median"].values)
    if not vals: return
    plt.boxplot(vals, labels=labels, showfliers=False)
    plt.ylabel("Median FPS (per run)")
    plt.title("Run-level median FPS – distribution")
    plt.xticks(rotation=25)
    plt.tight_layout(); plt.savefig(out_path, dpi=150); plt.close()

def plot_bars_fps_by_variant(agg_var_df, out_path):
    if agg_var_df.empty: return
    order = ["sequential","parallel"]
    vals = [agg_var_df.loc[agg_var_df["variant"]==v,"fps_inst_median"].iloc[0] if v in set(agg_var_df["variant"]) else np.nan for v in order]
    labels = ["Sequential","Parallel"]
    plt.figure(figsize=(6,4))
    plt.bar(np.arange(2), vals)
    plt.xticks(np.arange(2), labels)
    plt.ylabel("Median FPS (instant)")
    plt.title("Median FPS by variant (all palettes)")
    plt.tight_layout(); plt.savefig(out_path, dpi=150); plt.close()

def plot_frame_ms_p95_by_variant(agg_var_df, out_path):
    if agg_var_df.empty: return
    order = ["sequential","parallel"]
    vals = [agg_var_df.loc[agg_var_df["variant"]==v,"frame_ms_p95"].iloc[0] if v in set(agg_var_df["variant"]) else np.nan for v in order]
    labels = ["Sequential","Parallel"]
    plt.figure(figsize=(6,4))
    plt.bar(np.arange(2), vals)
    plt.xticks(np.arange(2), labels)
    plt.ylabel("Frame time p95 (ms) – lower is better")
    plt.title("Frame-time jitter (p95) by variant")
    plt.tight_layout(); plt.savefig(out_path, dpi=150); plt.close()

def plot_throughput_by_variant(agg_var_df, out_path):
    if agg_var_df.empty: return
    order = ["sequential","parallel"]
    vals = [agg_var_df.loc[agg_var_df["variant"]==v,"throughput_particles_per_s"].iloc[0] if v in set(agg_var_df["variant"]) else np.nan for v in order]
    labels = ["Sequential","Parallel"]
    plt.figure(figsize=(6,4))
    plt.bar(np.arange(2), vals)
    plt.xticks(np.arange(2), labels)
    plt.ylabel("Throughput (particles/s) – median")
    plt.title("Throughput by variant")
    plt.tight_layout(); plt.savefig(out_path, dpi=150); plt.close()


def main():
    print("🔎 Buscando CSVs…")
    seq_csvs = find_csvs(SEQ_DIR)
    par_csvs = find_csvs(PAR_DIR)
    print(f"  - secuencial: {len(seq_csvs)} archivos en {SEQ_DIR}")
    print(f"  - paralelo  : {len(par_csvs)} archivos en {PAR_DIR}")

    ensure_outdir(OUT_DIR)

    seq_runs = summarize_many(seq_csvs, "sequential")
    par_runs = summarize_many(par_csvs, "parallel")
    all_runs = pd.concat([seq_runs, par_runs], ignore_index=True)
    if all_runs.empty:
        print("No encontré corridas válidas.")
        return

    all_runs.to_csv(os.path.join(OUT_DIR, "run_summaries.csv"), index=False)
    print(f"Guardado: {OUT_DIR}/run_summaries.csv ({len(all_runs)} filas)")

    agg_pal = aggregate_by_variant_palette(all_runs)
    agg_pal.to_csv(os.path.join(OUT_DIR, "variant_palette_summary.csv"), index=False)

    plot_boxplots_runs(all_runs, os.path.join(OUT_DIR, "fig_fps_median_boxplots.png"))

    agg_var = aggregate_by_variant(all_runs)
    agg_var.to_csv(os.path.join(OUT_DIR, "variant_summary.csv"), index=False)
    plot_bars_fps_by_variant(agg_var, os.path.join(OUT_DIR, "fig_fps_median_by_variant.png"))
    plot_frame_ms_p95_by_variant(agg_var, os.path.join(OUT_DIR, "fig_frame_ms_p95_by_variant.png"))
    plot_throughput_by_variant(agg_var, os.path.join(OUT_DIR, "fig_throughput_by_variant.png"))

    sp_var = compute_speedup_by_variant(agg_var)
    sp_var.to_csv(os.path.join(OUT_DIR, "speedup_by_variant.csv"), index=False)
    if not sp_var.empty:
        s = sp_var["speedup_fps_inst_median"].iloc[0]
        thr = sp_var["threads_mode"].iloc[0]
        eff = sp_var["efficiency"].iloc[0]
        amd = sp_var["amdahl_serial_fraction_est"].iloc[0]
        print(f"\nSpeedup global: {s:.3f}× | hilos≈{int(thr) if pd.notna(thr) else 'NA'} | eficiencia≈{eff:.2f} | Amdahl≈{amd:.3f}")

    print("\nGráficas y CSVs en:", OUT_DIR)
    print("Listo")

if __name__ == "__main__":
    main()