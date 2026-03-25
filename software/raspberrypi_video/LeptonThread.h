#ifndef TEXTTHREAD
#define TEXTTHREAD

#include <ctime>
#include <cstdint>
#include <atomic>
#include <vector>
#include <string>

#include <QThread>
#include <QtCore>
#include <QMutex>
#include <QWaitCondition>

#define PACKET_SIZE 164
#define PACKET_SIZE_UINT16 (PACKET_SIZE/2)
#define PACKETS_PER_FRAME 60
#define FRAME_SIZE_UINT16 (PACKET_SIZE_UINT16*PACKETS_PER_FRAME)

class LeptonThread : public QThread
{
  Q_OBJECT;

public:
  LeptonThread();
  ~LeptonThread();

  void setLogLevel(uint16_t);
  void useColormap(int);
  void useLepton(int);
  void useSpiSpeedMhz(unsigned int);
  void useV4l2Input(const char* device);  // pure_thermal 등 V4L2 장치를 Y16 모드로 캡처
  void setAutomaticScalingRange();
  void useRangeMinValue(uint16_t);
  void useRangeMaxValue(uint16_t);
  void run();
  void stop();  // 종료 요청
  void updateVpipe();
  void open_vpipe();
  void printRawThermalData(int, int, uint16_t);

  // On-demand capture: request the next valid frame snapshot and wait for it.
  // This avoids per-frame memcpy overhead and only snapshots when requested (e.g., via MQTT capture_now).
  bool captureNextValidFrame(
      std::vector<uint16_t>& out_raw_u16,
      std::vector<uint8_t>& out_yuyv,
      int& out_width,
      int& out_height,
      uint64_t& out_frame_ts_ms,
      bool& out_frame_valid,
      uint16_t& out_range_min_ck,
      uint16_t& out_range_max_ck,
      float& out_scale,
      int& out_type_colormap,
      int& out_type_lepton,
      int timeout_ms = 2000);

  // Thread-safe snapshot of the last captured frame (raw16 + YUYV stream buffer).
  // Returns false if no capture has been performed yet.
  bool snapshotLatestFrame(
      std::vector<uint16_t>& out_raw_u16,
      std::vector<uint8_t>& out_yuyv,
      int& out_width,
      int& out_height,
      uint64_t& out_frame_ts_ms,
      bool& out_frame_valid,
      uint16_t& out_range_min_ck,
      uint16_t& out_range_max_ck,
      float& out_scale,
      int& out_type_colormap,
      int& out_type_lepton) const;

  // Lightweight per-frame status (updated on every frame; safe to query from other threads).
  // Useful for diagnosing capture_timeout without affecting the hot streaming path.
  bool getLastFrameStatus(
      uint64_t& out_ts_ms,
      bool& out_valid,
      bool& out_frame_incomplete,
      int& out_pixels_processed,
      int& out_expected_pixels,
      int& out_spi_resets,
      int& out_segment_number,
      int& out_wrong_segment_streak,
      int& out_zero_value_drop_streak) const;

  /** After MQTT capture_now 저장 성공 시: v4l2 스트림에만 짧은 캡처 이펙트(저장 JPG/PNG에는 비표시). */
  void notifyCaptureStreamFxStart();

public slots:
  void performFFC();

signals:
  // Qt GUI를 사용하지 않으므로 시그널 제거

private:

  void loadCaptureStreamFxEnv();
  void applyCaptureStreamFxYuyv(uint8_t* dst_yuyv, const uint8_t* src_yuyv, int w, int h, double elapsed_sec) const;
  static double captureFxSmoothstep01(double t);
  static void yuyvToRgb888(const uint8_t* yuyv, int w, int h, std::vector<uint8_t>& rgb);
  static void rgb888ToYuyv(const uint8_t* rgb, int w, int h, uint8_t* yuyv);

  void log_message(uint16_t, std::string);
  uint16_t loglevel;
  int typeColormap;
  const int *selectedColormap;
  int selectedColormapSize;
  int typeLepton;
  unsigned int spiSpeed;
  bool autoRangeMin;
  bool autoRangeMax;
  uint16_t rangeMin;
  uint16_t rangeMax;
  int myImageWidth;
  int myImageHeight;
  uchar*  vidsendbuf;  // YUYV422 버퍼 (QImage 제거, 직접 변환)
  uchar*  prev_vidsendbuf;  // 이전 프레임 버퍼 (프레임 드롭 시 재사용)

  uint8_t result[PACKET_SIZE*PACKETS_PER_FRAME];
  uint8_t shelf[4][PACKET_SIZE*PACKETS_PER_FRAME];
  uint16_t *frameBuffer;
  int v4l2sink;
  static const int V4L2_NBUF = 2;
  void* v4l2_bufs_[V4L2_NBUF];
  size_t v4l2_buf_len_;
  int v4l2_queued_;
  bool use_v4l2_input_;      // true: pure_thermal(V4L2) 캡처, false: SPI 직접 연결
  std::string v4l2_device_;  // V4L2 캡처 장치 경로 (예: /dev/video0)
  int v4l2src_fd_;          // V4L2 캡처용 fd
  static const int V4L2_SRC_NBUF = 4;
  void* v4l2src_bufs_[V4L2_SRC_NBUF];  // mmap 캡처 버퍼 (read() 미지원 드라이버용)
  size_t v4l2src_buf_len_;
  int v4l2src_nbuf_;        // 실제 할당된 캡처 버퍼 수
  bool shouldStop;  // 종료 플래그
  bool frameValid;  // 현재 프레임이 유효한지

  // Latest frame cache (for capture snapshot)
  mutable QMutex latest_mutex_;
  std::vector<uint16_t> latest_raw_u16_;
  std::vector<uint8_t> latest_yuyv_;
  uint64_t latest_ts_ms_ = 0;
  bool latest_valid_ = false;
  uint16_t latest_range_min_ck_ = 0;
  uint16_t latest_range_max_ck_ = 0;
  float latest_scale_ = 0.0f;

  // On-demand capture synchronization
  std::atomic<bool> capture_requested_{false};
  mutable QMutex capture_mutex_;
  QWaitCondition capture_cv_;
  uint64_t capture_seq_ = 0;

  // Per-frame diagnostics (no frame buffers; just counters/flags)
  mutable QMutex frame_stats_mutex_;
  uint64_t last_frame_ts_ms_ = 0;
  bool last_frame_valid_ = false;
  bool last_frame_incomplete_ = false;
  int last_pixels_processed_ = 0;
  int last_expected_pixels_ = 0;
  int last_spi_resets_ = 0;
  int last_segment_number_ = -1;
  int last_wrong_segment_streak_ = 0;
  int last_zero_value_drop_streak_ = 0;

  // capture_now 후 v4l2 스트림 전용 이펙트 (rgb_stream_capture.py 와 동일 ENV 이름 사용)
  bool capture_fx_enabled_ = false;
  double capture_fx_shrink_sec_ = 0.16;
  double capture_fx_hold_sec_ = 0.10;
  double capture_fx_fade_sec_ = 0.26;
  double capture_fx_speed_ = 0.5;
  double capture_fx_inset_ratio_ = 0.03;
  /** 테두리/십자 선 두께(픽셀): RGB_CAPTURE_FX_LINE_REF 짧은 변 기준으로 스케일. */
  int capture_fx_line_px_ = 2;
  int capture_fx_line_ref_min_ = 480;
  int capture_fx_rgb_r_ = 220;
  int capture_fx_rgb_g_ = 40;
  int capture_fx_rgb_b_ = 40;
  std::atomic<std::int64_t> capture_fx_t0_ns_{0};
  mutable std::vector<uint8_t> capture_fx_scratch_yuyv_;
  mutable std::vector<uint8_t> capture_fx_rgb_;

};

#endif

