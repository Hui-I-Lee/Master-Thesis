FROM huiilee142/ros2ns3:armv3

# Copy your workspace
COPY ros2_ws /root/ros2_ws

WORKDIR /root/ros2_ws

# Build ROS2
RUN . /opt/ros/humble/setup.sh && colcon build

# Copy run script
COPY run.sh /root/run.sh
RUN chmod +x /root/run.sh

ENTRYPOINT ["/root/run.sh"]

