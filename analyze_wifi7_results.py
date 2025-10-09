# analyze_wifi7_results.py
import re
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt
import os

sns.set(style="whitegrid", font_scale=1.2)

# ------------------ 設定輸出資料夾 ------------------
BASE_DIR = os.path.expanduser("~/ns-3-dev")
IMG_DIR = os.path.join(BASE_DIR, "images")
os.makedirs(IMG_DIR, exist_ok=True)

# 清空舊圖檔（避免混亂）
for f in os.listdir(IMG_DIR):
    if f.endswith(".png"):
        os.remove(os.path.join(IMG_DIR, f))

# =========================
# 讀取 summary.csv
# =========================
def load_summary():
    rows = []
    with open("summary.csv", "r") as f:
        for line in f:
            m = re.match(r"ps(\d+)_w(\d+)_b(\d+)GHz.*Bandwidth=(.*),DropRate=(.*)", line.strip())
            if not m:
                continue
            ps, w, b, bw, dr = m.groups()
            rows.append({
                "packetSize": int(ps),
                "width": int(w),
                "band": int(b),
                "Bandwidth_Mbps": float(bw),
                "DropRate": float(dr)
            })
    df = pd.DataFrame(rows)
    return df


# =========================
# Load data
# =========================
df = load_summary()

# =========================
# 2️⃣ Throughput vs Packet Size
# =========================
plt.figure(figsize=(8, 5))

# 把 hue + style 整合成新欄位
# 合併欄位為 label，並避免浮點數字串格式錯誤
df["label"] = df.apply(lambda row: f'{int(row["width"])}MHz / {int(row["band"])}GHz', axis=1)

palette = {
    '80MHz / 5GHz': '#4C72B0',
    '80MHz / 6GHz': '#55A868',
    '160MHz / 5GHz': '#C44E52',
    '160MHz / 6GHz': '#8172B3',
}

# 畫圖（用 label 作為唯一分群）
sns.lineplot(
    data=df,
    x="packetSize",
    y="Bandwidth_Mbps",
    hue="label",
    style="label",
    markers=True,
    dashes=True,
    linewidth=2,
    palette=palette
)

plt.title("Aggregate Throughput vs Packet Size")
plt.xlabel("Packet Size (Bytes)")
plt.ylabel("Throughput (Mbps)")
plt.grid(True)

# legend 放圖內、字體縮小
plt.legend(
    title="Width / Band",
    loc="lower right",
    fontsize=9,
    title_fontsize=10,
    frameon=True,
    facecolor='white',
    edgecolor='gray'
)

plt.tight_layout()
plt.savefig(os.path.join(IMG_DIR, "throughput_vs_packet_clean.png"), bbox_inches="tight")
plt.clf()



# =========================
# 3️⃣ Drop Rate vs Packet Size (Enhanced)
# =========================
plt.figure(figsize=(8,6))

# 對數 X 軸，讓小封包區不會擠在一起
sns.lineplot(
    data=df,
    x="packetSize",
    y="DropRate",
    hue="width",
    style="band",
    markers=True,
    dashes=False,
    linewidth=2.2,
    palette="Set1"  # 顏色飽和一點
)

plt.xscale("log")
plt.xticks([512, 1500, 4096, 8192, 16384, 32768, 65507],
           ["512", "1.5K", "4K", "8K", "16K", "32K", "65K"])

plt.title("Drop Rate vs Packet Size", fontsize=15, weight="bold")
plt.xlabel("Packet Size (Bytes)", fontsize=13)
plt.ylabel("Drop Rate", fontsize=13)
plt.legend(title="Width (MHz) / Band (GHz)", fontsize=11, title_fontsize=11, loc="upper right")
plt.grid(True, alpha=0.3)
plt.tight_layout()
plt.savefig(os.path.join(IMG_DIR, "droprate_vs_packet_pretty.png"), dpi=300)
plt.close()

# =========================
# 4️⃣ Latency CDF (legend for 7 packet sizes)
# =========================
# 淺色到深色，封包小到大
if os.path.exists("latency.csv"):
    import numpy as np

    lat = pd.read_csv("latency.csv", low_memory=False)
    n = len(lat)
    print(f"✅ Loaded latency.csv ({n:,} rows)")

    # ---- 根據資料大小自動取樣 ----
    if n > 2_000_000:
        frac = 0.02
    elif n > 500_000:
        frac = 0.05
    else:
        frac = 1.0

    if frac < 1.0:
        lat = lat.sample(frac=frac, random_state=42)
        print(f"⚡ Using {frac*100:.0f}% sample ({len(lat):,} rows) for faster plotting")

    # ---- 型別轉換與清理 ----
    lat["delay"] = pd.to_numeric(lat["delay"], errors="coerce")
    lat["width"] = pd.to_numeric(lat["width"], errors="coerce")
    lat["packetSize"] = pd.to_numeric(lat["packetSize"], errors="coerce")
    lat["band"] = lat["band"].astype(str).str.strip()
    lat = lat.dropna(subset=["delay", "width", "packetSize", "band"])

    # ---- 分組繪圖 ----
    groups = list(lat.groupby(["width", "band"]))
    print(f"📊 Generating {len(groups)} latency CDF plots...")

    for (w, b), g in groups:
        vals = pd.to_numeric(g["delay"], errors="coerce").to_numpy()
        vals = vals[np.isfinite(vals)]
        vals = vals[vals > 0]
        if vals.size == 0:
            print(f"⚠️ {w}MHz {b}GHz: no positive delays, skip.")
            continue

        p99 = np.quantile(vals, 0.99)
        band_clean = str(b).replace("b", "")

        plt.figure(figsize=(7,5))

        # ✅ 封包大小轉成字串 → legend 顯示每個值
        g["packetSize"] = g["packetSize"].astype(int).astype(str)

        sns.ecdfplot(
            data=g,
            x="delay",
            hue="packetSize",
            palette="viridis",
            legend="full"
        )

        plt.xscale("log")
        plt.xlim(1e-4, p99)
        plt.xlabel("Delay (s, log scale)")
        plt.ylabel("Cumulative Probability")
        plt.legend(
            title="Packet Size (Bytes)",
            fontsize=9,
            title_fontsize=10,
            loc="lower right"
        )
        plt.title(f"Latency CDF – {int(w)} MHz, {band_clean} GHz")
        plt.grid(True)
        plt.tight_layout()
        plt.savefig(os.path.join(IMG_DIR, f"latency_cdf_w{int(w)}_b{band_clean}.png"))
        plt.close()

    print(f"✅ All latency CDF plots saved in {IMG_DIR}/")


# =========================
# 5️⃣ Publish Rate (bonus)
# =========================
if os.path.exists("publish_rate.csv"):
    pub = pd.read_csv("publish_rate.csv", low_memory=False)
    plt.figure(figsize=(8,5))
    sns.lineplot(data=pub, x="packetSize", y="publishRate_pps",
                 hue="width", style="band", markers=True)
    plt.title("Publish Rate vs Packet Size")
    plt.xlabel("Packet Size (Bytes)")
    plt.ylabel("Publish Rate (packets/s)")
    plt.yscale("log")  # 對數軸視覺化反比關係
    plt.grid(True, which="both")
    plt.tight_layout()
    plt.savefig(os.path.join(IMG_DIR, "publishrate_vs_packet.png"))
    plt.close()

print(f"✅ All plots saved in '{IMG_DIR}/' (throughput, drop rate, latency CDF, publish rate if present).")
