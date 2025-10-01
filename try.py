import pandas as pd

# 讀取 summary.csv
summary = pd.read_csv("summary.csv", header=None)
summary.columns = ["info", "bandwidth", "dropRate"]

# 先去除可能的空白/換行
summary["info"] = summary["info"].str.strip()

print("=== unique info (前幾個) ===")
print(summary["info"].unique()[:10])

# 抓 Band，並先不轉 int，檢查抓到什麼
summary["Band_raw"] = summary["info"].str.extract(r'_b(\d+)$')[0]
print("=== Band_raw ===")
print(summary["Band_raw"].unique()[:10])

# 再轉 int（確保沒有 NaN）
summary = summary.dropna(subset=["Band_raw"])
summary["Band"] = summary["Band_raw"].astype(int)

print("=== 前 5 筆解析後 ===")
print(summary.head())

