#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

using namespace ns3;

class MyTimestampTag : public Tag
{
public:
  MyTimestampTag() {}
  MyTimestampTag(Time time) : m_time(time) {}

  static TypeId GetTypeId (void)
  {
    static TypeId tid = TypeId("MyTimestampTag")
      .SetParent<Tag>()
      .AddConstructor<MyTimestampTag>();
    return tid;
  }

  virtual TypeId GetInstanceTypeId (void) const
  {
    return GetTypeId();
  }

  virtual void Serialize(TagBuffer i) const
  {
    int64_t t = m_time.GetNanoSeconds();
    i.Write((const uint8_t *)&t, 8);
  }

  virtual void Deserialize(TagBuffer i)
  {
    int64_t t;
    i.Read((uint8_t *)&t, 8);
    m_time = NanoSeconds(t);
  }

  virtual uint32_t GetSerializedSize() const
  {
    return 8;
  }

  virtual void Print(std::ostream &os) const
  {
    os << m_time.GetSeconds();
  }

  Time GetTime() const { return m_time; }

private:
  Time m_time;
};


NS_LOG_COMPONENT_DEFINE("Wifi7Metrics");

// 全域狀態（在 callback 裡要用）
static uint32_t g_currentChannelWidth = 0;
static uint32_t g_packetSize = 0;
static uint32_t g_band = 0;

// 延遲開檔：第一次寫才開，並確保 header 只寫一次（避免全域 ofstream 的生命週期問題）
static std::ofstream& LatencyCsv() {
  static std::ofstream f("latency.csv", std::ios::app);
  static bool header_written = false;
  if (!header_written) {
    f << "time,delay,width,packetSize,band\n";
    header_written = true;
  }
  return f;
}

// static std::ofstream g_latencyCsv("latency.csv");
// ================== Packet Sink Callback ==================
// 讓所有 UdpClient 發出去的封包加上時間戳
void TxCallback(Ptr<const Packet> packet)
{
    // Ptr<Packet> copy = packet->Copy();
    Ptr<Packet> p = ConstCast<Packet>(packet);
    MyTimestampTag t(Simulator::Now());
    p->AddPacketTag(t);
    // NS_LOG_INFO("Tagged packet at " << Simulator::Now().GetSeconds() << "s");
}

void RxCallback(Ptr<const Packet> packet)
{
    // 用 Packet Tag 紀錄發送時間
    MyTimestampTag t;
    if (packet->PeekPacketTag(t))
    {
        Time delay = Simulator::Now() - t.GetTime();
        LatencyCsv() << Simulator::Now().GetSeconds()
             << "," << delay.GetSeconds()
             << "," << g_currentChannelWidth
             << "," << g_packetSize
             << ",b" << g_band << std::endl;

    }
}

// ================== Function ==================
void RunExperiment(uint32_t packetSize, uint32_t channelWidth, uint32_t band, uint32_t nSta, double distance, uint32_t expId, uint32_t totalExps)
{
    double simTime = 10.0;
    std::string prefix = std::string("ps") + std::to_string(packetSize) +
                     "_w" + std::to_string(channelWidth) +
                     "_b" + std::to_string(band) + "GHz";


    // log 一下
    NS_LOG_INFO("=== RunExperiment start ===");
    NS_LOG_INFO("[Experiment " << expId << "/" << totalExps << "] Start "
                << "PacketSize=" << packetSize
                << " bytes, ChannelWidth=" << channelWidth
                << " MHz, Band=" << band << " GHz"
                << ", STA=" << nSta
                << ", Distance=" << distance << "m");
    g_currentChannelWidth = channelWidth;
    g_packetSize = packetSize;
    g_band = band;

    NodeContainer apNode;
    apNode.Create(1); // 1 個 AP 
    NodeContainer staNodes;
    staNodes.Create(nSta); // nSta 個 STA

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211be);  // Wi-Fi 7
    wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager");

    YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());

    // 新版 ns-3: 不能用 Frequency/ChannelWidth，必須用 ChannelSettings
    /*
    channel 不是一個單獨的值，而是 (band, channelNumber, channelWidth) 三個合起來才唯一決定一個 Wi-Fi 通道
    每個 channelNumber 對應一個中心頻率（MHz），但它的「實際頻寬」要看 channelWidth
    例如在 5GHz 頻段，channelNumber=42 → 中心頻率 5210 MHz (80 MHz 頻寬情況下)
    */ 
    if (band == 5) {
    if (channelWidth == 20) {
        phy.Set("ChannelSettings", StringValue("{36, 20, BAND_5GHZ, 0}"));
    } else if (channelWidth == 40) {
        phy.Set("ChannelSettings", StringValue("{36, 40, BAND_5GHZ, 0}"));
    } else if (channelWidth == 80) {
        phy.Set("ChannelSettings", StringValue("{42, 80, BAND_5GHZ, 0}"));  // 80 MHz block
    } else if (channelWidth == 160) {
        phy.Set("ChannelSettings", StringValue("{50, 160, BAND_5GHZ, 0}")); // 160 MHz block
    } else {
        NS_ABORT_MSG("Unsupported channel width for 5 GHz: " << channelWidth);
    }
    }

    else if (band == 6) {
    if (channelWidth == 80) {
        phy.Set("ChannelSettings", StringValue("{7, 80, BAND_6GHZ, 0}"));   // 80 MHz block @ 6 GHz
    } else if (channelWidth == 160) {
        phy.Set("ChannelSettings", StringValue("{15, 160, BAND_6GHZ, 0}")); // 160 MHz block @ 6 GHz
    } else {
        NS_ABORT_MSG("Unsupported channel width for 6 GHz: " << channelWidth);
    }
}

    // 在 ns-3，Wi-Fi 是分成三層物件來組：Channel（空中媒介）→ Phy（實體層）→ Mac（MAC 層）。
    // 三者再組成一個 WifiNetDevice 掛在某個 Node 上。
    WifiMacHelper mac;
    Ssid ssid("wifi7-test");

    // AP
    mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));
    NetDeviceContainer apDev = wifi.Install(phy, mac, apNode.Get(0));

    // STA
    mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid), "ActiveProbing", BooleanValue(true));
    NetDeviceContainer staDevs = wifi.Install(phy, mac, staNodes);

    // Mobility
    // ns-3 的 Node 需要掛一個 MobilityModel 才有位置可算 RSSI 路徑損耗：
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(apNode);
    mobility.Install(staNodes);

    apNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(0, 0, 1));
    for (uint32_t i = 0; i < nSta; i++) {
        staNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(Vector(distance, i, 1));
    }

    // Internet
    InternetStackHelper stack; // InternetStackHelper: 在每個 Node 上裝 TCP/UDP/IPv4/ARP/路由等協定。
    stack.Install(apNode);
    stack.Install(staNodes);

    // Ipv4AddressHelper: 指定一個子網，然後按順序把 IP 分給 NetDeviceContainer 裡的裝置
    Ipv4AddressHelper address;
    address.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer apIf = address.Assign(apDev);    // 給 AP 的 Wi-Fi 裝置配 IP
    Ipv4InterfaceContainer staIf = address.Assign(staDevs); // 給所有 STA 配 IP

    // Traffic: each STA -> AP
    // 在 AP 上跑一個 UDP Server 監聽 port 5000
    UdpServerHelper server(5000);
    ApplicationContainer sink = server.Install(apNode.Get(0));
    sink.Start(Seconds(0.5));
    sink.Stop(Seconds(simTime));

    // 綁定接收 callback
    Ptr<UdpServer> udpServer = DynamicCast<UdpServer>(sink.Get(0));
    udpServer->TraceConnectWithoutContext("Rx", MakeCallback(&RxCallback));

    // 每個 STA 跟 AP 通 (uplink)：持續送資料到 AP:5000
    // 整個系統（全部 STA 加總）在應用層「嘗試」送出的總速率
    double targetRateMbps = 200.0;  // 總流量。接近塞滿 channel，觀察 drop rate, bandwidth
    // 把總速率平均分給每個 STA，所以每個 STA 都會盡量以這個速率送
    double perStaRateMbps = targetRateMbps / nSta;

    for (uint32_t i = 0; i < nSta; i++) {
        UdpClientHelper client(apIf.GetAddress(0), 5000); // 目標 IP = AP 的 IP, 目標 port = 5000
        client.SetAttribute("MaxPackets", UintegerValue(0));  // 無限發

         // 根據封包大小 & STA 分配速率，自動算 interval （每個 STA 的發包間隔）
        double packetsPerSecond = (perStaRateMbps * 1e6) / (packetSize * 8.0); //pps 每秒要送幾包
        double intervalSec = 1.0 / packetsPerSecond;
        client.SetAttribute("Interval", TimeValue(Seconds(intervalSec))); 

        client.SetAttribute("PacketSize", UintegerValue(packetSize)); // 1~10 MTU
        ApplicationContainer app = client.Install(staNodes.Get(i));
        app.Start(Seconds(1.0));
        app.Stop(Seconds(simTime));
    }

    // 綁定 Tx trace (所有 UdpClient)
    Config::ConnectWithoutContext(
        "/NodeList/*/ApplicationList/*/$ns3::UdpClient/Tx",
        MakeCallback(&TxCallback));

    // 綁定 Rx trace (AP 上的 UdpServer)
    udpServer->TraceConnectWithoutContext("Rx", MakeCallback(&RxCallback));


    // FlowMonitor
    // FlowMonitor 是 ns-3 的一個模組，用來監測模擬期間的流量表現，自動搜集
    // Throughput（吞吐量 / bandwidth） , delay , jitter, loss....
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> monitor = fmHelper.InstallAll();

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // 收集結果
    monitor->CheckForLostPackets();
    auto stats = monitor->GetFlowStats();

    std::ofstream summary("summary.csv", std::ios::app);
    for (auto const &kv : stats) {
        const FlowMonitor::FlowStats &st = kv.second;

        // bandwidth (Mbps)
        // 接收端 (AP) 收到的總位元組數 除以 simTime = 平均速率 (bit/s)
        // bandwidth = 實際成功接收到的資料量 / 模擬時間
        double thr_mbps = (st.rxBytes * 8.0) / (simTime * 1e6);

        // drop rate
        // drop rate = 丟包數 / 總發送數
        double dropRate = (double)st.lostPackets / (st.txPackets + st.lostPackets);

        summary << prefix
                << ",Bandwidth=" << thr_mbps
                << ",DropRate=" << dropRate
                << std::endl;
        NS_LOG_INFO("[Experiment " << expId << "] Result: "
                    << " Bandwidth=" << thr_mbps << " Mbps"
                    << " DropRate=" << dropRate);
    }
    summary.close();

    // (選擇性) 輸出到 XML 檔，方便 Python/Gnuplot 分析 latency 分布
    monitor->SerializeToXmlFile("flowmon_" + prefix + ".xml", true, true);

    Simulator::Destroy();
    NS_LOG_INFO("[Experiment " << expId << "/" << totalExps << "] End");
}

// ================== Main ==================
int main(int argc, char *argv[])
{
    LogComponentEnable("Wifi7Metrics", LOG_LEVEL_INFO);
    std::vector<uint32_t> Ps;
    for (uint32_t i = 1; i <= 10; i++) {
        Ps.push_back(i * 1500); // 1~10 MTU
    }

    std::vector<uint32_t> Ws = {80, 160};
    std::vector<uint32_t> Bs = {5,6}; // 先只跑 5 GHz，6 GHz 需要 patch

    uint32_t nSta = 10;    // 10 clients
    double distance = 20.0; // 20m

    uint32_t expId = 0;
    uint32_t totalExps = Ps.size() * Ws.size() * Bs.size();

    for (auto ps : Ps) {
        for (auto w : Ws) {
            for (auto b : Bs) {
                expId++;
                RunExperiment(ps, w, b, nSta, distance, expId, totalExps);
            }
        }
    }

    return 0;
}
