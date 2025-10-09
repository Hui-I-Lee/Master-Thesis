import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import os
import numpy as np

# ------------------ 設定輸出資料夾 ------------------
BASE_DIR = os.path.expanduser("~/ns-3-dev")
IMG_DIR = os.path.join(BASE_DIR, "images")
os.makedirs(IMG_DIR, exist_ok=True)

# ================== 讀取 summary.csv ==================
summary = pd.read_csv("summary.csv", header=None)
summary.columns = ["info", "bandwidth", "dropRate"]

summary["packetSize"] = summary["info"].str.extract(r'ps(\d+)_')[0].astype(int)
summary["width"] = summary["info"].str.extract(r'_w(\d+)_')[0].astype(int)
summary["band"] = summary["info"].str.extract(r'_b(\d+)GHz')[0].astype(int)
summary["throughput"] = summary["bandwidth"].str.split("=").str[1].astype(float)
summary["dropRate"] = summary["dropRate"].str.split("=").str[1].astype(float)
summary_clean = summary[["packetSize", "width", "band", "throughput", "dropRate"]]

# ================== 讀取 latency.csv ==================
lat = pd.read_csv("latency.csv", low_memory=False)
if len(lat) > 200000:
    lat = lat.sample(frac=0.05, random_state=42)
    print(f"⚡ Using 5% sample ({len(lat)} rows) for faster computation")

# ---- 強制轉型 ----
lat["delay"] = pd.to_numeric(lat["delay"], errors="coerce")  # 非數字變 NaN
lat = lat.dropna(subset=["delay"])                           # 丟掉壞掉的 row
lat["latency_ms"] = lat["delay"].astype(float) * 1000

lat["band"] = lat["band"].astype(str).str.strip()
lat = lat[lat["band"].str.startswith("b", na=False)]
lat["band_num"] = lat["band"].str.extract(r"b(\d+)")[0].astype(int)

# ✅ 改用 numpy percentile（快 & 不爆記憶體）
def latency_stats_fast(df):
    vals = df["latency_ms"].to_numpy()
    return pd.Series({
        "p50": np.percentile(vals, 50),
        "p90": np.percentile(vals, 90),
        "p99": np.percentile(vals, 99),
        "mean": vals.mean(),
        "std": vals.std(),
        "le10ms": np.mean(vals <= 10),
    })

lat_stats = (
    lat.groupby(["band_num", "width", "packetSize"], group_keys=False)
       .apply(latency_stats_fast)
       .reset_index()
)

lat_stats.to_csv(os.path.join(IMG_DIR, "latency_summary.csv"), index=False)

# ================== Heatmaps ==================
def plot_heat(df, value, title, fname, fmt=".1f", cmap="mako"):
    pv = df.pivot_table(index="packetSize", columns="width", values=value, aggfunc="mean")
    plt.figure(figsize=(6, 4))
    sns.heatmap(pv, annot=True, fmt=fmt, cmap=cmap)
    plt.title(title)
    plt.xlabel("width (MHz)")
    plt.ylabel("packetSize (bytes)")
    plt.tight_layout()
    plt.savefig(os.path.join(IMG_DIR, fname))
    plt.close()

for band in [5, 6]:
    df_band = summary_clean.query("band == @band")
    lat_band = lat_stats.query("band_num == @band")

    plot_heat(df_band, "throughput", f"Throughput (Mbps) @{band}GHz",
              f"heat_thr_ps_width_{band}g.png", fmt=".0f")
    plot_heat(df_band, "dropRate", f"DropRate @{band}GHz",
              f"heat_drop_ps_width_{band}g.png", fmt=".2f")
    for q in ["p50", "p99"]:  # 🚀 暫時只畫 2 張
        plot_heat(lat_band, q, f"{q} Latency (ms) @{band}GHz",
                  f"heat_{q}_ps_width_{band}g.png", fmt=".1f")
    plot_heat(lat_band, "le10ms", f"≤10ms Ratio @{band}GHz",
              f"heat_le10ms_ps_width_{band}g.png", fmt=".0%")

print("Done: saved summary_clean.csv, latency_summary.csv, and all heatmaps (sampled).")
