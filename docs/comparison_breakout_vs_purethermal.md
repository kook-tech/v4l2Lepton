# Breakout Board vs PureThermal 상세 비교 분석

## 📋 목차
1. [커스텀 팔레트 비교](#커스텀-팔레트-비교)
2. [기술적 차이점](#기술적-차이점)
3. [성능 및 특성 비교](#성능-및-특성-비교)
4. [사용 사례별 권장사항](#사용-사례별-권장사항)

---

## 🎨 커스텀 팔레트 비교

### ✅ PureThermal: 커스텀 팔레트 **가능**

#### 구현 방법
```c
// LEPTON_VID.h에서 확인
typedef enum LEP_PCOLOR_LUT_E_TAG
{
   LEP_VID_WHEEL6_LUT=0,
   LEP_VID_FUSION_LUT,
   LEP_VID_RAINBOW_LUT,
   LEP_VID_GLOBOW_LUT,
   LEP_VID_SEPIA_LUT,
   LEP_VID_COLOR_LUT,
   LEP_VID_ICE_FIRE_LUT,
   LEP_VID_RAIN_LUT,
   LEP_VID_USER_LUT,        // ← 커스텀 팔레트 옵션
   LEP_VID_END_PCOLOR_LUT
}LEP_PCOLOR_LUT_E;

// 커스텀 팔레트 구조체
typedef struct LEP_VID_LUT_BUFFER_T_TAG
{
   LEP_VID_LUT_PIXEL_T bin[256];  // 256 엔트리
} LEP_VID_LUT_BUFFER_T;

// 각 픽셀은 RGBA (Reserved, Red, Green, Blue)
typedef struct LEP_VID_LUT_PIXEL_T_TAG
{
   LEP_UINT8 reserved;
   LEP_UINT8 red;
   LEP_UINT8 green;
   LEP_UINT8 blue;
} LEP_VID_LUT_PIXEL_T;
```

#### 사용 방법
1. **UVC Extension Unit을 통한 제어**:
   ```c
   // VID 모듈을 통해 커스텀 LUT 업로드
   LEP_VID_LUT_BUFFER_T userLut;
   
   // 256 엔트리로 커스텀 팔레트 구성
   for (int i = 0; i < 256; i++) {
       userLut.bin[i].reserved = 0;
       userLut.bin[i].red = ...;    // 커스텀 R 값
       userLut.bin[i].green = ...;  // 커스텀 G 값
       userLut.bin[i].blue = ...;   // 커스텀 B 값
   }
   
   // Lepton 센서에 커스텀 LUT 업로드
   LEP_SetVidUserLut(&hport_desc, &userLut);
   
   // USER_LUT 선택
   LEP_SetVidPcolorLut(&hport_desc, LEP_VID_USER_LUT);
   ```

2. **특징**:
   - ✅ **256 엔트리** 커스텀 팔레트 지원
   - ✅ **Lepton 하드웨어 레벨**에서 적용 (센서 내부 처리)
   - ⚠️ **고정된 256 엔트리** (사용자 정의 엔트리 수 불가)
   - ⚠️ UVC Extension Unit을 통해 제어 (추가 개발 필요)

---

### ✅ Breakout Board: 커스텀 팔레트 **완전 제어**

#### 구현 방법
```cpp
// Palettes.cpp에서 확인
// 1. 완전 자유로운 팔레트 배열
int colormap_custom[] = {
    R0, G0, B0,
    R1, G1, B1,
    ...
    -1  // 종료 마커
};

// 2. 실시간 팔레트 커스터마이징
void customizePalette(int sigMin, int sigMax, int rangeMin, int rangeMax) {
    // 온도 범위에 따른 동적 팔레트 생성
    // - sigMin/sigMax: 중요 온도 범위 (60% 색상 할당)
    // - rangeMin/rangeMax: 전체 표시 범위
    // - 실시간으로 colormap_custom[] 수정 가능
}

// 3. 소프트웨어 레벨에서 팔레트 적용
// - Raw 데이터를 직접 읽어서
// - 커스텀 팔레트로 변환 후
// - V4L2 출력 버퍼에 쓰기
```

#### 특징
- ✅ **완전 자유로운 엔트리 수** (무제한)
- ✅ **실시간 동적 변경** 가능
- ✅ **소프트웨어 레벨 처리** (완전한 제어)
- ✅ **온도 범위 기반 자동 매핑** 지원
- ✅ **CSV 내보내기** 등 디버깅 도구 포함

---

## 🔬 기술적 차이점 상세 분석

### 1. **팔레트 적용 위치**

| 항목 | Breakout Board | PureThermal |
|------|---------------|-------------|
| **적용 위치** | 소프트웨어 (호스트 PC/Raspberry Pi) | 하드웨어 (Lepton 센서 내부) |
| **처리 단계** | Raw 데이터 → 팔레트 변환 → V4L2 출력 | 센서 내부 → RGB888 출력 → USB 전송 |
| **CPU 부하** | 호스트 CPU 사용 | 센서 내부 처리 (호스트 부하 적음) |

### 2. **팔레트 제약사항**

| 항목 | Breakout Board | PureThermal |
|------|---------------|-------------|
| **엔트리 수** | 제한 없음 (사용자 정의) | 고정 256 엔트리 |
| **동적 변경** | ✅ 실시간 가능 (소프트웨어 레벨) | ⚠️ 가능하나 Lepton에 재업로드 필요 |
| **메모리 사용** | 호스트 메모리 사용 | Lepton 센서 내부 메모리 |
| **커스터마이징 자유도** | ⭐⭐⭐⭐⭐ 완전 자유 | ⭐⭐⭐⭐ 256 엔트리 제한 |

### 3. **팔레트 변경 방식**

#### Breakout Board
```cpp
// 즉시 변경 가능 (메모리 배열 수정)
colormap_custom[i] = newColor;
selectedColormap = colormap_custom;  // 즉시 적용
```

#### PureThermal
```c
// 1. 커스텀 LUT 준비 (256 엔트리)
LEP_VID_LUT_BUFFER_T userLut;
// ... 팔레트 데이터 채우기 ...

// 2. Lepton에 업로드 (I2C 통신 필요)
LEP_SetVidUserLut(&hport_desc, &userLut);

// 3. USER_LUT 선택
LEP_SetVidPcolorLut(&hport_desc, LEP_VID_USER_LUT);
// → 약간의 지연 발생 가능
```

---

## ⚡ 성능 및 특성 비교

### 1. **레이턴시**

| 방식 | 레이턴시 | 원인 |
|------|---------|------|
| **Breakout Board** | ⭐⭐⭐⭐⭐ 매우 낮음 (~1-2ms) | 직접 SPI 통신, 즉시 처리 |
| **PureThermal** | ⭐⭐⭐ 중간 (~10-30ms) | USB 스택, UVC 프로토콜, 센서 내부 처리 |

### 2. **CPU 사용량**

| 방식 | CPU 사용 | 처리 위치 |
|------|---------|----------|
| **Breakout Board** | ⭐⭐⭐ 높음 | 호스트에서 팔레트 변환 처리 |
| **PureThermal** | ⭐⭐⭐⭐ 낮음 | Lepton 센서 내부에서 처리 |

### 3. **팔레트 처리 성능**

| 방식 | 처리량 | 최적화 |
|------|-------|--------|
| **Breakout Board** | ⭐⭐⭐⭐ 높음 (소프트웨어 최적화 가능) | SIMD, 멀티스레드 최적화 가능 |
| **PureThermal** | ⭐⭐⭐⭐⭐ 매우 높음 | 하드웨어 가속 (센서 내부) |

### 4. **메모리 사용**

| 방식 | 메모리 | 특성 |
|------|-------|------|
| **Breakout Board** | ⭐⭐⭐ 호스트 메모리 사용 | 팔레트 + 버퍼 (사용자 제어) |
| **PureThermal** | ⭐⭐⭐⭐⭐ 센서 내부 메모리 | 호스트 메모리 사용 최소화 |

---

## 🎯 사용 사례별 권장사항

### ✅ Breakout Board를 선택해야 하는 경우

1. **고급 커스터마이징 필요**
   - 256 엔트리 이상의 팔레트 필요
   - 실시간 동적 팔레트 변경
   - 온도 범위 기반 자동 매핑

2. **레이턴시가 중요한 애플리케이션**
   - 실시간 제어 시스템
   - 로봇/드론 등 빠른 반응 필요

3. **개발 및 디버깅 편의성**
   - 팔레트 디버깅 도구 필요
   - CSV 내보내기/분석 기능

4. **비용 및 복잡도 고려**
   - PureThermal 보드 추가 구매 비용 없음
   - 기존 Raspberry Pi GPIO 활용

### ✅ PureThermal을 선택해야 하는 경우

1. **물리적 거리 제약**
   - 50cm 이상의 긴 케이블 필요
   - 노이즈/프레임 드롭 문제 해결

2. **CPU 리소스 최적화**
   - 호스트 CPU 부하 최소화
   - 다른 프로세스에 리소스 할당

3. **안정성 우선**
   - USB 표준 프로토콜 사용
   - 하드웨어 레벨 처리

4. **단순한 팔레트 요구사항**
   - 256 엔트리로 충분
   - Lepton 내장 팔레트 활용 가능

---

## 📊 종합 비교표

| 항목 | Breakout Board (SPI) | PureThermal (USB) |
|------|---------------------|-------------------|
| **커스텀 팔레트** | ✅ 가능 (완전 자유) | ✅ 가능 (256 엔트리) |
| **팔레트 엔트리 수** | 무제한 | 256 고정 |
| **동적 변경** | ✅ 실시간 즉시 변경 | ⚠️ 재업로드 필요 |
| **처리 위치** | 호스트 (소프트웨어) | 센서 (하드웨어) |
| **CPU 부하** | 높음 | 낮음 |
| **레이턴시** | ⭐⭐⭐⭐⭐ 매우 낮음 | ⭐⭐⭐ 중간 |
| **안정성 (긴 케이블)** | ⭐⭐ 낮음 | ⭐⭐⭐⭐⭐ 높음 |
| **개발 복잡도** | 중간 | 높음 (UVC Extension) |
| **디버깅 편의성** | ⭐⭐⭐⭐⭐ 쉬움 | ⭐⭐⭐ 중간 |
| **비용** | ⭐⭐⭐⭐⭐ 낮음 | ⭐⭐⭐ 중간 |
| **물리적 거리** | ~30cm 권장 | USB 케이블로 길게 가능 |

---

## 💡 하이브리드 접근법

### 방법 1: Y16 모드 + 소프트웨어 팔레트
```
PureThermal (Y16 raw 모드)
    ↓ USB
호스트 PC/Raspberry Pi
    ↓ 소프트웨어 팔레트 적용
V4L2 출력
```
- ✅ 안정적인 USB 연결
- ✅ 완전한 커스텀 팔레트 제어
- ⚠️ CPU 부하 증가

### 방법 2: 센서 내장 팔레트 활용
```
PureThermal (내장 팔레트 선택)
    ↓ USB
호스트 PC/Raspberry Pi
    ↓ V4L2 출력
```
- ✅ 가장 낮은 CPU 부하
- ⚠️ 256 엔트리 제한
- ⚠️ 커스터마이징 제한적

---

## 📝 결론

### PureThermal의 커스텀 팔레트 지원
- ✅ **가능합니다!** `LEP_VID_USER_LUT`를 통해 256 엔트리 커스텀 팔레트 지원
- ⚠️ Breakout board 방식보다 제한적이지만 실용적인 커스터마이징 가능

### 선택 기준
1. **팔레트 커스터마이징이 핵심** → Breakout Board
2. **안정성과 거리가 핵심** → PureThermal
3. **둘 다 필요** → Y16 모드 + 소프트웨어 팔레트 (하이브리드)

### 권장사항
- 현재 **50cm 이상 케이블 문제**가 있다면 → **PureThermal + Y16 모드 + 소프트웨어 팔레트**
- **긴 케이블 문제 해결**하면서 **커스텀 팔레트 유지** 가능
- CPU 부하는 증가하지만 Raspberry Pi 4 정도면 충분히 처리 가능

