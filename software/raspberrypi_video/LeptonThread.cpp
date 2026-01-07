#include <iostream>

#include "LeptonThread.h"
#include <linux/videodev2.h>

#include "Palettes.h"
#include "SPI.h"
#include "Lepton_I2C.h"
//
#include <cmath>
#define PACKET_SIZE 164
#define PACKET_SIZE_UINT16 (PACKET_SIZE/2)
#define PACKETS_PER_FRAME 60
#define FRAME_SIZE_UINT16 (PACKET_SIZE_UINT16*PACKETS_PER_FRAME)
#define FPS 27;

LeptonThread::LeptonThread() : QThread()
{
	//
	loglevel = 0;
	shouldStop = false;  // 종료 플래그 초기화
	vidsendbuf = nullptr;  // 버퍼 초기화
	prev_vidsendbuf = nullptr;  // 이전 프레임 버퍼 초기화
	v4l2sink = -1;  // 파일 디스크립터 초기화
	frameValid = false;  // 프레임 유효성 플래그 초기화

	//
	typeColormap = 3; // 1:colormap_rainbow  /  2:colormap_grayscale  /  3:colormap_ironblack(default)
	selectedColormap = colormap_ironblack;
	selectedColormapSize = get_size_colormap_ironblack();

	//
	typeLepton = 2; // 2:Lepton 2.x  / 3:Lepton 3.x
	myImageWidth = 80;
	myImageHeight = 60;

	//
	spiSpeed = 5 * 1000 * 1000; // SPI bus speed 20MHz -> 5MHz

	// min/max value for scaling
	autoRangeMin = true;
	autoRangeMax = true;
	rangeMin = 30000;
	rangeMax = 32000;
	open_vpipe();
}

LeptonThread::~LeptonThread() {
	stop();  // 종료 요청
	wait(5000);  // 최대 5초 대기
	if (isRunning()) {
		terminate();  // 강제 종료
		wait(1000);
	}
	// 리소스 정리
	if (vidsendbuf) {
		free(vidsendbuf);
		vidsendbuf = nullptr;
	}
	if (prev_vidsendbuf) {
		free(prev_vidsendbuf);
		prev_vidsendbuf = nullptr;
	}
	if (v4l2sink >= 0) {
		close(v4l2sink);
		v4l2sink = -1;
	}
}

void LeptonThread::stop() {
	shouldStop = true;
}

void LeptonThread::setLogLevel(uint16_t newLoglevel)
{
	loglevel = newLoglevel;
}

void LeptonThread::useColormap(int newTypeColormap)
{
	switch (newTypeColormap) {
	case 1:
		typeColormap = 1;
		selectedColormap = colormap_rainbow;
		selectedColormapSize = get_size_colormap_rainbow();
		break;
	case 2:
		typeColormap = 2;
		selectedColormap = colormap_grayscale;
		selectedColormapSize = get_size_colormap_grayscale();
		break;
	case 4:
		typeColormap = 4;
		selectedColormap = colormap_custom;
		selectedColormapSize = get_size_colormap_custom();
		break;
	default:
		typeColormap = 3;
		selectedColormap = colormap_ironblack;
		selectedColormapSize = get_size_colormap_ironblack();
		break;
	}
}

void LeptonThread::useLepton(int newTypeLepton)
{
	switch (newTypeLepton) {
	case 3:
		typeLepton = 3;
		myImageWidth = 160;
		myImageHeight = 120;
		break;
	default:
		typeLepton = 2;
		myImageWidth = 80;
		myImageHeight = 60;
	}
}

void LeptonThread::useSpiSpeedMhz(unsigned int newSpiSpeed)
{
	spiSpeed = newSpiSpeed * 1000 * 1000;
}

void LeptonThread::setAutomaticScalingRange()
{
	autoRangeMin = true;
	autoRangeMax = true;
}

void LeptonThread::useRangeMinValue(uint16_t newMinValue)
{
	autoRangeMin = false;
	rangeMin = newMinValue;
}

void LeptonThread::useRangeMaxValue(uint16_t newMaxValue)
{
	autoRangeMax = false;
	rangeMax = newMaxValue;
}

void LeptonThread::run()
{
	const int *colormap = selectedColormap;
	const int colormapSize = selectedColormapSize;
	uint16_t minValue = rangeMin;
	uint16_t maxValue = rangeMax;
	float diff = maxValue - minValue;
	float scale = 3000/diff; 
	// 온도 민감도 0.05 -> -10 ~ 140도는 3000개 컬러맵 필요. ( -10 ~ 140 : default gain mode - high )
	uint16_t n_wrong_segment = 0;
	uint16_t n_zero_value_drop_frame = 0;
	
	// YUYV422 버퍼: 2 pixels per 4 bytes
	// 홀수 픽셀 처리를 위한 임시 저장
	uint8_t prev_r = 0, prev_g = 0, prev_b = 0;
	bool has_prev_pixel = false;

	//open spi port
	SpiOpenPort(0, spiSpeed);

	while(!shouldStop) {

		//read data packets from lepton over SPI
		int resets = 0;
		int segmentNumber = -1;
		bool frameIncomplete = false;  // 프레임 불완전 플래그 (루프 시작 시 초기화)
		for(int j=0;j<PACKETS_PER_FRAME;j++) {
			//if it's a drop packet, reset j to 0, set to -1 so he'll be at 0 again loop
			read(spi_cs0_fd, result+sizeof(uint8_t)*PACKET_SIZE*j, sizeof(uint8_t)*PACKET_SIZE);
			int packetNumber = result[j*PACKET_SIZE+1];
			if(packetNumber != j) {
				j = -1;
				resets += 1;
				// 적응형 대기 시간: 드롭이 적으면 짧게, 많으면 길게
				// resets < 10: 1ms (빠른 복구 시도)
				// resets < 30: 2ms (안정성 우선)
				// resets >= 30: 3ms (심각한 동기화 문제)
				int waitTime = (resets < 10) ? 1000 : (resets < 30) ? 2000 : 3000;
				usleep(waitTime);
				//Note: we've selected 750 resets as an arbitrary limit, since there should never be 750 "null" packets between two valid transmissions at the current poll rate
				//By polling faster, developers may easily exceed this count, and the down period between frames may then be flagged as a loss of sync
				if(resets == 750) {
					SpiClosePort(0);
					lepton_reboot();
					n_wrong_segment = 0;
					n_zero_value_drop_frame = 0;
					usleep(750000);
					SpiOpenPort(0, spiSpeed);
				}
				continue;
			}
			if ((typeLepton == 3) && (packetNumber == 20)) {
				segmentNumber = (result[j*PACKET_SIZE] >> 4) & 0x0f;
				if ((segmentNumber < 1) || (4 < segmentNumber)) {
					log_message(10, "[ERROR] Wrong segment number " + std::to_string(segmentNumber));
					break;
				}
			}
		}
		if(resets >= 30) {
			log_message(3, "done reading, resets: " + std::to_string(resets));
		}

		int iSegmentStart = 1;
		int iSegmentStop;
		if (typeLepton == 3) {
			if ((segmentNumber < 1) || (4 < segmentNumber)) {
				n_wrong_segment++;
				if ((n_wrong_segment % 12) == 0) {
					log_message(5, "[WARNING] Got wrong segment number continuously " + std::to_string(n_wrong_segment) + " times");
				}
				frameIncomplete = true;  // 잘못된 세그먼트로 인한 프레임 불완전
				// 이전 프레임 재사용하고 다음 프레임으로
				if (prev_vidsendbuf != nullptr) {
					memcpy(vidsendbuf, prev_vidsendbuf, myImageWidth * myImageHeight * 2);
				} else {
					memset(vidsendbuf, 0, myImageWidth * myImageHeight * 2);
				}
				updateVpipe();
				continue;
			}
			if (n_wrong_segment != 0) {
				log_message(8, "[WARNING] Got wrong segment number continuously " + std::to_string(n_wrong_segment) + " times [RECOVERED] : " + std::to_string(segmentNumber));
				n_wrong_segment = 0;
			}

			//
			memcpy(shelf[segmentNumber - 1], result, sizeof(uint8_t) * PACKET_SIZE*PACKETS_PER_FRAME);
			if (segmentNumber != 4) {
				continue;
			}
			iSegmentStop = 4;
		}
		else {
			memcpy(shelf[0], result, sizeof(uint8_t) * PACKET_SIZE*PACKETS_PER_FRAME);
			iSegmentStop = 1;
		}

		if ((autoRangeMin == true) || (autoRangeMax == true)) {
			if (autoRangeMin == true) {
				maxValue = 65535;
			}
			if (autoRangeMax == true) {
				maxValue = 0;
			}
			for(int iSegment = iSegmentStart; iSegment <= iSegmentStop; iSegment++) {
				for(int i=0;i<FRAME_SIZE_UINT16;i++) {
					//skip the first 2 uint16_t's of every packet, they're 4 header bytes
					if(i % PACKET_SIZE_UINT16 < 2) {
						continue;
					}

					//flip the MSB and LSB at the last second
					uint16_t value = (shelf[iSegment - 1][i*2] << 8) + shelf[iSegment - 1][i*2+1];
					if (value == 0) {
						// Why this value is 0?
						continue;
					}
					if ((autoRangeMax == true) && (value > maxValue)) {
						maxValue = value;
					}
					if ((autoRangeMin == true) && (value < minValue)) {
						minValue = value;
					}
				}
			}
			diff = maxValue - minValue;
			scale = 3000/diff;
		}

		// QImage 제거: 컬러맵에서 RGB를 가져와 바로 YUYV422로 변환
		// YUYV422는 2 pixels per 4 bytes (Y0 U Y1 V)
		uchar* yuyvPtr = vidsendbuf;
		has_prev_pixel = false;
		frameValid = false;  // 프레임 유효성 초기화
		
		int row, column;
		uint16_t value;
		uint16_t valueFrameBuffer;
		uint8_t r, g, b;
		int pixelsProcessed = 0;  // 처리된 픽셀 수 추적
		for(int iSegment = iSegmentStart; iSegment <= iSegmentStop; iSegment++) {
			int ofsRow = 30 * (iSegment - 1);
			for(int i=0;i<FRAME_SIZE_UINT16;i++) {
				//skip the first 2 uint16_t's of every packet, they're 4 header bytes
				if(i % PACKET_SIZE_UINT16 < 2) {
					continue;
				}

				//flip the MSB and LSB at the last second
				valueFrameBuffer = (shelf[iSegment - 1][i*2] << 8) + shelf[iSegment - 1][i*2+1];
				if (valueFrameBuffer == 0) {
					// Why this value is 0?
					n_zero_value_drop_frame++;
					if ((n_zero_value_drop_frame % 12) == 0) {
						log_message(5, "[WARNING] Found zero-value. Drop the frame continuously " + std::to_string(n_zero_value_drop_frame) + " times");
					}
					// 제로 값을 만나면 프레임 불완전으로 표시하고 중단
					frameIncomplete = true;
					break;
				}
				pixelsProcessed++;
				//##############################
				//온도 데이터인 valueFrameBuffer를 가지고 컬러팔레트에 매핑을 하는 부분입니다.
				// scale을 곱해서 min ~ max 범위에 대한 온도만 컬러맵에 매핑
				value = (valueFrameBuffer-minValue)*scale;
				
				// 컬러맵 인덱스 범위 체크 최적화 (std::min 사용)
				int base_ofs = 3 * value;
				int ofs_r = (base_ofs + 0 < colormapSize) ? base_ofs + 0 : colormapSize - 1;
				int ofs_g = (base_ofs + 1 < colormapSize) ? base_ofs + 1 : colormapSize - 1;
				int ofs_b = (base_ofs + 2 < colormapSize) ? base_ofs + 2 : colormapSize - 1;
				r = colormap[ofs_r];
				g = colormap[ofs_g];
				b = colormap[ofs_b];
				
				if (typeLepton == 3) {
					column = (i % PACKET_SIZE_UINT16) - 2 + (myImageWidth / 2) * ((i % (PACKET_SIZE_UINT16 * 2)) / PACKET_SIZE_UINT16);
					row = i / PACKET_SIZE_UINT16 / 2 + ofsRow;
				}
				else {
					column = (i % PACKET_SIZE_UINT16) - 2;
					row = i / PACKET_SIZE_UINT16;
				}
				
				// RGB → YUYV422 직접 변환 (2 pixels per 4 bytes)
				if (!has_prev_pixel) {
					// 첫 번째 픽셀 저장
					prev_r = r;
					prev_g = g;
					prev_b = b;
					has_prev_pixel = true;
				} else {
					// 두 번째 픽셀과 함께 YUYV422 변환
					// YUV 변환 (ITU-R BT.601, 정수 연산)
					int y1 = ((77 * prev_r + 150 * prev_g + 29 * prev_b) >> 8);
					int y2 = ((77 * r + 150 * g + 29 * b) >> 8);
					
					// U, V는 두 픽셀의 평균 사용
					int r_avg = (prev_r + r) >> 1;
					int g_avg = (prev_g + g) >> 1;
					int b_avg = (prev_b + b) >> 1;
					int u = ((-43 * r_avg - 85 * g_avg + 128 * b_avg) >> 8) + 128;
					int v = ((128 * r_avg - 107 * g_avg - 21 * b_avg) >> 8) + 128;
					
					// 클램핑 (0-255) - 컴파일러 최적화 활용
					// O3 최적화에서 삼항 연산자가 효율적으로 처리됨
					y1 = (y1 < 0) ? 0 : (y1 > 255) ? 255 : y1;
					y2 = (y2 < 0) ? 0 : (y2 > 255) ? 255 : y2;
					u = (u < 0) ? 0 : (u > 255) ? 255 : u;
					v = (v < 0) ? 0 : (v > 255) ? 255 : v;
					
					// YUYV422 포맷: Y0 U Y1 V
					*yuyvPtr++ = (uchar)y1;
					*yuyvPtr++ = (uchar)u;
					*yuyvPtr++ = (uchar)y2;
					*yuyvPtr++ = (uchar)v;
					
					has_prev_pixel = false;
				}
				//###############################

				//픽셀별 rawTemperature 데이터 출력 메서드 (단위 centiKelvin)
				//printRawThermalData(column,row,valueFrameBuffer); 
				
			}
		}
		
		// 홀수 픽셀 처리 (마지막 픽셀이 홀수인 경우)
		if (has_prev_pixel) {
			// 마지막 픽셀을 복제하여 처리
			int y1 = ((77 * prev_r + 150 * prev_g + 29 * prev_b) >> 8);
			int y2 = y1; // 같은 픽셀 복제
			int u = ((-43 * prev_r - 85 * prev_g + 128 * prev_b) >> 8) + 128;
			int v = ((128 * prev_r - 107 * prev_g - 21 * prev_b) >> 8) + 128;
			
			y1 = (y1 < 0) ? 0 : (y1 > 255) ? 255 : y1;
			y2 = (y2 < 0) ? 0 : (y2 > 255) ? 255 : y2;
			u = (u < 0) ? 0 : (u > 255) ? 255 : u;
			v = (v < 0) ? 0 : (v > 255) ? 255 : v;
			
			*yuyvPtr++ = (uchar)y1;
			*yuyvPtr++ = (uchar)u;
			*yuyvPtr++ = (uchar)y2;
			*yuyvPtr++ = (uchar)v;
		}
		//각 프레임에 적용된 min, max, diff, scale 값 디버깅 ( min/max 미지정시 auto모드로 인해 매번 변함 )
		//printf("minValue : %d , maxValue : %d , diff : %f , scale : %f\n", minValue, maxValue, diff, scale);

		// 프레임 완전성 검증: 예상 픽셀 수와 실제 처리된 픽셀 수 비교
		int expectedPixels = myImageWidth * myImageHeight;
		if (frameIncomplete || pixelsProcessed < expectedPixels * 0.9) {  // 90% 미만이면 프레임 불완전
			if (prev_vidsendbuf != nullptr) {
				// 이전 프레임 재사용
				memcpy(vidsendbuf, prev_vidsendbuf, myImageWidth * myImageHeight * 2);
				log_message(3, "[WARNING] Incomplete frame (" + std::to_string(pixelsProcessed) + "/" + std::to_string(expectedPixels) + " pixels), reusing previous frame");
			} else {
				// 첫 프레임이거나 이전 프레임이 없으면 버퍼를 검은색으로 초기화
				memset(vidsendbuf, 0, myImageWidth * myImageHeight * 2);
				log_message(3, "[WARNING] Incomplete frame, no previous frame available");
			}
		} else {
			// 유효한 프레임이면 이전 프레임 버퍼에 복사
			if (prev_vidsendbuf == nullptr) {
				// 첫 유효 프레임이면 이전 프레임 버퍼 할당
				prev_vidsendbuf = (uchar*)malloc(myImageWidth * myImageHeight * 2);
			}
			memcpy(prev_vidsendbuf, vidsendbuf, myImageWidth * myImageHeight * 2);
			frameValid = true;
		}

		if (n_zero_value_drop_frame != 0) {
			log_message(8, "[WARNING] Found zero-value. Drop the frame continuously " + std::to_string(n_zero_value_drop_frame) + " times [RECOVERED]");
			n_zero_value_drop_frame = 0;
		}

		updateVpipe();
	}
	
	//finally, close SPI port just bcuz
	SpiClosePort(0);
}

void LeptonThread::performFFC() {
	//perform FFC
	lepton_perform_ffc();
}

void LeptonThread::printRawThermalData(int col, int row, uint16_t val){
	float celcius = (float)((val - 27315) / 100.0);
	//celcius로 출력
	printf("(%d, %d) = %.f \n",row,col, celcius);
	//centiKelvin으로 출력
	// printf("(%d, %d) = %u \n",row,col, val);
}

void LeptonThread::log_message(uint16_t level, std::string msg)
{
	if (level <= loglevel) {
		std::cerr << msg << std::endl;
	}
}

void LeptonThread::updateVpipe()
{
	// QImage 제거: run()에서 이미 YUYV422로 변환 완료
	// 버퍼를 직접 v4l2에 출력
	int yuyvSize = myImageWidth * myImageHeight * 2; // YUYV422는 2 bytes/pixel
	write(v4l2sink, vidsendbuf, yuyvSize);
}

void LeptonThread::open_vpipe() {
    int vidsendsiz;

    v4l2sink = open("/dev/video12", O_WRONLY);
    if (v4l2sink < 0) {
        fprintf(stderr, "Failed to open v4l2sink device. (%s)\n", strerror(errno));
        exit(-2);
    }

    struct v4l2_format v;
    memset(&v, 0, sizeof(v));

    v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    v.fmt.pix.width = 160;
    v.fmt.pix.height = 120;
    v.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; // YUYV422 포맷으로 변경
    // YUYV422는 2 bytes per pixel (160 * 120 * 2 = 38,400 bytes)
    // 기존 RGB24는 3 bytes per pixel이었음 (메모리 33% 절약)
    vidsendsiz = 160 * 120 * 2;
    vidsendbuf = (uchar*)malloc(vidsendsiz);

    v.fmt.pix.sizeimage = vidsendsiz;
    if (ioctl(v4l2sink, VIDIOC_S_FMT, &v) < 0) {
        fprintf(stderr, "Failed to set format on v4l2sink. (%s)\n", strerror(errno));
        exit(-1);
    }

}




