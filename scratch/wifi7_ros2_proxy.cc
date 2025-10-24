#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("Wifi7Proxy");
// ==========================================================
// Timestamp Tag
// ==========================================================
class MyTimestampTag : public Tag
{
public:
  MyTimestampTag() {}
  MyTimestampTag(Time time) : m_time(time) {}
  static TypeId GetTypeId(void)
  {
    static TypeId tid = TypeId("MyTimestampTag")
                            .SetParent<Tag>()
                            .AddConstructor<MyTimestampTag>();
    return tid;
  }
  virtual TypeId GetInstanceTypeId(void) const { return GetTypeId(); }
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
  virtual uint32_t GetSerializedSize() const { return 8; }
  virtual void Print(std::ostream &os) const { os << m_time.GetSeconds(); }
  Time GetTime() const { return m_time; }

private:
  Time m_time;
};

// ==========================================================
// 全域記錄檔
// ==========================================================
static std::ofstream &LatencyCsv()
{
  static std::ofstream f("latency.csv", std::ios::app);
  static bool header_written = false;
  if (!header_written)
  {
    f << "time,delay,width,packetSize,band\n";
    header_written = true;
  }
  return f;
}

static uint32_t g_currentChannelWidth = 0;
static uint32_t g_packetSize = 0;
static uint32_t g_band = 0;

// ==========================================================
// Ros2 Proxy App
// 在一個節點上，接一個本地 socket（給 ROS2 方向）＋一個 ns-3 socket（給模擬網路方向），
// 把封包在兩個世界之間轉送，並且打上 ns-3 模擬時間戳做延遲統計
// ==========================================================
// 從 ROS2 到 ns-3 的方向： ROS2 發送封包 → m_sockRos 收到 → FromRos2() 被呼叫 → m_sockNs3->SendTo() → ns-3 網路
// 從 ns-3 到 ROS2 的方向： ns-3 網路回傳封包 → m_sockNs3 收到 → FromNs3() 被呼叫 → m_sockRos->SendTo() → ROS2 接收
/*
應用場景舉例
假設有 STA（工作站） 和 AP（存取點）：
在 STA 端：
m_local: 監聽 *:9000（接收來自 ROS2 的資料）
m_remote: 轉發到 AP_IP:5001（送往 AP）

在 AP 端：
m_local: 監聽 *:5001（接收來自 STA 的資料）
m_remote: 轉發到 127.0.0.1:9999（送回本機 ROS2）*/
// ===================== Ros2ProxyApp =====================
class Ros2ProxyApp : public Application
{
public:
  // ===== 初始化設定 =====
  // local: 我要在哪個 port 接收封包（例如 STA:9000, AP:5001）
  // remote: 我要把封包送到哪裡（例如 STA→AP, AP→ROS2 listener）
  void Setup(Address local, Address remote)
  {
    m_local = local;
    m_remote = remote;
  }

private:
  // ===== 啟動時建立兩個 socket =====
  virtual void StartApplication() override
  {
    // (1) 綁定本地 socket，負責接收「從外部來的封包」
    //   STA 模式時：ROS2 Talker → 這裡
    //   AP 模式時：STA 經 Wi-Fi → 這裡
    m_sockRos = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    m_sockRos->Bind(m_local);
    m_sockRos->SetRecvCallback(MakeCallback(&Ros2ProxyApp::FromRos2, this));

    // (2) 建立另一個 socket，負責「往對方送封包」
    //   STA 模式：往 AP 送
    //   AP 模式：往 ROS2 Listener 送
    m_sockNs3 = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    m_sockNs3->SetRecvCallback(MakeCallback(&Ros2ProxyApp::FromNs3, this));
  }

  // ===== 接收到 ROS2 (或外部) 封包時的行為 =====
  // STA 模式：ROS2 Talker → STA proxy (這裡)
  void FromRos2(Ptr<Socket> socket)
  {
    Address from;
    Ptr<Packet> pkt = socket->RecvFrom(from);

    // 在封包上打上 ns-3 模擬時間戳
    MyTimestampTag t(Simulator::Now());
    pkt->AddPacketTag(t);

    // 把封包丟進 ns-3 模擬網路（往 AP proxy）
    m_sockNs3->SendTo(pkt, 0, m_remote);
  }

  // ===== 接收到 ns-3 網路 (例如 AP 收 STA) 的封包時 =====
  // AP 模式：STA → AP proxy (這裡)
  void FromNs3(Ptr<Socket> socket)
  {
    Address from;
    Ptr<Packet> pkt = socket->RecvFrom(from);

    // 檢查封包是否有被打 timestamp tag
    MyTimestampTag t;
    if (pkt->PeekPacketTag(t))
    {
      // 計算模擬時間內的延遲（單純 ns-3 模擬時間差）
      Time delay = Simulator::Now() - t.GetTime();

      // 寫入 latency.csv
      LatencyCsv() << Simulator::Now().GetSeconds()
                   << "," << delay.GetSeconds()
                   << "," << g_currentChannelWidth
                   << "," << g_packetSize
                   << ",b" << g_band
                   << std::endl;
    }

    // 將封包回送到 ROS2 listener (host 或容器)
    m_sockRos->SendTo(pkt, 0, InetSocketAddress(Ipv4Address("127.0.0.1"), 9999));
  }

  // ===== socket 變數 =====
  Ptr<Socket> m_sockRos;   // 本地接收（ROS2方向）或回送用
  Ptr<Socket> m_sockNs3;   // 模擬網路方向（ns-3網路內傳輸）
  Address m_local;         // 我的接收端位址
  Address m_remote;        // 我要送去的對端位址
};


// ==========================================================
// Clock Publisher 
/* ClockPublisherApp 是 ns-3 世界裡的一個「模擬時鐘廣播器」，
    用 UDP 每隔固定時間發出 { "clock": 模擬時間 } 給 ROS2，
    讓 ROS2 node 用 --use-sim-time 跟 ns-3 的時間軸對齊。*/
// ==========================================================
class ClockPublisherApp : public Application
{
public:
  void Setup(Address peer, double interval)
  {
    m_peer = peer;
    m_interval = Seconds(interval);
  }

private:
  void StartApplication() override // 這是 ns-3 生命週期裡 App 啟動時會自動被呼叫 的函式。
  {
    NS_LOG_UNCOND("[ClockPublisherApp] Started at " << Simulator::Now().GetSeconds());
    m_sock = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId()); //這個 socket 負責發封包
    Simulator::ScheduleNow(&ClockPublisherApp::SendClock, this); // 模擬時間 現在 就呼叫 SendClock()（排入 event queue）
  }

  void SendClock() 
  { //所以在 ROS2 端，只要跑一個 UDP receiver 監聽 13337，就能收到ns3 的clock
    NS_LOG_UNCOND("[ClockPublisherApp] Send clock at " << Simulator::Now().GetSeconds());
    std::ostringstream ss;
    ss << "{ \"clock\": " << Simulator::Now().GetSeconds() << " }";
    Ptr<Packet> pkt = Create<Packet>((uint8_t *)ss.str().c_str(), ss.str().length());
    m_sock->SendTo(pkt, 0, m_peer);
    // 告訴模擬器「在 m_interval 秒之後，再呼叫一次 SendClock()」
    Simulator::Schedule(m_interval, &ClockPublisherApp::SendClock, this);
  }

  Ptr<Socket> m_sock;
  Address m_peer;
  Time m_interval;
};

// ==========================================================
// 實驗主程式
// ==========================================================
void RunExperiment(uint32_t packetSize, uint32_t channelWidth, uint32_t band, uint32_t nSta, double distance)
{
  double simTime = 10.0;
  g_currentChannelWidth = channelWidth;
  g_packetSize = packetSize;
  g_band = band;

  NodeContainer apNode;
  apNode.Create(1);
  NodeContainer staNodes;
  staNodes.Create(nSta);

  WifiHelper wifi;
  wifi.SetStandard(WIFI_STANDARD_80211be);
  wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager");

  YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
  YansWifiPhyHelper phy;
  phy.SetChannel(channel.Create());

  if (band == 5)
  {
    if (channelWidth == 80)
      phy.Set("ChannelSettings", StringValue("{42,80,BAND_5GHZ,0}"));
    else if (channelWidth == 160)
      phy.Set("ChannelSettings", StringValue("{50,160,BAND_5GHZ,0}"));
  }
  else if (band == 6)
  {
    if (channelWidth == 80)
      phy.Set("ChannelSettings", StringValue("{7,80,BAND_6GHZ,0}"));
    else if (channelWidth == 160)
      phy.Set("ChannelSettings", StringValue("{15,160,BAND_6GHZ,0}"));
  }

  WifiMacHelper mac;
  Ssid ssid("wifi7-test");

  mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));
  NetDeviceContainer apDev = wifi.Install(phy, mac, apNode.Get(0));

  mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid), "ActiveProbing", BooleanValue(true));
  NetDeviceContainer staDevs = wifi.Install(phy, mac, staNodes);

  MobilityHelper mobility;
  mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobility.Install(apNode);
  mobility.Install(staNodes);
  apNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(0.0, 0.0, 1.0));
  for (uint32_t i = 0; i < nSta; i++)
  {
    double angle = 2 * M_PI * i / nSta;
    double x = distance * std::cos(angle);
    double y = distance * std::sin(angle);
    staNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(Vector(x, y, 1.0));
  }

  InternetStackHelper stack;
  stack.Install(apNode);
  stack.Install(staNodes);

  Ipv4AddressHelper address;
  address.SetBase("10.1.0.0", "255.255.255.0");
  Ipv4InterfaceContainer apIf = address.Assign(apDev);
  Ipv4InterfaceContainer staIf = address.Assign(staDevs);

  // === STA Proxies ===
  // 第 i 個 ROS2 Talker 應該把 UDP 封包送往 宿主機/容器對應的 STA_i 入口 9000+i
  // Ros2ProxyApp::FromRos2() 接到後，打上模擬時間戳，SendTo(AP_IP:5001+i) → 進 Wi-Fi 模擬通道。
  for (uint32_t i = 0; i < nSta; ++i)
  {
    Ptr<Ros2ProxyApp> proxySta = CreateObject<Ros2ProxyApp>();  // 為每個 STA 建立一個 Ros2ProxyApp
    uint16_t rosPort = 9000 + i; // 第 i 個 Talker 指定入口port：9000+i
       // 在 STA 節點上綁定「任意本地位址」+「埠 9000+i」，用來接收 ROS2 Talker 容器送來的封包（uplink 起點）
    proxySta->Setup(InetSocketAddress(Ipv4Address::GetAny(), rosPort), 
                    InetSocketAddress(apIf.GetAddress(0), 5001 + i)); // 把收到的封包往 AP 節點的 port 5001+i 丟
                    // apIf.GetAddress(0) 就是 AP 的 IP（前面分配的 10.1.0.x）
    staNodes.Get(i)->AddApplication(proxySta); // 把 proxy app 加到 STA 節點上
    proxySta->SetStartTime(Seconds(0.5 + 0.1 * i)); // 第 0 個 STA 0.5s 啟動、第 1 個 0.6s 啟動，錯開時間
  }

  // === AP Proxy ===
  //AP 端 Ros2ProxyApp::FromNs3() 收到封包後，會讀出先前的時間戳 → 用 Simulator::Now() 算出純模擬延遲 → 寫 latency.csv。
  //然後把封包轉送給 ROS2 Listener 的 UDP 埠 9999+i
  for (uint32_t i = 0; i < nSta; ++i)
  {
    Ptr<Ros2ProxyApp> proxyAp = CreateObject<Ros2ProxyApp>(); //為每個 STA 通道，在 AP 節點建立一個對應的 proxy（通道一一對應）
    // AP 端在這個埠接收從 STA 經 Wi-Fi 模擬送來的封包 → 對應 STA 端剛才的 m_remote = (AP_IP:5001+i)
    proxyAp->Setup(InetSocketAddress(Ipv4Address::GetAny(), 5001 + i),
    // 把封包送回本地上的 ROS2 Listener（每條通道對應一個埠）。
      // !!!!若 Listener 在 Docker 且非 host 網路，這裡要改成宿主/容器可達的 IP，不是 127.0.0.1
                   InetSocketAddress(Ipv4Address("127.0.0.1"), 9999 + i));
    apNode.Get(0)->AddApplication(proxyAp); // 把這個 AP proxy 掛到 AP 節點（唯一一個 AP，所以 Get(0)）
    proxyAp->SetStartTime(Seconds(0.5 + 0.1 * i));
  }

  // === Clock Publisher === 
  // 把 ns-3 模擬時鐘送給 ROS2
  Ptr<ClockPublisherApp> clockApp = CreateObject<ClockPublisherApp>();
  clockApp->Setup(InetSocketAddress(Ipv4Address("127.0.0.1"), 13337), 0.01);
  apNode.Get(0)->AddApplication(clockApp);
  clockApp->SetStartTime(Seconds(0.1));

  Simulator::Stop(Seconds(simTime));
  std::cout << "Before run, event count = " << Simulator::GetEventCount() << std::endl;
  Simulator::Run();
  Simulator::Destroy();
}

// ==========================================================
// Main
// ==========================================================
int main(int argc, char *argv[])
{
  LogComponentEnable("Wifi7Proxy", LOG_LEVEL_INFO);

  uint32_t nSta = 3;     // 3 talkers (you can increase this)
  double distance = 20.0;
  RunExperiment(1500, 80, 5, nSta, distance);
  return 0;
}
