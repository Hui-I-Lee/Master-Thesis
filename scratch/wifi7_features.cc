#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

using namespace ns3;

static std::ofstream g_csv("features.csv");
NS_LOG_COMPONENT_DEFINE("MultiWifi7");

// --- 全域變數 ---
std::map<uint64_t, Time> g_pktEnq;                // 封包入佇列時間 (算 MACQ)
std::map<uint32_t, double> g_rssiMap;             // STA → 最新 RSSI
std::map<uint32_t, double> g_macqMap;             // STA → 最新 MACQ
std::map<uint32_t, Mac48Address> g_assocMap;      // STA → AP
std::map<uint32_t, uint32_t> g_apQueueLen;        // AP nodeId → queue length

uint32_t g_band = 5;
uint32_t g_channelWidth = 80;
double g_simTime = 10.0;
uint32_t g_packetSize = 1500;

// === RSSI Trace ===
void SniffRx(Ptr<const Packet>, uint16_t, WifiTxVector, MpduInfo, SignalNoiseDbm sn, uint16_t) {
  uint32_t nodeId = Simulator::GetContext();
  g_rssiMap[nodeId] = sn.signal;
}

// === MAC Queue Delay ===
void EnqueueTrace(Ptr<const WifiMpdu> mpdu) {
  g_pktEnq[mpdu->GetPacket()->GetUid()] = Simulator::Now();
}
void DequeueTrace(Ptr<const WifiMpdu> mpdu) {
  auto it = g_pktEnq.find(mpdu->GetPacket()->GetUid());
  if (it != g_pktEnq.end()) {
    Time sojourn = Simulator::Now() - it->second;
    g_macqMap[Simulator::GetContext()] = sojourn.GetMilliSeconds();
    g_pktEnq.erase(it);
  }
}

// === AP QueueLen Trace ===
void ApQueueLenTrace(uint32_t oldValue, uint32_t newValue) {
  uint32_t nodeId = Simulator::GetContext(); // AP node ID
  g_apQueueLen[nodeId] = newValue;
}

// === Association Trace ===
void AssocCallback(std::string context, Mac48Address apAddr) {
  size_t start = context.find("NodeList/") + 9;
  size_t end = context.find("/", start);
  uint32_t staId = std::stoi(context.substr(start, end - start));
  g_assocMap[staId] = apAddr;

  std::cout << "[LOG] STA=" << staId 
            << " associated to " << apAddr 
            << " at t=" << Simulator::Now().GetSeconds()
            << std::endl;

  // 延遲 0.05 秒再安裝 client，模擬 DHCP/ARP
  Simulator::Schedule(Seconds(0.05), [staId, apAddr]() {
    // 找到 AP 的 IPv4
    Ipv4Address apIp;
    bool found = false;
    for (uint32_t j = 0; j < NodeList::GetNNodes(); j++) {
      Ptr<Node> node = NodeList::GetNode(j);
      for (uint32_t k = 0; k < node->GetNDevices(); k++) {
        Ptr<WifiNetDevice> apDev = DynamicCast<WifiNetDevice>(node->GetDevice(k));
        if (apDev && apDev->GetMac()->GetAddress() == apAddr) {
          Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
          if (!ipv4) continue;
          for (uint32_t ifIndex = 0; ifIndex < ipv4->GetNInterfaces(); ifIndex++) {
            for (uint32_t addrIndex = 0; addrIndex < ipv4->GetNAddresses(ifIndex); addrIndex++) {
              Ipv4Address testIp = ipv4->GetAddress(ifIndex, addrIndex).GetLocal();
              if (testIp != Ipv4Address::GetLoopback()) {
                apIp = testIp;
                found = true;
                break;
              }
            }
            if (found) break;
          }
        }
      }
      if (found) break;
    }
    if (!found) return;

    Ptr<Node> staNode = NodeList::GetNode(staId);
    UdpClientHelper client(apIp, 5000);
    client.SetAttribute("MaxPackets", UintegerValue(0));
    client.SetAttribute("Interval", TimeValue(MilliSeconds(10)));
    client.SetAttribute("PacketSize", UintegerValue(g_packetSize));
    auto app = client.Install(staNode);
    app.Start(Seconds(Simulator::Now().GetSeconds() + 0.01));
    app.Stop(Seconds(g_simTime));

    std::cout << "[CLIENT] STA=" << staId 
              << " started traffic to " << apIp 
              << " at t=" << Simulator::Now().GetSeconds()
              << std::endl;
  });
}

// === Snapshot Logging ===
void LogBssLoad(NodeContainer staNodes, NetDeviceContainer apDevs, Ptr<FlowMonitor> fm) {
  if (!fm) return;
  auto stats = fm->GetFlowStats();

  // 聚合 flow 指標（全域）
  uint64_t rxBytes = 0, rxPkts = 0, loss = 0;
  double delay_sum = 0.0, jitter_sum = 0.0;
  for (auto const &kv3 : stats) {
    const auto &st = kv3.second;
    // 跳過完全沒有封包的 flow，避免 nullptr 問題
    if (st.rxPackets == 0 && st.txPackets == 0) continue;
    rxBytes += st.rxBytes;
    loss    += st.lostPackets;
    rxPkts  += st.rxPackets;
    if (st.rxPackets > 0) delay_sum += st.delaySum.GetSeconds();
    if (st.rxPackets > 1) jitter_sum += st.jitterSum.GetSeconds();
  }
  double thr_mbps   = (rxBytes * 8.0) / (g_simTime * 1e6);
  double meanDelay  = (rxPkts > 0) ? (delay_sum / rxPkts) : NAN;
  double meanJitter = (rxPkts > 1) ? (jitter_sum / (rxPkts - 1)) : NAN;

  for (auto const &kv : g_assocMap) {
    uint32_t staId = kv.first;
    Mac48Address apAddr = kv.second;

    // RSSI / MACQ
    double rssi = g_rssiMap.count(staId) ? g_rssiMap[staId] : NAN;
    double macq = g_macqMap.count(staId) ? g_macqMap[staId] : NAN;

    // AP StaCount
    uint32_t staCount = 0;
    for (auto const &kv2 : g_assocMap) {
      if (kv2.second == apAddr) staCount++;
    }

    // AP QueueLen (with nullptr protection)
    uint32_t apNodeId = UINT32_MAX;  // 預設 invalid
    for (uint32_t j = 0; j < apDevs.GetN(); j++) {
      Ptr<WifiNetDevice> apDev = DynamicCast<WifiNetDevice>(apDevs.Get(j));
      if (!apDev) continue;  // 保護1: apDev 空

      Ptr<WifiMac> mac = apDev->GetMac();
     if (!mac) continue;    // 保護2: mac 空

      if (mac->GetAddress() == apAddr) {
        Ptr<Node> apNode = apDev->GetObject<Node>();
        if (!apNode) continue;  // 保護3: node 空

       apNodeId = apNode->GetId();
        break;
     }
}

// 如果找不到 AP，queueLen = 0
uint32_t queueLen = 0;
if (apNodeId != UINT32_MAX && g_apQueueLen.count(apNodeId)) {
  queueLen = g_apQueueLen[apNodeId];
}

    // === 輸出到 CSV ===
    g_csv << Simulator::Now().GetSeconds()
          << ",STA=" << staId
          << ",AP=" << apAddr
          << ",RSSI=" << rssi
          << ",MACQ=" << macq
          << ",AP_StaCount=" << staCount
          << ",AP_QueueLen=" << queueLen
          << ",Throughput=" << thr_mbps
          << ",Delay=" << meanDelay
          << ",Jitter=" << meanJitter
          << ",Loss=" << loss
          << ",band=" << g_band
          << ",chWidth=" << g_channelWidth
          << "\n";
  }
}

int main(int argc, char *argv[]) {
  uint32_t nSta = 6;
  double distance = 20.0;

  CommandLine cmd;
  cmd.AddValue("nSta", "Number of STAs", nSta);
  cmd.AddValue("distance", "Distance", distance);
  cmd.AddValue("band", "Band (5 or 6)", g_band);
  cmd.AddValue("channelWidth", "Channel width", g_channelWidth);
  cmd.AddValue("packetSize", "Packet size", g_packetSize);
  cmd.AddValue("simTime", "Simulation time", g_simTime);
  cmd.Parse(argc, argv);

  NodeContainer apNodes, staNodes;
  apNodes.Create(2);
  staNodes.Create(nSta);

  WifiHelper wifi;
  wifi.SetStandard(WIFI_STANDARD_80211be);
  wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager");

  YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
  YansWifiPhyHelper phy;
  phy.SetChannel(channel.Create());

  // === Band/ChannelWidth ===
  if (g_band == 5) {
    if (g_channelWidth == 80)  phy.Set("ChannelSettings", StringValue("{42, 80, BAND_5GHZ, 0}"));
    if (g_channelWidth == 160) phy.Set("ChannelSettings", StringValue("{50,160, BAND_5GHZ, 0}"));
  } else if (g_band == 6) {
    if (g_channelWidth == 80)  phy.Set("ChannelSettings", StringValue("{7,  80, BAND_6GHZ, 0}"));
    if (g_channelWidth == 160) phy.Set("ChannelSettings", StringValue("{15,160, BAND_6GHZ, 0}"));
  }

  WifiMacHelper mac;
  Ssid ssid("wifi7-test");

  NetDeviceContainer apDevs;
  for (uint32_t i = 0; i < apNodes.GetN(); i++) {
    mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));
    apDevs.Add(wifi.Install(phy, mac, apNodes.Get(i)));
  }
  mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid), "ActiveProbing", BooleanValue(true));
  NetDeviceContainer staDevs = wifi.Install(phy, mac, staNodes);

  MobilityHelper mob;
  mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mob.Install(apNodes);
  mob.Install(staNodes);

  apNodes.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(0, 0, 1));
  apNodes.Get(1)->GetObject<MobilityModel>()->SetPosition(Vector(distance, 0, 1));
  for (uint32_t i = 0; i < nSta; i++) {
    if (i % 2 == 0)
      staNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(Vector(5, i, 1));
    else
      staNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(Vector(distance - 5, i, 1));
  }

  InternetStackHelper stack;
  stack.Install(apNodes);
  stack.Install(staNodes);

  Ipv4AddressHelper address;
  address.SetBase("10.1.0.0", "255.255.255.0");
  Ipv4InterfaceContainer apIf = address.Assign(apDevs);
  Ipv4InterfaceContainer staIf = address.Assign(staDevs);

  // UDP Server 在兩個 AP
  UdpServerHelper server(5000);
  ApplicationContainer sinks;
  sinks.Add(server.Install(apNodes.Get(0)));
  sinks.Add(server.Install(apNodes.Get(1)));
  sinks.Start(Seconds(0.5));
  sinks.Stop(Seconds(g_simTime));

  // RSSI Trace
  for (uint32_t i=0; i<staDevs.GetN(); i++) {
    Ptr<WifiNetDevice> wnd = DynamicCast<WifiNetDevice>(staDevs.Get(i));
    wnd->GetPhy()->TraceConnectWithoutContext("MonitorSnifferRx", MakeCallback(&SniffRx));
  }
  // MACQ Trace
  Config::ConnectWithoutContext(
    "/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/BE_Txop/$ns3::QosTxop/Queue/Enqueue",
    MakeCallback(&EnqueueTrace));
  Config::ConnectWithoutContext(
    "/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/BE_Txop/$ns3::QosTxop/Queue/Dequeue",
    MakeCallback(&DequeueTrace));
  // Assoc Trace
  Config::Connect(
    "/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/$ns3::StaWifiMac/Assoc",
    MakeCallback(&AssocCallback));
  // AP QueueLen Trace
  Config::ConnectWithoutContext(
    "/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/Mac/$ns3::ApWifiMac/BE_Txop/$ns3::QosTxop/Queue/PacketsInQueue",
    MakeCallback(&ApQueueLenTrace));

  // FlowMonitor
  FlowMonitorHelper fmHelper;
  Ptr<FlowMonitor> fm = fmHelper.InstallAll();

  // 每秒記錄一次
  for (double t = 2; t <= g_simTime; t += 1.0) {
    Simulator::Schedule(Seconds(t), &LogBssLoad, staNodes, apDevs, fm);
  }

  Simulator::Stop(Seconds(g_simTime));
  Simulator::Run();
  Simulator::Destroy();

  g_csv.close();
  return 0;
}
