#!/bin/bash

# Lepton 3.5 Video Capture Script
# This script runs the raspberrypi_video application with optimized settings

# ============================================
# Configuration - Modify these values as needed
# ============================================

# Lepton type: 2 for Lepton 2.x, 3 for Lepton 3.x/3.5
LEPTON_TYPE=3

# V4L2 capture device for pure_thermal (empty = use SPI)
# When set, captures Y16 from device (e.g. /dev/video0) instead of SPI
V4L2_DEVICE=""

# SPI bus speed in MHz (10-30, default: 20)
# Lower values are more stable but slower
# Higher values are faster but may cause data loss
SPI_SPEED=16

# Temperature range in Celsius
# Minimum temperature for scaling
TEMP_MIN=0

# Maximum temperature for scaling
TEMP_MAX=50

# Signal temperature range for custom palette (Celsius)
# Normal temperature range that will use grayscale
SIG_MIN=20
SIG_MAX=30

# Custom palette version: 1 or 2
PALETTE_VERSION=2

# Colormap: 1=rainbow, 2=grayscale, 3=ironblack, 4=custom
COLORMAP=4

# Log level (0-255, 0=no logs, higher=more verbose)
LOG_LEVEL=0

# ============================================
# Script execution
# ============================================

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# Check if the executable exists
if [ ! -f "./raspberrypi_video" ]; then
    echo "Error: raspberrypi_video executable not found!"
    echo "Please build the project first with: qmake && make"
    exit 1
fi

# Check if running on Raspberry Pi 4 (optional CPU governor setting)
if [ -f /sys/devices/system/cpu/cpufreq/policy0/scaling_governor ]; then
    CURRENT_GOVERNOR=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor)
    if [ "$CURRENT_GOVERNOR" != "performance" ]; then
        echo "Setting CPU governor to performance mode..."
        sudo sh -c "echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor" 2>/dev/null
        if [ $? -eq 0 ]; then
            echo "CPU governor set to performance mode"
        else
            echo "Warning: Could not set CPU governor (may need sudo)"
        fi
    fi
fi

# Display configuration
echo "============================================"
echo "Lepton Video Capture Configuration"
echo "============================================"
echo "Lepton Type: $LEPTON_TYPE"
if [ -n "$V4L2_DEVICE" ]; then
    echo "V4L2 Device: $V4L2_DEVICE (Y16 mode)"
else
    echo "SPI Speed: ${SPI_SPEED}MHz"
fi
echo "Temperature Range: ${TEMP_MIN}°C ~ ${TEMP_MAX}°C"
echo "Signal Range: ${SIG_MIN}°C ~ ${SIG_MAX}°C"
echo "Palette Version: $PALETTE_VERSION"
echo "Colormap: $COLORMAP"
echo "Log Level: $LOG_LEVEL"
echo "============================================"
echo ""

# Build arguments
ARGS="-tl $LEPTON_TYPE -min $TEMP_MIN -max $TEMP_MAX -sigmin $SIG_MIN -sigmax $SIG_MAX -ver $PALETTE_VERSION -cm $COLORMAP -d $LOG_LEVEL"
if [ -n "$V4L2_DEVICE" ]; then
    ARGS="-v4l2 $V4L2_DEVICE $ARGS"
else
    ARGS="-ss $SPI_SPEED $ARGS"
fi

# Run the application
./raspberrypi_video $ARGS

# Exit with the same code as the application
exit $?




