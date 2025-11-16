import rclpy
from rclpy.node import Node
import socket

class Listener(Node):
    def __init__(self):
        super().__init__('listener')

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", 9999))

        self.get_logger().info("UDP Listener started ← ns-3 proxy")
        self.timer = self.create_timer(0.01, self.recv_msg)

    def recv_msg(self):
        self.sock.settimeout(0.0001)
        try:
            data, addr = self.sock.recvfrom(2048)
            self.get_logger().info(f"Received: {data} from {addr}")
        except:
            pass

def main():
    rclpy.init()
    node = Listener()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    node.destroy_node()
    rclpy.shutdown()

if __name__ == '__main__':
    main()

