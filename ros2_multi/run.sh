#!/bin/bash
set -e

# Source ROS2
source /opt/ros/humble/setup.bash

# Source workspace
if [ -f "/root/ros2_ws/install/setup.bash" ]; then
  source /root/ros2_ws/install/setup.bash
fi

echo "[Container] ROLE=$ROLE"

if [ "$ROLE" = "talker" ]; then
  exec ros2 run py_pubsub talker
elif [ "$ROLE" = "listener" ]; then
  exec ros2 run py_pubsub listener
else
  echo "Unknown ROLE: $ROLE"
  exit 1
fi

