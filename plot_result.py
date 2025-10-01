import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import os

# ------------------ 設定輸出資料夾 ------------------
BASE_DIR = os.path.expanduser("~/ns-3-dev")
IMG_DIR = os.path.join(BASE_DIR, "images")
os.makedirs(IMG_DIR, exist_ok=True)

# ================== 讀取 summary.csv (bandwidth, drop rate) ==================
summary = pd.read_csv("summary.csv", header=None)
summary.columns = ["info", "bandwidth", "dropRate"]

# 解析 info，例如 ps1500_w80_b5
summary["PacketSize"] = summary["info"].str.extract(r'ps(\d+)_')[0].astype(int)
summary["Width"] = summary["info"].str.extract(r'_w(\d+)_')[0].astype(int)
summary["Band"] = summary["info"].str.extract(r'_b(\d+)')[0]

summary["Bandwidth(Mbps)"] = summary["bandwidth"].str.split("=").str[1].astype(float)
summary["DropRate"] = summary["dropRate"].str.split("=").str[1].astype(float)

# Width 改成字串 (加單位)
summary["WidthLabel"] = summary["Width"].astype(str) + " MHz"

# ------------------ 畫 bandwidth ------------------
plt.figure(figsize=(10,6))
sns.barplot(data=summary, x="PacketSize", y="Bandwidth(Mbps)", hue="WidthLabel")
plt.title("Bandwidth vs Packet Size")
plt.ylabel("Bandwidth (Mbps)")
plt.xlabel("Packet Size (bytes)")
plt.legend(title="Channel Width")
plt.tight_layout()
plt.savefig(os.path.join(IMG_DIR, "bandwidth.png"))
plt.close()

# ------------------ 畫 drop rate ------------------
plt.figure(figsize=(10,6))
sns.barplot(data=summary, x="PacketSize", y="DropRate", hue="WidthLabel")
plt.title("Drop Rate vs Packet Size")
plt.ylabel("Drop Rate")
plt.xlabel("Packet Size (bytes)")
plt.legend(title="Channel Width")
plt.tight_layout()
plt.savefig(os.path.join(IMG_DIR, "drop_rate.png"))
plt.close()


# ================== 讀取 latency.csv ==================
latency = pd.read_csv("latency.csv", header=None, names=["time", "latency", "width", "packetSize"], sep=",")
latency["latency"] = pd.to_numeric(latency["latency"], errors="coerce")
latency = latency.dropna()
latency["latency_ms"] = latency["latency"] * 1000

# Width 改成字串 (加單位)
latency["width"] = latency["width"].astype(int).astype(str) + " MHz"

# ================== Latency 分析三件套 ==================
# 1. Latency vs Time (每個 PacketSize 一張圖) 
for ps, df in latency.groupby("packetSize"):
    plt.figure(figsize=(10,6))
    sns.scatterplot(
        data=df,
        x="time",
        y="latency_ms",
        hue="width",
        alpha=0.5,
        s=10
    )
    plt.title(f"Latency vs Time by Channel Width (PacketSize={ps})")
    plt.xlabel("Time (s)")
    plt.ylabel("Latency (ms)")
    plt.ylim(0, 20)
    plt.legend(title="Channel Width")
    plt.tight_layout()
    plt.savefig(os.path.join(IMG_DIR, f"latency_vs_time_ps{ps}.png"))
    plt.close()


# 2. Latency Histogram (Count, FacetGrid)
for ps, df_ps in latency.groupby("packetSize"):
    fig, axes = plt.subplots(1, 2, figsize=(12, 5), sharey=True)  # 兩個子圖，y 軸共用
    
    for ax, (w, df) in zip(axes, df_ps.groupby("width")):
        sns.histplot(df, x="latency_ms", bins=50, color="steelblue", alpha=0.7, ax=ax)
        ax.set_title(f"Channel Width = {w}")
        ax.set_xlabel("Latency (ms)")
        ax.set_ylabel("Count")
        ax.set_xlim(0, 2000)  # 可以調整範圍，例如 0~2000 ms
    
    fig.suptitle(f"Latency Distribution (PacketSize={ps})")
    plt.tight_layout()
    plt.savefig(os.path.join(IMG_DIR, f"latency_hist_ps{ps}.png"))
    plt.close()


# 3. Latency CDF
plt.figure(figsize=(10,6))
sns.ecdfplot(data=latency, x="latency_ms", hue="width", linewidth=2)
plt.title("Latency CDF by Channel Width")
plt.xlabel("Latency (ms)")
plt.ylabel("CDF")
plt.xlim(0, 20)
plt.legend(title="Channel Width")
plt.tight_layout()
plt.savefig(os.path.join(IMG_DIR, "latency_cdf_by_width.png"))
plt.close()

print("bandwidth.png, drop_rate.png, latency_vs_time_by_width.png, latency_hist_facet.png, latency_cdf_by_width.png")
