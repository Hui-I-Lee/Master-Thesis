import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import os

# ------------------ 設定輸出資料夾 ------------------
BASE_DIR = os.path.expanduser("~/ns-3-dev")
IMG_DIR = os.path.join(BASE_DIR, "images")
os.makedirs(IMG_DIR, exist_ok=True)

# ================== 讀取 summary.csv ==================
summary = pd.read_csv("summary.csv", header=None)
summary.columns = ["info", "bandwidth", "dropRate"]

# 解析 info，例如 ps1500_w80_b5GHz
summary["PacketSize"] = summary["info"].str.extract(r'ps(\d+)_')[0].astype(int)
summary["Width"] = summary["info"].str.extract(r'_w(\d+)_')[0].astype(int)
summary["Band"] = summary["info"].str.extract(r'_b(\d+)GHz')[0].astype(int)

summary["Bandwidth(Mbps)"] = summary["bandwidth"].str.split("=").str[1].astype(float)
summary["DropRate"] = summary["dropRate"].str.split("=").str[1].astype(float)

# Label 欄位
summary["WidthLabel"] = summary["Width"].astype(str) + " MHz"
summary["BandLabel"] = summary["Band"].astype(str) + " GHz"
summary["Config"] = summary["BandLabel"] + " / " + summary["WidthLabel"]

# ------------------ 畫 bandwidth ------------------
g = sns.catplot(
    data=summary,
    x="PacketSize", y="Bandwidth(Mbps)",
    hue="WidthLabel",
    col="BandLabel", col_wrap=2,
    kind="bar", height=5, aspect=1.2
)
g.set_axis_labels("Packet Size (bytes)", "Bandwidth (Mbps)")
g.set_titles("Band = {col_name}")
plt.tight_layout()
plt.savefig(os.path.join(IMG_DIR, "bandwidth_by_band.png"))
plt.close()

# ------------------ 畫 drop rate ------------------
g = sns.catplot(
    data=summary,
    x="PacketSize", y="DropRate",
    hue="WidthLabel",
    col="BandLabel", col_wrap=2,
    kind="bar", height=5, aspect=1.2
)
g.set_axis_labels("Packet Size (bytes)", "Drop Rate")
g.set_titles("Band = {col_name}")
plt.tight_layout()
plt.savefig(os.path.join(IMG_DIR, "drop_rate_by_band.png"))
plt.close()

# ================== 讀取 latency.csv ==================
latency = pd.read_csv("latency.csv")
latency.columns = ["time", "latency", "width", "packetSize", "band_raw"]

# 轉數值
latency["latency_ms"] = latency["latency"] * 1000
latency["WidthLabel"] = latency["width"].astype(str) + " MHz"
latency["Band"] = latency["band_raw"].str.extract(r'b(\d+)')[0].astype(int)
latency["BandLabel"] = latency["Band"].astype(str) + " GHz"
latency["Config"] = latency["BandLabel"] + " / " + latency["WidthLabel"]

# ================== Latency 分析三件套 ==================
# 1. Latency vs Time (每個 PacketSize 一張圖)
for ps, df in latency.groupby("packetSize"):
    plt.figure(figsize=(10,6))
    sns.scatterplot(
        data=df,
        x="time",
        y="latency_ms",
        hue="Config",
        alpha=0.5,
        s=10
    )
    plt.title(f"Latency vs Time (PacketSize={ps})")
    plt.xlabel("Time (s)")
    plt.ylabel("Latency (ms)")
    plt.ylim(0, 20)
    plt.legend(title="Config")
    plt.tight_layout()
    plt.savefig(os.path.join(IMG_DIR, f"latency_vs_time_ps{ps}.png"))
    plt.close()

# 2. Latency Histogram (FacetGrid by Band × Width)
for (band, width, ps), df_bwp in latency.groupby(["BandLabel", "WidthLabel", "packetSize"]):
    plt.figure(figsize=(8,5))
    sns.histplot(df_bwp, x="latency_ms", bins=100, color="steelblue", alpha=0.7)
    plt.title(f"Latency Distribution ({band}, {width}, PacketSize={ps})")
    plt.xlabel("Latency (ms)")
    plt.ylabel("Count")
    plt.xlim(0, 200)  # 這裡可以依需求調整，例如 0~200ms
    plt.tight_layout()
    plt.savefig(os.path.join(IMG_DIR, f"latency_hist_{band}_{width}_ps{ps}.png"))
    plt.close()

# 3. Latency CDF
plt.figure(figsize=(10,6))
sns.ecdfplot(data=latency, x="latency_ms", hue="Config", linewidth=2)
plt.title("Latency CDF by Band and Width")
plt.xlabel("Latency (ms)")
plt.ylabel("CDF")
plt.xlim(0, 20)
plt.legend(title="Config")
plt.tight_layout()
plt.savefig(os.path.join(IMG_DIR, "latency_cdf_by_band_width.png"))
plt.close()

print("bandwidth_by_band.png, drop_rate_by_band.png, latency_vs_time_ps*.png, latency_hist_by_band_width.png, latency_cdf_by_band_width.png")
