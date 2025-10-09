import pandas as pd
lat = pd.read_csv("latency.csv")
print("✅ 檔案筆數:", len(lat))
print(lat.head())

# 嘗試檢查主要欄位
print("\n📊 欄位資訊:")
print(lat.dtypes)

print("\n📦 band 唯一值:", lat["band"].unique()[:10])

# 測試篩選其中一組
subset = lat.query("width == 80 and band == 'b5'")
print("\n🧩 width=80, band=b5 筆數:", len(subset))
print(subset.head())

if len(subset) > 0:
    import seaborn as sns
    import matplotlib.pyplot as plt
    sns.ecdfplot(data=subset, x="delay", hue="packetSize")
    plt.xlim(0, subset["delay"].quantile(0.99))
    plt.show()
else:
    print("⚠️ subset 為空，圖不可能有線！")

