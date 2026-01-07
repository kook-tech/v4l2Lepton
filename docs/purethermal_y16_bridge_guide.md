# PureThermal Y16 모드 브릿지 가이드

## 📋 핵심 답변

### Q1: Y16 모드가 USB Serial 통신인가?
**답: ❌ 아닙니다. USB Video Class (UVC) 프로토콜입니다.**

- **Serial 통신 (CDC/ACM)이 아님**: 데이터 시리얼 포트가 아님
- **비디오 스트림**: UVC (USB Video Class) 프로토콜 사용
- **V4L2 장치**: `/dev/videoX`로 나타남 (예: `/dev/video4`)

### Q2: 속도는 괜찮은가?
**답: ✅ 충분합니다.**

```
데이터 크기 계산:
- Lepton 3.5: 160 x 120 = 19,200 픽셀
- Y16 포맷: 2 bytes/pixel = 38,400 bytes/frame
- 9 fps 기준: 38,400 × 9 = 345,600 bytes/s ≈ 2.76 Mbps

USB 2.0 대역폭:
- 이론: 480 Mbps
- 실제: 약 280 Mbps
- 사용률: 2.76 / 280 ≈ 1% (매우 여유)

결론: USB 2.0으로 충분하고, 더 높은 프레임레이트도 가능
```

### Q3: Y16 데이터가 비디오로 오는가?
**답: ✅ 네, 비디오 스트림으로 전송됩니다.**

코드 확인 (`usb_task.c:261-277`):
```c
case VS_FMT_INDEX(Y16):
{
    // UVC 비디오 패킷으로 전송
    while (uvc_xmit_row < (IMAGE_NUM_LINES + g_telemetry_num_lines))
    {
        for (i = 0; i < FRAME_LINE_LENGTH; i++)
        {
            uint16_t val = last_buffer->lines.y16[...].data.image_data[i];
            packet[count++] = (uint8_t)((val >> 0) & 0xFF);  // Little-endian
            packet[count++] = (uint8_t)((val >> 8) & 0xFF);
        }
        uvc_xmit_row++;
    }
}
```

### Q4: 브릿지 방식이 맞는가?
**답: ✅ 맞습니다!**

```
[PureThermal] → [USB] → [/dev/video4 (Y16)]
                                ↓
                    [브릿지 프로그램]
                    - Y16 raw 데이터 읽기
                    - 커스텀 팔레트 적용
                    - YUYV422/RGB 변환
                                ↓
                    [/dev/video12 (v4l2loopback)]
```

---

## 🔧 구현 방법

### 1. 하드웨어 확인

```bash
# PureThermal 장치 확인
lsusb | grep -i "PureThermal\|GroupGets"

# V4L2 장치 확인
v4l2-ctl --list-devices

# 예상 출력:
# PureThermal 1 (usb-...):
#   /dev/video4
```

### 2. Y16 포맷 확인

```bash
# Y16 포맷 지원 확인
v4l2-ctl -d /dev/video4 --list-formats-ext

# 예상 출력:
# [0]: 'Y16 ' (16-bit Greyscale)
#         Size: Discrete 160x120
```

### 3. 브릿지 프로그램 구조

```cpp
// 의사 코드
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>

// 1. PureThermal에서 Y16 읽기
int src_fd = open("/dev/video4", O_RDWR);
// Y16 포맷 설정
// 버퍼 할당 (160x120x2 bytes)

// 2. v4l2loopback 가상 장치 생성
// sudo modprobe v4l2loopback video_nr=12
int dst_fd = open("/dev/video12", O_WRONLY);
// YUYV422 또는 RGB888 포맷 설정

// 3. 메인 루프
while (running) {
    // Y16 raw 데이터 읽기
    uint16_t* y16_frame = read_frame(src_fd);
    
    // 커스텀 팔레트 적용
    uint8_t* rgb_frame = apply_palette(y16_frame, colormap_custom);
    
    // YUYV422로 변환 (또는 RGB 그대로)
    uint8_t* yuyv_frame = rgb_to_yuyv(rgb_frame);
    
    // 가상 비디오 장치에 쓰기
    write_frame(dst_fd, yuyv_frame);
}
```

### 4. 실제 구현 예시 (C++)

```cpp
#include <opencv2/opencv.hpp>
#include <linux/videodev2.h>

class PureThermalBridge {
private:
    int src_fd;  // /dev/video4 (PureThermal)
    int dst_fd;  // /dev/video12 (v4l2loopback)
    const int* colormap;
    int colormap_size;
    
public:
    void apply_palette(const uint16_t* y16, uint8_t* rgb, int width, int height) {
        for (int i = 0; i < width * height; i++) {
            // Y16 값을 0-255 인덱스로 매핑
            int idx = map_y16_to_index(y16[i]);
            idx = clamp(idx, 0, colormap_size / 3 - 1);
            
            // 커스텀 팔레트에서 RGB 가져오기
            rgb[i * 3 + 0] = colormap[idx * 3 + 0];  // R
            rgb[i * 3 + 1] = colormap[idx * 3 + 1];  // G
            rgb[i * 3 + 2] = colormap[idx * 3 + 2];  // B
        }
    }
    
    void bridge_loop() {
        const int WIDTH = 160;
        const int HEIGHT = 120;
        
        // Y16 버퍼 (2 bytes/pixel)
        uint16_t y16_buffer[WIDTH * HEIGHT];
        
        // RGB 버퍼 (3 bytes/pixel)
        uint8_t rgb_buffer[WIDTH * HEIGHT * 3];
        
        // YUYV 버퍼 (2 bytes/pixel, 2 pixels per 4 bytes)
        uint8_t yuyv_buffer[WIDTH * HEIGHT * 2];
        
        while (running) {
            // 1. PureThermal에서 Y16 읽기
            read(src_fd, y16_buffer, WIDTH * HEIGHT * 2);
            
            // 2. 커스텀 팔레트 적용
            apply_palette(y16_buffer, rgb_buffer, WIDTH, HEIGHT);
            
            // 3. RGB → YUYV 변환
            rgb_to_yuyv(rgb_buffer, yuyv_buffer, WIDTH, HEIGHT);
            
            // 4. v4l2loopback에 쓰기
            write(dst_fd, yuyv_buffer, WIDTH * HEIGHT * 2);
        }
    }
};
```

### 5. v4l2loopback 설정

```bash
# 모듈 로드
sudo modprobe v4l2loopback video_nr=12 card_label="PureThermal_Bridged"

# 확인
v4l2-ctl --list-devices
# 출력:
# PureThermal Bridged (platform:v4l2loopback-...):
#   /dev/video12
```

---

## 📊 데이터 흐름도

```
┌─────────────────┐
│  PureThermal    │
│  (STM32 MCU)    │
└────────┬────────┘
         │ USB UVC (Y16)
         │ 2.76 Mbps @ 9fps
         ↓
┌─────────────────┐
│  /dev/video4    │  ← Y16 raw 데이터
│  (PureThermal)  │    160x120x2 bytes
└────────┬────────┘
         │
         │ read()
         ↓
┌─────────────────┐
│ Bridge Program  │
│  - Y16 읽기     │
│  - 팔레트 적용  │  ← 커스텀 팔레트
│  - RGB 변환     │     적용
│  - YUYV 변환    │
└────────┬────────┘
         │
         │ write()
         ↓
┌─────────────────┐
│ /dev/video12    │  → YUYV422 컬러 비디오
│ (v4l2loopback)  │    160x120 @ 9fps
└─────────────────┘
         │
         ↓
   [기존 애플리케이션]
   - ffmpeg
   - mediasoup
   - 기타 V4L2 앱
```

---

## ⚡ 성능 최적화

### 1. 메모리 복사 최소화
```cpp
// 버퍼 재사용 (할당 1회)
static uint16_t y16_frame[WIDTH * HEIGHT];
static uint8_t rgb_frame[WIDTH * HEIGHT * 3];
static uint8_t yuyv_frame[WIDTH * HEIGHT * 2];
```

### 2. SIMD 최적화 (선택사항)
```cpp
#include <immintrin.h>
// 팔레트 적용 시 SIMD 사용 가능
```

### 3. 멀티스레딩 (선택사항)
```cpp
// 읽기/변환/쓰기를 파이프라인으로 처리
std::thread read_thread([&]() { /* Y16 읽기 */ });
std::thread convert_thread([&]() { /* 팔레트 적용 */ });
std::thread write_thread([&]() { /* YUYV 쓰기 */ });
```

---

## 🔍 디버깅

### 1. Y16 데이터 확인
```bash
# 프레임 덤프
v4l2-ctl -d /dev/video4 --set-fmt-video=width=160,height=120,pixelformat=Y16
v4l2-ctl -d /dev/video4 --stream-mmap --stream-to=y16_frame.raw --stream-count=1
hexdump -C y16_frame.raw | head
```

### 2. 가상 장치 확인
```bash
# 출력 확인
ffplay /dev/video12
```

### 3. 성능 모니터링
```bash
# CPU 사용률
top -p $(pgrep purethermal_bridge)

# USB 트래픽
sudo cat /sys/kernel/debug/usb/usbmon/*u | grep -i "purethermal"
```

---

## 📝 요약

1. **Y16 모드는 USB Video Class (UVC) 프로토콜** 사용
   - Serial 통신 ❌
   - 비디오 스트림 ✅

2. **속도 충분함** (USB 2.0 기준)
   - 사용률 약 1%
   - 더 높은 프레임레이트 가능

3. **브릿지 방식 맞음**
   - `/dev/video4` (Y16) → 브릿지 → `/dev/video12` (YUYV)
   - 커스텀 팔레트 적용 후 가상 비디오 장치에 출력

4. **기존 코드 재사용 가능**
   - `Palettes.cpp`의 팔레트 코드 그대로 사용
   - Y16 → RGB 변환만 추가하면 됨

---

## 🎯 다음 단계

1. PureThermal 장치 확인 (`lsusb`, `v4l2-ctl`)
2. Y16 포맷 지원 확인 (`--list-formats-ext`)
3. v4l2loopback 설정 (`modprobe`)
4. 브릿지 프로그램 개발 (기존 `Palettes.cpp` 활용)
5. 테스트 및 최적화

