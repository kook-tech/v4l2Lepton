#!/bin/bash

# 1. v4l2loopback 모듈 제거 및 재로딩
sudo modprobe -r v4l2loopback
sudo modprobe v4l2loopback video_nr=4,5 card_label="Lepton","LeptonOverlay" exclusive_caps=1,1

# 2. 디바이스 설정 완료 대기
udevadm settle

# 3. /dev/video4가 생성될 때까지 대기
while [ ! -e /dev/video4 ]; do
    sleep 0.1
done

# 4. 실행
./raspberrypi_video -tl 3 -ss 30 -min 0 -max 50 -sigmin 20 -sigmax 30 -ver 2 -cm 4
