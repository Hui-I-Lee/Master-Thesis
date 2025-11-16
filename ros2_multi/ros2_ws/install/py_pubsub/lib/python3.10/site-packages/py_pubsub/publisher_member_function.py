import rclpy
from rclpy.node import Node
import socket
import time
import os

class Talker(Node):
    def __init__(self):
        super().__init__('talker')
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        target_port = int(os.getenv("TARGET_PORT", "9000"))
        self.target = ('172.17.0.1', target_port)
        self.seq = 0
        self.timer = self.create_timer(0.1, self.publish_msg)
        self.get_logger().info("UDP Talker started → ns-3 proxy")

    def publish_msg(self):
        self.seq += 1
        timestamp = time.time()
        msg = f"{self.seq},{timestamp:.6f}".encode()
        self.sock.sendto(msg, self.target)
        self.get_logger().info(f"Sent: {msg}")

def main():
    rclpy.init()
    node = Talker()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    node.destroy_node()
    rclpy.shutdown()

if __name__ == '__main__':
    main()

