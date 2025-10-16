import os
import re
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt

# === 基本設定 ===
sns.set_theme(style="whitegrid", context="talk", palette="deep")
output_dir = "/home/ubuntu/ns-3-dev/images"
os.makedirs(output_dir, exist_ok=True)

# === 1️⃣ Parse summary.csv ===
summary_data = []
with open("summary.csv") as f:
    for line in f:
        line = line.strip()
        m = re.match(r"ps(\d+)_w(\d+)_b(\d+)GHz,Bandwidth=([\d\.eE\-\+]+),DropRate=([\d\.eE\-\+]+)", line)
        if m:
            ps, w, b, bw, dr = m.groups()
            summary_data.append({
                "PacketSize": int(ps),
                "ChannelWidth": int(w),
                "Band": int(b),
                "Bandwidth": float(bw),
                "DropRate": float(dr)
            })

df = pd.DataFrame(summary_data)
if df.empty:
    raise ValueError("❌ summary.csv 解析失敗，請確認格式與範例一致。")

# === 2️⃣ Parse latency.csv ===
lat = pd.read_csv("latency.csv", dtype=str, low_memory=False)
lat = lat.dropna(subset=["band"])
lat["Band"] = lat["band"].astype(str).str.extract(r"b(\d+)")   # 提取數字
lat = lat.dropna(subset=["Band"])
lat["Band"] = lat["Band"].astype(int)

lat["packetSize"] = pd.to_numeric(lat["packetSize"], errors="coerce")
lat["width"] = pd.to_numeric(lat["width"], errors="coerce")
lat["delay"] = pd.to_numeric(lat["delay"], errors="coerce")

lat_summary = (
    lat.groupby(["packetSize", "width", "Band"])["delay"]
    .agg(["mean", "std"])
    .reset_index()
)
lat_summary.rename(columns={
    "packetSize": "PacketSize",
    "width": "ChannelWidth",
    "mean": "Latency",
    "std": "LatencyStd"
}, inplace=True)

# === 3️⃣ Merge summary 與 latency ===
merged = pd.merge(df, lat_summary, on=["PacketSize", "ChannelWidth", "Band"], how="inner")

# === 4️⃣ 定義繪圖函式 ===
def plot_trends(data, logx=False):
    custom_palette = {5: "#d62728", 6: "#1f77b4"}
    metrics = ["Latency", "Bandwidth", "DropRate"]
    titles = ["Latency vs PacketSize", "Bandwidth vs PacketSize", "DropRate vs PacketSize"]
    ylabels = ["Latency (s)", "Throughput (Mbps)", "Drop Rate"]

    fig, axes = plt.subplots(3, 1, figsize=(10, 14))

    for ax, metric, title, ylabel in zip(axes, metrics, titles, ylabels):
        sns.lineplot(
            data=data,
            x="PacketSize",
            y=metric,
            hue="Band",
            style="ChannelWidth",
            palette=custom_palette,
            markers=True,
            dashes=False,
            linewidth=2.2,
            markersize=8,
            ax=ax
        )
        if logx:
            ax.set_xscale("log")
        ax.set_title(title, fontsize=18, pad=10)
        ax.set_ylabel(ylabel, fontsize=14)
        ax.set_xlabel("PacketSize", fontsize=13)
        ax.tick_params(axis='both', labelsize=12)
        ax.grid(True, linestyle='--', alpha=0.5)
        ax.legend_.remove()

    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(
        handles,
        labels,
        title="Band / ChannelWidth",
        loc="center left",
        bbox_to_anchor=(1.03, 0.5),
        frameon=False,
        fontsize=12,
        title_fontsize=13
    )

    plt.tight_layout(rect=[0, 0, 0.88, 1])

    suffix = "_logx" if logx else ""
    output_path = os.path.join(output_dir, f"wifi7_experiment_packetsize_trends{suffix}.png")
    plt.savefig(output_path, dpi=400, bbox_inches="tight")
    plt.show()
    print(f"✅ 已完成：{output_path}")

# === 5️⃣ 繪製一般尺度 ===
plot_trends(merged, logx=False)

# === 6️⃣ 繪製對數尺度（log₁₀(PacketSize)）===
plot_trends(merged, logx=True)
