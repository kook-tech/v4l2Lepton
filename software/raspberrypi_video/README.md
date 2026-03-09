## raspberrypi_video (v4l2Lepton)

라즈베리파이에서 FLIR Lepton(2.x/3.x) 프레임을 읽어서 팔레트(컬러맵)를 적용한 뒤, **V4L2 video output**(`/dev/video12`)으로 스트리밍하는 프로그램입니다.

추가로, 스트리밍은 유지하면서 **로컬 MQTT**로 스냅샷 캡쳐(1회)를 요청하면, 요청된 경로에 **JPG + RAW16 덤프**를 저장하고 결과를 MQTT로 응답할 수 있습니다.

---

## 빌드/실행(라즈베리파이)

1) SPI/I2C 활성화:

```bash
sudo raspi-config
```

2) 의존성(환경에 따라 qt 버전은 다를 수 있음):

```bash
sudo apt-get install qt4-dev-tools
```

3) 빌드:

```bash
qmake && make
```

4) 정리(clean):

```bash
make sdkclean && make distclean
```

---

## 실행 예시

### Lepton 2.x

```bash
./raspberrypi_video
```

### Lepton 3.x

```bash
./raspberrypi_video -tl 3
```

### pure_thermal (V4L2 Y16 캡처)

pure_thermal 보드를 `/dev/video0` 등 V4L2 장치로 사용할 때, `-v4l2` 옵션으로 Y16 모드를 자동 설정하고 캡처합니다:

```bash
./raspberrypi_video -v4l2 /dev/video0 -tl 3
```

`run.sh`에서 `V4L2_DEVICE="/dev/video0"`으로 설정해도 됩니다.

라즈베리파이 4에서 CPU governor를 올리고 싶으면:

```bash
sudo sh -c "echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
```

---

## MQTT 스냅샷 캡쳐 (local-only)

이 캡쳐는 **로컬 전용 토픽**을 사용하며(RMS로 브릿지 금지), TVOC와 동일한 “cmd/result” 패턴을 따릅니다.

### 토픽

- **명령(subscribe)**: `local/ir/cmd`
- **결과(publish)**: `local/ir/capture_result`

호환(레거시) 토픽:

- `local/thermal/cmd` (구독만 지원)

### 요청 JSON (`capture_now`)

`local/ir/cmd`로 publish:

```json
{
  "cmd": "capture_now",
  "ts_ms": 1760000000000,
  "request_id": "optional-string",
  "out": {
    "jpg_path": "/var/lib/payload_driver/jobs/20260113/944/27/ir_FN.jpg",
    "raw_path": "/var/lib/payload_driver/jobs/20260113/944/27/ir_FN_raw16.raw",
    "jpg_quality": 90
  }
}
```

- **`out.jpg_path`**: 필수. 팔레트 적용된 스냅샷 JPG 저장 경로
- **`out.raw_path`**: 필수. raw16 덤프 저장 경로
- **`out.jpg_quality`**: 옵션. JPG quality(기본 90)
- `out.meta_path`: 옵션(레거시/디버그용). 보통은 `capture_result`에 포함된 메타를 `actions_result.json`에 넣는 방식 권장

### 응답 JSON (`capture_result`)

`local/ir/capture_result`로 publish:

```json
{
  "type": "thermal_capture_result",
  "cmd": "capture_now",
  "status": "completed",
  "ts_ms": 1760000001234,
  "request_ts_ms": 1760000000000,
  "request_id": "optional-string",
  "frame_ts_ms": 1760000001200,
  "frame_valid": true,
  "width": 160,
  "height": 120,

  "range_min_ck": 30000,
  "range_max_ck": 32000,
  "range_min_c": 26.85,
  "range_max_c": 46.85,

  "scale_min_ck": 30500,
  "scale_max_ck": 31500,
  "scale_min_c": 31.85,
  "scale_max_c": 41.85,

  "scale": 1.5,
  "colormap_type": 4,
  "lepton_type": 3,

  "raw_format": "uint16",
  "raw_endianness": "little",
  "raw_unit": "centi_kelvin",

  "jpg_path": "/var/lib/payload_driver/jobs/20260113/944/27/ir_FN.jpg",
  "raw_path": "/var/lib/payload_driver/jobs/20260113/944/27/ir_FN_raw16.raw"
}
```

### 응답 필드 의미(키별)

- **`type`**: 메시지 타입 식별자(현재 `"thermal_capture_result"` 고정)
- **`cmd`**: 요청 커맨드 에코(현재 `"capture_now"` 고정)
- **`status`**: 캡쳐 결과 상태
  - `completed`: JPG + RAW 저장까지 완료
  - `failed`: 실패(아래 `error` 포함 가능)
- **`error`**: 실패 사유(실패 케이스에만 포함될 수 있음)
  - 예: `busy`(이미 처리중), `capture_timeout`, `out.jpg_path and out.raw_path are required`, 파일 저장 실패 메시지 등
- **`ts_ms`**: 이 응답을 publish한 시각(ms)
- **`request_ts_ms`**: 요청 payload의 `ts_ms`(없으면 내부에서 수신 시각으로 채움). 요청/응답 매칭용
- **`request_id`**: 요청 payload의 `request_id`를 그대로 에코(옵션). 요청/응답 매칭용
- **`frame_ts_ms`**: 캡쳐에 사용된 프레임 타임스탬프(ms)
- **`frame_valid`**: 캡쳐 시점 프레임 유효 여부(센서/프레임 상태 디버깅용)
- **`width` / `height`**: 프레임 해상도(픽셀)
- **`jpg_path` / `raw_path`**: 실제로 저장된 파일 경로(요청의 out.*와 동일)
- **`meta_path`**: 요청에서 `out.meta_path`가 주어진 경우에만 포함(레거시/디버그용)
- **`range_min_ck` / `range_max_ck`**: raw 프레임에서 계산한 “실제 프레임” 픽셀 min/max (unit: centiKelvin)
- **`range_min_c` / `range_max_c`**: `range_*_ck`를 Celsius로 변환한 값
- **`scale_min_ck` / `scale_max_ck`**: 팔레트 매핑(스케일링)에 사용된 범위 min/max (unit: centiKelvin)
- **`scale_min_c` / `scale_max_c`**: `scale_*_ck`를 Celsius로 변환한 값
- **`scale`**: 팔레트 매핑을 위한 스케일 계수(디버그/리플레이용)
- **`colormap_type`**: 사용한 컬러맵 타입(예: 4=custom)
- **`lepton_type`**: Lepton 타입(예: 2 또는 3)
- **`raw_format`**: raw 덤프 데이터 타입(현재 `"uint16"`)
- **`raw_endianness`**: raw 덤프 엔디안(현재 `"little"`)
- **`raw_unit`**: raw 덤프 물리 단위(현재 `"centi_kelvin"`, 예: 27315 = 0°C)
- **`capture_timeout_ms`**: 캡쳐 타임아웃(ms). 환경변수 `IR_CAPTURE_TIMEOUT_MS`로 조절 가능(기본 2000)
- **`last_*` 진단 필드들**: `capture_timeout` 발생 시 “마지막 프레임 상태”를 추가로 포함할 수 있습니다.
  - `last_frame_ts_ms`, `last_frame_age_ms`
  - `last_frame_valid`, `last_frame_incomplete`
  - `last_pixels_processed`, `last_expected_pixels`
  - `last_spi_resets`, `last_segment_number`
  - `last_wrong_segment_streak`, `last_zero_value_drop_streak`
  - 의미: **스트리밍은 이전 프레임 재사용으로 계속 유지될 수 있지만**, 캡쳐는 `frameValid==true`인 “완전한 프레임”이 올 때까지 기다리므로, 불완전 프레임이 반복되면 타임아웃이 발생할 수 있습니다.

#### range_min/max 의 의미(중요)

- **`range_min_* / range_max_*`는 raw 덤프(`raw_path`)의 “실제 프레임” 전체 픽셀에서 계산한 min/max 입니다.**
  - 즉 **팔레트 매핑 범위 내에서만** 계산한 값이 아니라, raw 프레임 전체 기반입니다.
- **`scale_min_* / scale_max_*`는 팔레트 매핑(스케일링)에 사용된 범위**입니다(설정 또는 auto-range 결과).

### RAW16 덤프 포맷 (`out.raw_path`)

- 타입: `uint16`
- 엔디안: little-endian
- 단위: centiKelvin(`cK`)
- 레이아웃: row-major, `width * height` 픽셀

---

## 배선 참고

Lepton 모듈은 SPI, 전원(GND/3v3), I2C(SDA/SCL)을 라즈베리파이 GPIO에 연결해야 합니다.
