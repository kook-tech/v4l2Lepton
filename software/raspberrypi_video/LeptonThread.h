#ifndef TEXTTHREAD
#define TEXTTHREAD

#include <ctime>
#include <stdint.h>

#include <QThread>
#include <QtCore>

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
  void setAutomaticScalingRange();
  void useRangeMinValue(uint16_t);
  void useRangeMaxValue(uint16_t);
  void run();
  void stop();  // 종료 요청
  void updateVpipe();
  void open_vpipe();
  void printRawThermalData(int, int, uint16_t);
public slots:
  void performFFC();

signals:
  // Qt GUI를 사용하지 않으므로 시그널 제거

private:

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
  bool shouldStop;  // 종료 플래그
  bool frameValid;  // 현재 프레임이 유효한지

};

#endif
