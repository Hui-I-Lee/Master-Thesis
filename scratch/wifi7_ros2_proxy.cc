#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

using namespace ns3;

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>


NS_LOG_COMPONENT_DEFINE("Wifi7Proxy");

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

// 全域記錄檔
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
////////
class Ros2ProxyApp : public Application
{
public:
  void Setup(Address local, Address remote)
  {
    m_local = local;
    m_remote = remote;
    m_localPort = InetSocketAddress::ConvertFrom(local).GetPort();
  }

private:
  // ====== ns-3 生命週期 ======
  virtual void StartApplication() override
  {
    NS_LOG_UNCOND("[Ros2ProxyApp] StartApplication at t=" << Simulator::Now().GetSeconds());

    // 1) ns-3 這邊的 socket，用來往 AP / STA 送封包
    m_sockNs3 = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    if (m_localPort >= 5001 && m_localPort < 6000)
    {
        if (m_sockNs3->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_localPort)) == 0)
        {
            NS_LOG_UNCOND("[Ros2ProxyApp] AP ns3 socket bound on port " << m_localPort);
            m_sockNs3->SetRecvCallback(MakeCallback(&Ros2ProxyApp::FromNs3, this));
        }
        else
        {
            NS_LOG_UNCOND("[Ros2ProxyApp] ❌ AP bind failed on port " << m_localPort);
        }
    }

    // 2) 建 real world 的 Linux UDP socket，綁在 host: m_localPort
    m_rosFd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_rosFd < 0)
    {
      NS_LOG_UNCOND("[Ros2ProxyApp] ❌ socket() failed, errno=" << errno);
      return;
    }

    // non-blocking
    int flags = fcntl(m_rosFd, F_GETFL, 0);
    fcntl(m_rosFd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0
    addr.sin_port = htons(m_localPort);

    if (::bind(m_rosFd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
      NS_LOG_UNCOND("[Ros2ProxyApp] ❌ bind() failed on port " << m_localPort
                          << ", errno=" << errno);
      ::close(m_rosFd);
      m_rosFd = -1;
      return;
    }

    NS_LOG_UNCOND("[Ros2ProxyApp] ✅ Linux UDP bound on 0.0.0.0:" << m_localPort);

    m_running = true;
    // 啟動輪詢 real socket 的迴圈（用 ns-3 的事件排程，不用 thread）
    m_pollEvent = Simulator::Schedule(Seconds(0.0),
                                      &Ros2ProxyApp::PollRosSocket, this);
  }

  virtual void StopApplication() override
  {
    NS_LOG_UNCOND("[Ros2ProxyApp] StopApplication at t=" << Simulator::Now().GetSeconds());
    m_running = false;

    if (m_pollEvent.IsRunning())
    {
      Simulator::Cancel(m_pollEvent);
    }

    if (m_sockNs3)
    {
      m_sockNs3->Close();
      m_sockNs3 = nullptr;
    }

    if (m_rosFd >= 0)
    {
      ::close(m_rosFd);
      m_rosFd = -1;
    }
  }

  // ====== 輪詢 host ← ROS2 封包：從 Linux socket 收，丟進 ns-3 ======
  void PollRosSocket()
  {
    if (!m_running || m_rosFd < 0)
      return;

    uint8_t buf[2048];
    sockaddr_in src{};
    socklen_t slen = sizeof(src);

    while (true)
    {
      ssize_t n = ::recvfrom(m_rosFd, buf, sizeof(buf), 0,
                             (sockaddr *)&src, &slen);
      if (n < 0)
      {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          // no more data
          break;
        }
        else
        {
          NS_LOG_UNCOND("[Ros2ProxyApp] recvfrom error, errno=" << errno);
          break;
        }
      }

      NS_LOG_UNCOND("[Ros2ProxyApp] FromRos2 (Linux) got " << n
                       << " bytes at t=" << Simulator::Now().GetSeconds());

      Ptr<Packet> pkt = Create<Packet>(buf, n);

      // 打 tag
      MyTimestampTag t(Simulator::Now());
      pkt->AddPacketTag(t);

      // 丟進 ns-3 網路 (STA -> AP)
      m_sockNs3->SendTo(pkt, 0, m_remote);
    }

    // 再過一小段時間再來 poll 一次
    m_pollEvent = Simulator::Schedule(Seconds(0.0005),
                                      &Ros2ProxyApp::PollRosSocket, this);
  }

  // ====== ns-3 → ROS2  ======
  void FromNs3(Ptr<Socket> socket)
{
  Address from;
  Ptr<Packet> pkt = socket->RecvFrom(from);

  // 1) 讀 timestamp tag、記錄延遲
  MyTimestampTag t;
  if (pkt->PeekPacketTag(t))
  {
    Time delay = Simulator::Now() - t.GetTime();
    LatencyCsv() << Simulator::Now().GetSeconds()
                 << "," << delay.GetSeconds()
                 << "," << g_currentChannelWidth
                 << "," << g_packetSize
                 << ",b" << g_band
                 << std::endl;

    std::cout << "[Ros2ProxyApp][AP] FromNs3: got "
              << pkt->GetSize()
              << " bytes, delay=" << delay.GetSeconds()
              << "s at t=" << Simulator::Now().GetSeconds()
              << std::endl;
  }
  else
  {
    std::cout << "[Ros2ProxyApp][AP] FromNs3: got packet without timestamp tag, size="
              << pkt->GetSize()
              << " at t=" << Simulator::Now().GetSeconds()
              << std::endl;
  }

  // 2) 把 ns-3 的 Packet payload 抽成 buffer
  const uint32_t size = pkt->GetSize();
  if (size == 0)
  {
    return;
  }

  std::vector<uint8_t> buf(size);
  pkt->CopyData(buf.data(), size);

  // 3) 用 Linux UDP 送回 Docker container 裡的 listener: 172.17.0.2:9999
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
  {
    perror("[Ros2ProxyApp][AP] socket");
    return;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9999);              // listener 的 port
  // ⚠ 這裡用現在 single-container 的 IP：172.17.0.2
  // 之後你換成 k3s + hostNetwork，就可以改成 127.0.0.1
  inet_pton(AF_INET, "172.17.0.2", &addr.sin_addr);

  ssize_t sent = ::sendto(fd,
                          buf.data(),
                          size,
                          0,
                          reinterpret_cast<sockaddr*>(&addr),
                          sizeof(addr));
  if (sent < 0)
  {
    perror("[Ros2ProxyApp][AP] sendto");
  }
  else
  {
    std::cout << "[Ros2ProxyApp][AP] sendto listener: "
              << sent << " bytes to 172.17.0.2:9999"
              << std::endl;
  }

  ::close(fd);
}


  // ====== 成員變數 ======
  Ptr<Socket> m_sockNs3;    // ns-3 world 的 socket
  Address     m_local;
  Address     m_remote;
  uint16_t    m_localPort = 0;

  int         m_rosFd = -1; // real Linux socket
  bool        m_running = false;
  EventId     m_pollEvent;
};
/////////////////////////////////////////////////// ////////



// 同步ns3 ros2時間
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
    //NS_LOG_UNCOND("[ClockPublisherApp] Send clock at " << Simulator::Now().GetSeconds());
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


// 實驗主程式
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

  mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid), "ActiveProbing", BooleanValue(false));
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

  NS_LOG_UNCOND("STA: " << staIf.GetAddress(0));
  NS_LOG_UNCOND("AP:  " << apIf.GetAddress(0));

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
    Ptr<Ros2ProxyApp> proxyAp = CreateObject<Ros2ProxyApp>();
    proxyAp->Setup(
        InetSocketAddress(Ipv4Address::GetAny(), 5001 + i),
        InetSocketAddress(Ipv4Address("172.17.0.2"), 9999 + i)
    );

    apNode.Get(0)->AddApplication(proxyAp);
    proxyAp->SetStartTime(Seconds(0.5 + 0.1 * i));
}


  // === Clock Publisher === 
  // 把 ns-3 模擬時鐘送給 ROS2
  Ptr<ClockPublisherApp> clockApp = CreateObject<ClockPublisherApp>();
  clockApp->Setup(InetSocketAddress(Ipv4Address("127.0.0.1"), 13337), 0.01);
  apNode.Get(0)->AddApplication(clockApp);
  clockApp->SetStartTime(Seconds(0.1));

  //Simulator::Stop(Seconds(simTime));
  Simulator::Stop(Time::Max());
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

  uint32_t nSta = 1;     // 3 talkers (you can increase this)
  double distance = 20.0;
  RunExperiment(1500, 80, 5, nSta, distance);
  return 0;
}
