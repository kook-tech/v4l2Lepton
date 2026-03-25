#include <iostream>

#include "LeptonThread.h"
#include <linux/videodev2.h>

#include "Palettes.h"
#include "SPI.h"
#include "Lepton_I2C.h"
//
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <sys/mman.h>
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
	v4l2sink = -1;
	for (int i = 0; i < V4L2_NBUF; i++) v4l2_bufs_[i] = nullptr;
	v4l2_buf_len_ = 0;
	v4l2_queued_ = 0;
	use_v4l2_input_ = false;
	v4l2src_fd_ = -1;
	for (int i = 0; i < V4L2_SRC_NBUF; i++) v4l2src_bufs_[i] = nullptr;
	v4l2src_buf_len_ = 0;
	v4l2src_nbuf_ = 0;
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
	// Default to FIXED scaling range (do not auto-change min/max at runtime).
	// Auto-range can be enabled explicitly via LeptonThread::setAutomaticScalingRange().
	autoRangeMin = false;
	autoRangeMax = false;
	rangeMin = 30000;
	rangeMax = 32000;
	open_vpipe();
	loadCaptureStreamFxEnv();
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
		enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		ioctl(v4l2sink, VIDIOC_STREAMOFF, &t);
		for (int i = 0; i < V4L2_NBUF && v4l2_bufs_[i]; i++) {
			munmap(v4l2_bufs_[i], v4l2_buf_len_);
			v4l2_bufs_[i] = nullptr;
		}
		close(v4l2sink);
		v4l2sink = -1;
	}
	if (v4l2src_fd_ >= 0) {
		enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		ioctl(v4l2src_fd_, VIDIOC_STREAMOFF, &t);
		for (int i = 0; i < V4L2_SRC_NBUF && v4l2src_bufs_[i]; i++) {
			munmap(v4l2src_bufs_[i], v4l2src_buf_len_);
			v4l2src_bufs_[i] = nullptr;
		}
		v4l2src_nbuf_ = 0;
		close(v4l2src_fd_);
		v4l2src_fd_ = -1;
	}
}

void LeptonThread::useV4l2Input(const char* device) {
	if (device && device[0]) {
		use_v4l2_input_ = true;
		v4l2_device_ = device;
	}
}

void LeptonThread::stop() {
	// Make shutdown responsive:
	// - flip flag so run() exits
	// - wake any capture waiters
	// - close FDs to unblock potential blocking I/O (SPI read, v4l2 write)
	shouldStop = true;

	// Unblock captureNextValidFrame waiters immediately.
	capture_requested_.store(false, std::memory_order_relaxed);
	{
		QMutexLocker locker(&capture_mutex_);
		capture_cv_.wakeAll();
	}

	if (v4l2sink >= 0) {
		enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		ioctl(v4l2sink, VIDIOC_STREAMOFF, &t);
		for (int i = 0; i < V4L2_NBUF && v4l2_bufs_[i]; i++) {
			munmap(v4l2_bufs_[i], v4l2_buf_len_);
			v4l2_bufs_[i] = nullptr;
		}
		::close(v4l2sink);
		v4l2sink = -1;
	}

	// Close SPI fd to unblock read().
	// NOTE: spi_cs0_fd is a global defined in SPI.cpp.
	if (spi_cs0_fd >= 0) {
		::close(spi_cs0_fd);
		spi_cs0_fd = -1;
	}

	// Close V4L2 capture fd (stream off + munmap first).
	if (v4l2src_fd_ >= 0) {
		enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		ioctl(v4l2src_fd_, VIDIOC_STREAMOFF, &t);
		for (int i = 0; i < V4L2_SRC_NBUF && v4l2src_bufs_[i]; i++) {
			munmap(v4l2src_bufs_[i], v4l2src_buf_len_);
			v4l2src_bufs_[i] = nullptr;
		}
		v4l2src_nbuf_ = 0;
		::close(v4l2src_fd_);
		v4l2src_fd_ = -1;
	}
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

	if (use_v4l2_input_) {
		// pure_thermal 등 V4L2 장치를 Y16 모드로 캡처
		// 일부 UVC 드라이버는 ioctl S_FMT만으로 Y16 전환이 안 되므로, v4l2-ctl로 선설정 시도
		char v4l2ctl_cmd[256];
		snprintf(v4l2ctl_cmd, sizeof(v4l2ctl_cmd),
		         "v4l2-ctl -d %s --set-fmt-video=width=160,height=120,pixelformat=Y16 2>/dev/null",
		         v4l2_device_.c_str());
		if (system(v4l2ctl_cmd) != 0) {
			fprintf(stderr, "[raspberrypi_video] v4l2-ctl pre-set (optional) failed, trying ioctl...\n");
		}
		v4l2src_fd_ = open(v4l2_device_.c_str(), O_RDWR);  // blocking: pure_thermal 9fps에 read() 호환
		if (v4l2src_fd_ < 0) {
			fprintf(stderr, "Failed to open V4L2 capture device %s: %s\n", v4l2_device_.c_str(), strerror(errno));
			return;
		}
		struct v4l2_format fmt;
		memset(&fmt, 0, sizeof(fmt));
		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		fmt.fmt.pix.width = 160;
		fmt.fmt.pix.height = 120;
		fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_Y16;
		if (ioctl(v4l2src_fd_, VIDIOC_S_FMT, &fmt) < 0) {
			fprintf(stderr, "Failed to set Y16 format on %s: %s\n", v4l2_device_.c_str(), strerror(errno));
			close(v4l2src_fd_);
			v4l2src_fd_ = -1;
			return;
		}
		// S_FMT 성공 후 실제 적용된 포맷 검증 (드라이버가 UYVY 등으로 되돌릴 수 있음)
		if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_Y16) {
			uint32_t pf = fmt.fmt.pix.pixelformat;
			fprintf(stderr, "[raspberrypi_video] WARNING: %s format is not Y16 (got 0x%08x '%c%c%c%c'). "
			        "Run before start: v4l2-ctl -d %s --set-fmt-video=width=160,height=120,pixelformat=Y16\n",
			        v4l2_device_.c_str(), pf,
			        (char)(pf&0xff), (char)((pf>>8)&0xff), (char)((pf>>16)&0xff), (char)((pf>>24)&0xff),
			        v4l2_device_.c_str());
			close(v4l2src_fd_);
			v4l2src_fd_ = -1;
			return;
		}
		// pure_thermal은 160x120 고정
		myImageWidth = 160;
		myImageHeight = 120;
		typeLepton = 3;
		fprintf(stderr, "[raspberrypi_video] V4L2 Y16 capture from %s (160x120)\n", v4l2_device_.c_str());

		// read() 미지원 드라이버용: mmap 스트리밍으로 캡처
		struct v4l2_requestbuffers req;
		memset(&req, 0, sizeof(req));
		req.count = V4L2_SRC_NBUF;
		req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		req.memory = V4L2_MEMORY_MMAP;
		if (ioctl(v4l2src_fd_, VIDIOC_REQBUFS, &req) < 0) {
			fprintf(stderr, "[raspberrypi_video] VIDIOC_REQBUFS capture failed: %s\n", strerror(errno));
			close(v4l2src_fd_);
			v4l2src_fd_ = -1;
			return;
		}
		v4l2src_nbuf_ = static_cast<int>(req.count);
		if (v4l2src_nbuf_ <= 0 || v4l2src_nbuf_ > V4L2_SRC_NBUF) {
			fprintf(stderr, "[raspberrypi_video] no capture buffers\n");
			close(v4l2src_fd_);
			v4l2src_fd_ = -1;
			return;
		}
		for (int i = 0; i < v4l2src_nbuf_; i++) {
			struct v4l2_buffer buf;
			memset(&buf, 0, sizeof(buf));
			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;
			buf.index = i;
			if (ioctl(v4l2src_fd_, VIDIOC_QUERYBUF, &buf) < 0) {
				fprintf(stderr, "[raspberrypi_video] VIDIOC_QUERYBUF capture failed: %s\n", strerror(errno));
				for (int j = 0; j < i; j++) { munmap(v4l2src_bufs_[j], v4l2src_buf_len_); v4l2src_bufs_[j] = nullptr; }
				close(v4l2src_fd_);
				v4l2src_fd_ = -1;
				return;
			}
			v4l2src_buf_len_ = buf.length;
			v4l2src_bufs_[i] = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, v4l2src_fd_, buf.m.offset);
			if (v4l2src_bufs_[i] == MAP_FAILED) {
				fprintf(stderr, "[raspberrypi_video] capture mmap failed: %s\n", strerror(errno));
				for (int j = 0; j < i; j++) { munmap(v4l2src_bufs_[j], v4l2src_buf_len_); v4l2src_bufs_[j] = nullptr; }
				close(v4l2src_fd_);
				v4l2src_fd_ = -1;
				return;
			}
		}
		for (int i = 0; i < v4l2src_nbuf_; i++) {
			struct v4l2_buffer buf;
			memset(&buf, 0, sizeof(buf));
			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;
			buf.index = i;
			if (ioctl(v4l2src_fd_, VIDIOC_QBUF, &buf) < 0) {
				fprintf(stderr, "[raspberrypi_video] VIDIOC_QBUF capture failed: %s\n", strerror(errno));
				for (int j = 0; j < v4l2src_nbuf_; j++) { munmap(v4l2src_bufs_[j], v4l2src_buf_len_); v4l2src_bufs_[j] = nullptr; }
				close(v4l2src_fd_);
				v4l2src_fd_ = -1;
				return;
			}
		}
		enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (ioctl(v4l2src_fd_, VIDIOC_STREAMON, &cap_type) < 0) {
			fprintf(stderr, "[raspberrypi_video] VIDIOC_STREAMON capture failed: %s\n", strerror(errno));
			for (int i = 0; i < v4l2src_nbuf_; i++) { munmap(v4l2src_bufs_[i], v4l2src_buf_len_); v4l2src_bufs_[i] = nullptr; }
			close(v4l2src_fd_);
			v4l2src_fd_ = -1;
			return;
		}
		fprintf(stderr, "[raspberrypi_video] V4L2 capture mmap streaming on (%d buffers)\n", v4l2src_nbuf_);
	} else {
		// SPI 직접 연결
		SpiOpenPort(0, spiSpeed);
	}

	// Reusable work buffer for raw16 (centiKelvin) snapshot.
	// We only commit this buffer to latest_raw_u16_ if the frame is valid.
	std::vector<uint16_t> work_raw_u16;
	work_raw_u16.resize(myImageWidth * myImageHeight);

	if (use_v4l2_input_) {
		// === V4L2 Y16 캡처 루프 (pure_thermal) ===
		fprintf(stderr, "[V4L2] colormap=%s size=%d min=%u max=%u scale=%.3f\n",
		        (typeColormap == 4) ? "custom" : (typeColormap == 3) ? "ironblack" : "other",
		        colormapSize, minValue, maxValue, scale);
		const size_t frame_bytes = static_cast<size_t>(myImageWidth) * static_cast<size_t>(myImageHeight) * 2;
		std::vector<uint8_t> y16_buf(frame_bytes);
		int frame_count = 0;
		while (!shouldStop) {
			const bool capture_this_frame = capture_requested_.load(std::memory_order_relaxed);
			if (capture_this_frame) {
				const size_t expected_raw_size = static_cast<size_t>(myImageWidth) * static_cast<size_t>(myImageHeight);
				if (work_raw_u16.size() != expected_raw_size) work_raw_u16.resize(expected_raw_size);
				std::fill(work_raw_u16.begin(), work_raw_u16.end(), 0);
			}

			// DQBUF: mmap 스트리밍에서 채워진 버퍼 하나 받기 (블로킹)
			struct v4l2_buffer buf;
			memset(&buf, 0, sizeof(buf));
			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;
			if (ioctl(v4l2src_fd_, VIDIOC_DQBUF, &buf) < 0) {
				if (shouldStop) break;
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					usleep(5000);
				} else {
					fprintf(stderr, "[V4L2] DQBUF failed: %s\n", strerror(errno));
					usleep(50000);
				}
				continue;
			}
			size_t bytesused = static_cast<size_t>(buf.bytesused);
			if (bytesused < frame_bytes || buf.index >= static_cast<unsigned>(v4l2src_nbuf_)) {
				(void)ioctl(v4l2src_fd_, VIDIOC_QBUF, &buf);
				continue;
			}
			memcpy(y16_buf.data(), v4l2src_bufs_[buf.index], frame_bytes);
			if (ioctl(v4l2src_fd_, VIDIOC_QBUF, &buf) < 0) {
				fprintf(stderr, "[V4L2] QBUF failed: %s\n", strerror(errno));
			}
			if (frame_count == 0) fprintf(stderr, "[V4L2] first frame ok, %zu bytes\n", frame_bytes);

			// 디버깅: raw 프레임 min/max (centiKelvin → Celsius)
			uint16_t raw_min = 65535, raw_max = 0;
			for (size_t i = 0; i < work_raw_u16.size(); i++) {
				uint16_t v = y16_buf[i*2] | (static_cast<uint16_t>(y16_buf[i*2+1]) << 8);
				if (v < raw_min) raw_min = v;
				if (v > raw_max) raw_max = v;
			}

			// auto-range
			if (autoRangeMin || autoRangeMax) {
				minValue = autoRangeMin ? 65535u : minValue;
				maxValue = autoRangeMax ? 0u : maxValue;
				for (size_t i = 0; i < work_raw_u16.size(); i++) {
					uint16_t v = y16_buf[i*2] | (static_cast<uint16_t>(y16_buf[i*2+1]) << 8);
					if (v == 0) continue;
					if (autoRangeMax && v > maxValue) maxValue = v;
					if (autoRangeMin && v < minValue) minValue = v;
				}
				diff = maxValue - minValue;
				scale = (diff > 0.0f) ? (3000.0f / diff) : 1.0f;
			}

			uchar* yuyvPtr = vidsendbuf;
			has_prev_pixel = false;
			frameValid = false;
			int pixelsProcessed = 0;
			for (int row = 0; row < myImageHeight; row++) {
				for (int col = 0; col < myImageWidth; col++) {
					size_t idx = static_cast<size_t>(row) * static_cast<size_t>(myImageWidth) + static_cast<size_t>(col);
					uint16_t valueFrameBuffer = y16_buf[idx*2] | (static_cast<uint16_t>(y16_buf[idx*2+1]) << 8);
					uint8_t r, g, b;
					if (valueFrameBuffer == 0) {
						// 센서가 0을 반환하는 경우는 "invalid" 의미로 사용되므로 그대로 검정으로 출력한다.
						r = g = b = 0;
					} else if (valueFrameBuffer <= minValue) {
						// min 이하(예: 0°C 및 그 이하)는 항상 화이트로 표현
						r = g = b = 255;
					} else if (valueFrameBuffer >= maxValue) {
						// max 이상(예: 50°C 및 그 초과)은 항상 블랙으로 표현
						r = g = b = 0;
					} else {
						// 정상 범위(min < value < max)는 3000 step under-mapping으로 팔레트에 매핑
						pixelsProcessed++;
						const float diff_local = static_cast<float>(maxValue - minValue);
						const float scale_local = (diff_local > 0.0f) ? (3000.0f / diff_local) : 1.0f;
						float vf = (static_cast<float>(valueFrameBuffer) - static_cast<float>(minValue)) * scale_local;
						if (vf < 0.0f) vf = 0.0f;
						if (vf > 2999.0f) vf = 2999.0f;  // 정상 구간 3000 step (0..2999)
						const int value = static_cast<int>(vf);  // under-mapping
						const int base_ofs = 3 * value;
						const int ofs_r = (base_ofs + 0 < colormapSize) ? base_ofs + 0 : colormapSize - 1;
						const int ofs_g = (base_ofs + 1 < colormapSize) ? base_ofs + 1 : colormapSize - 1;
						const int ofs_b = (base_ofs + 2 < colormapSize) ? base_ofs + 2 : colormapSize - 1;
						r = static_cast<uint8_t>(colormap[ofs_r]);
						g = static_cast<uint8_t>(colormap[ofs_g]);
						b = static_cast<uint8_t>(colormap[ofs_b]);
					}
					if (capture_this_frame) work_raw_u16[idx] = valueFrameBuffer;
					if (!has_prev_pixel) { prev_r = r; prev_g = g; prev_b = b; has_prev_pixel = true; }
					else {
						int y1 = ((77*prev_r + 150*prev_g + 29*prev_b) >> 8);
						int y2 = ((77*r + 150*g + 29*b) >> 8);
						int r_avg = (prev_r + r) >> 1, g_avg = (prev_g + g) >> 1, b_avg = (prev_b + b) >> 1;
						int u = ((-43*r_avg - 85*g_avg + 128*b_avg) >> 8) + 128;
						int v = ((128*r_avg - 107*g_avg - 21*b_avg) >> 8) + 128;
						y1 = (y1<0)?0:(y1>255)?255:y1; y2 = (y2<0)?0:(y2>255)?255:y2;
						u = (u<0)?0:(u>255)?255:u; v = (v<0)?0:(v>255)?255:v;
						*yuyvPtr++ = (uchar)y1; *yuyvPtr++ = (uchar)u; *yuyvPtr++ = (uchar)y2; *yuyvPtr++ = (uchar)v;
						has_prev_pixel = false;
					}
				}
			}
			if (has_prev_pixel) {
				int y1 = ((77*prev_r + 150*prev_g + 29*prev_b) >> 8);
				int u = ((-43*prev_r - 85*prev_g + 128*prev_b) >> 8) + 128;
				int v = ((128*prev_r - 107*prev_g - 21*prev_b) >> 8) + 128;
				y1 = (y1<0)?0:(y1>255)?255:y1; u = (u<0)?0:(u>255)?255:u; v = (v<0)?0:(v>255)?255:v;
				*yuyvPtr++ = (uchar)y1; *yuyvPtr++ = (uchar)u; *yuyvPtr++ = (uchar)y1; *yuyvPtr++ = (uchar)v;
			}
			int expectedPixels = myImageWidth * myImageHeight;
			bool frameIncomplete = (pixelsProcessed < expectedPixels * 9 / 10);
			if (++frame_count <= 3 || (frame_count % 90) == 0) {
				float min_c = (raw_min <= 65534) ? (raw_min / 100.f - 273.15f) : 0.f;
				float max_c = (raw_max > 0) ? (raw_max / 100.f - 273.15f) : 0.f;
				fprintf(stderr, "[V4L2] frame %d raw min=%u (%.1f°C) max=%u (%.1f°C) px=%d/%d\n",
				        frame_count, raw_min, min_c, raw_max, max_c, pixelsProcessed, expectedPixels);
			}
			if (frameIncomplete) {
				if (prev_vidsendbuf) memcpy(vidsendbuf, prev_vidsendbuf, myImageWidth * myImageHeight * 2);
				else memset(vidsendbuf, 0, myImageWidth * myImageHeight * 2);
			} else {
				if (!prev_vidsendbuf) prev_vidsendbuf = (uchar*)malloc(myImageWidth * myImageHeight * 2);
				if (prev_vidsendbuf) memcpy(prev_vidsendbuf, vidsendbuf, myImageWidth * myImageHeight * 2);
				frameValid = true;
			}
			uint64_t now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
			{ QMutexLocker locker(&frame_stats_mutex_);
				last_frame_ts_ms_ = now_ms; last_frame_valid_ = frameValid; last_frame_incomplete_ = frameIncomplete;
				last_pixels_processed_ = pixelsProcessed; last_expected_pixels_ = expectedPixels;
				last_spi_resets_ = 0; last_segment_number_ = -1; last_wrong_segment_streak_ = 0; last_zero_value_drop_streak_ = 0;
			}
			if (capture_this_frame && frameValid) {
				{ QMutexLocker locker(&latest_mutex_);
					latest_ts_ms_ = now_ms; latest_valid_ = frameValid; latest_range_min_ck_ = minValue; latest_range_max_ck_ = maxValue; latest_scale_ = scale;
					if (latest_yuyv_.size() != static_cast<size_t>(myImageWidth)*myImageHeight*2) latest_yuyv_.resize(myImageWidth*myImageHeight*2);
					memcpy(latest_yuyv_.data(), vidsendbuf, myImageWidth*myImageHeight*2);
					latest_raw_u16_ = work_raw_u16;
				}
				{ QMutexLocker locker(&capture_mutex_); capture_seq_++; capture_requested_.store(false, std::memory_order_relaxed); capture_cv_.wakeAll(); }
			}
			updateVpipe();
		}
		if (v4l2src_fd_ >= 0) {
			enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			ioctl(v4l2src_fd_, VIDIOC_STREAMOFF, &t);
			for (int i = 0; i < V4L2_SRC_NBUF && v4l2src_bufs_[i]; i++) {
				munmap(v4l2src_bufs_[i], v4l2src_buf_len_);
				v4l2src_bufs_[i] = nullptr;
			}
			v4l2src_nbuf_ = 0;
			::close(v4l2src_fd_);
			v4l2src_fd_ = -1;
		}
		return;
	}

	// === SPI 캡처 루프 ===
	while(!shouldStop) {
		// Capture is on-demand: only build raw snapshot buffers when a capture is requested.
		const bool capture_this_frame = capture_requested_.load(std::memory_order_relaxed);

		// Ensure work buffer matches current dimensions (in case Lepton type changed).
		if (capture_this_frame) {
			const size_t expected_raw_size = static_cast<size_t>(myImageWidth) * static_cast<size_t>(myImageHeight);
			if (work_raw_u16.size() != expected_raw_size) {
				work_raw_u16.assign(expected_raw_size, 0);
			}
			// Clear for this frame to avoid leaking stale pixels if any unexpected index holes occur.
			std::fill(work_raw_u16.begin(), work_raw_u16.end(), 0);
		}

		//read data packets from lepton over SPI
		int resets = 0;
		int segmentNumber = -1;
		bool frameIncomplete = false;  // 프레임 불완전 플래그 (루프 시작 시 초기화)
		for(int j=0;j<PACKETS_PER_FRAME;j++) {
			if (shouldStop) {
				frameIncomplete = true;
				break;
			}
			//if it's a drop packet, reset j to 0, set to -1 so he'll be at 0 again loop
			const ssize_t nread = ::read(
				spi_cs0_fd,
				result + sizeof(uint8_t) * PACKET_SIZE * j,
				sizeof(uint8_t) * PACKET_SIZE
			);
			if (nread != static_cast<ssize_t>(sizeof(uint8_t) * PACKET_SIZE)) {
				if (shouldStop) {
					frameIncomplete = true;
					break;
				}
				// SPI hiccup or fd closed/reopened; try to resync.
				j = -1;
				resets += 1;
				usleep(2000);
				continue;
			}
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
					// Close/reopen SPI (avoid SpiClosePort() because it exits on error).
					if (spi_cs0_fd >= 0) {
						::close(spi_cs0_fd);
						spi_cs0_fd = -1;
					}
					if (shouldStop) {
						frameIncomplete = true;
						break;
					}
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
		if (shouldStop) {
			break;
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
				// Initialize minValue high so we can find the true frame minimum.
				minValue = 65535;
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
			// Guard against divide-by-zero (e.g., flat frame or min/max not updated)
			if (diff > 0.0f) {
				scale = 3000 / diff;
			} else {
				scale = 1.0f;
			}
		}

		// QImage 제거: 컬러맵에서 RGB를 가져와 바로 YUYV422로 변환
		// YUYV422는 2 pixels per 4 bytes (Y0 U Y1 V)
		uchar* yuyvPtr = vidsendbuf;
		has_prev_pixel = false;
		frameValid = false;  // 프레임 유효성 초기화
		
		int row = 0, column = 0;
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
				
				if (capture_this_frame) {
					if (typeLepton == 3) {
						column = (i % PACKET_SIZE_UINT16) - 2 + (myImageWidth / 2) * ((i % (PACKET_SIZE_UINT16 * 2)) / PACKET_SIZE_UINT16);
						row = i / PACKET_SIZE_UINT16 / 2 + ofsRow;
					}
					else {
						column = (i % PACKET_SIZE_UINT16) - 2;
						row = i / PACKET_SIZE_UINT16;
					}

					// Cache raw16 (centiKelvin) into a stable 2D layout for snapshot saving.
					// Bounds-check protects against any unexpected index math.
					if (0 <= row && row < myImageHeight && 0 <= column && column < myImageWidth) {
						work_raw_u16[static_cast<size_t>(row) * static_cast<size_t>(myImageWidth) + static_cast<size_t>(column)] = valueFrameBuffer;
					}
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

		// Update lightweight per-frame status for diagnostics (no buffers copied).
		{
			const uint64_t now_ms =
			    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			                              std::chrono::system_clock::now().time_since_epoch())
			                              .count());
			QMutexLocker locker(&frame_stats_mutex_);
			last_frame_ts_ms_ = now_ms;
			last_frame_valid_ = frameValid;
			last_frame_incomplete_ = frameIncomplete || (pixelsProcessed < expectedPixels * 0.9);
			last_pixels_processed_ = pixelsProcessed;
			last_expected_pixels_ = expectedPixels;
			last_spi_resets_ = resets;
			last_segment_number_ = segmentNumber;
			last_wrong_segment_streak_ = static_cast<int>(n_wrong_segment);
			last_zero_value_drop_streak_ = static_cast<int>(n_zero_value_drop_frame);
		}

		if (n_zero_value_drop_frame != 0) {
			log_message(8, "[WARNING] Found zero-value. Drop the frame continuously " + std::to_string(n_zero_value_drop_frame) + " times [RECOVERED]");
			n_zero_value_drop_frame = 0;
		}

		// On-demand snapshot: if a capture is requested, commit one valid frame and wake waiters.
		if (capture_this_frame && frameValid) {
			const uint64_t now_ms =
			    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			                              std::chrono::system_clock::now().time_since_epoch())
			                              .count());

			{
				QMutexLocker locker(&latest_mutex_);
				latest_ts_ms_ = now_ms;
				latest_valid_ = frameValid;
				latest_range_min_ck_ = minValue;
				latest_range_max_ck_ = maxValue;
				latest_scale_ = scale;

				const size_t yuyv_size = static_cast<size_t>(myImageWidth) * static_cast<size_t>(myImageHeight) * 2;
				if (latest_yuyv_.size() != yuyv_size) {
					latest_yuyv_.resize(yuyv_size);
				}
				memcpy(latest_yuyv_.data(), vidsendbuf, yuyv_size);
				latest_raw_u16_ = work_raw_u16;  // copy only when capturing
			}

			{
				QMutexLocker locker(&capture_mutex_);
				capture_seq_++;
				capture_requested_.store(false, std::memory_order_relaxed);
				capture_cv_.wakeAll();
			}
		}

		updateVpipe();
	}
	
	// finally, close SPI port (avoid SpiClosePort() because it exits on error)
	if (spi_cs0_fd >= 0) {
		::close(spi_cs0_fd);
		spi_cs0_fd = -1;
	}
}

bool LeptonThread::snapshotLatestFrame(
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
    int& out_type_lepton) const
{
	QMutexLocker locker(&latest_mutex_);
	if (latest_ts_ms_ == 0 || latest_yuyv_.empty()) {
		return false;
	}

	out_width = myImageWidth;
	out_height = myImageHeight;
	out_frame_ts_ms = latest_ts_ms_;
	out_frame_valid = latest_valid_;
	out_range_min_ck = latest_range_min_ck_;
	out_range_max_ck = latest_range_max_ck_;
	out_scale = latest_scale_;
	out_type_colormap = typeColormap;
	out_type_lepton = typeLepton;

	out_yuyv = latest_yuyv_;
	out_raw_u16 = latest_raw_u16_;
	return true;
}

bool LeptonThread::getLastFrameStatus(
    uint64_t& out_ts_ms,
    bool& out_valid,
    bool& out_frame_incomplete,
    int& out_pixels_processed,
    int& out_expected_pixels,
    int& out_spi_resets,
    int& out_segment_number,
    int& out_wrong_segment_streak,
    int& out_zero_value_drop_streak) const
{
	QMutexLocker locker(&frame_stats_mutex_);
	if (last_frame_ts_ms_ == 0) {
		return false;
	}
	out_ts_ms = last_frame_ts_ms_;
	out_valid = last_frame_valid_;
	out_frame_incomplete = last_frame_incomplete_;
	out_pixels_processed = last_pixels_processed_;
	out_expected_pixels = last_expected_pixels_;
	out_spi_resets = last_spi_resets_;
	out_segment_number = last_segment_number_;
	out_wrong_segment_streak = last_wrong_segment_streak_;
	out_zero_value_drop_streak = last_zero_value_drop_streak_;
	return true;
}

bool LeptonThread::captureNextValidFrame(
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
    int timeout_ms)
{
	// Request capture
	uint64_t start_seq = 0;
	{
		QMutexLocker locker(&capture_mutex_);
		start_seq = capture_seq_;
		capture_requested_.store(true, std::memory_order_relaxed);
	}

	// Wait for a new capture
	{
		QMutexLocker locker(&capture_mutex_);
		while (capture_seq_ == start_seq) {
			if (shouldStop) return false;
			if (!capture_cv_.wait(&capture_mutex_, timeout_ms)) {
				return false;
			}
		}
	}

	return snapshotLatestFrame(
	    out_raw_u16,
	    out_yuyv,
	    out_width,
	    out_height,
	    out_frame_ts_ms,
	    out_frame_valid,
	    out_range_min_ck,
	    out_range_max_ck,
	    out_scale,
	    out_type_colormap,
	    out_type_lepton);
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

static const char* getenv_def(const char* k, const char* d) {
	const char* v = std::getenv(k);
	return (v && v[0]) ? v : d;
}

void LeptonThread::loadCaptureStreamFxEnv() {
	const std::string on = getenv_def("RGB_CAPTURE_FX", "1");
	capture_fx_enabled_ = (on == "1" || on == "true" || on == "yes" || on == "on");
	capture_fx_shrink_sec_ = std::atof(getenv_def("RGB_CAPTURE_FX_SHRINK_SEC", "0.16"));
	capture_fx_hold_sec_ = std::atof(getenv_def("RGB_CAPTURE_FX_HOLD_SEC", "0.10"));
	capture_fx_fade_sec_ = std::atof(getenv_def("RGB_CAPTURE_FX_FADE_SEC", "0.26"));
	capture_fx_speed_ = std::atof(getenv_def("RGB_CAPTURE_FX_SPEED", "0.5"));
	capture_fx_inset_ratio_ = std::atof(getenv_def("RGB_CAPTURE_FX_INSET_RATIO", "0.03"));
	capture_fx_line_px_ = std::max(1, std::atoi(getenv_def("RGB_CAPTURE_FX_LINE_PX", "2")));
	// 짧은 변이 이 값(기본 480≈640x480)일 때 LINE_PX가 그대로 적용된다고 보고 스케일 (IR 160x120이면 ~1/4 두께)
	capture_fx_line_ref_min_ = std::max(1, std::atoi(getenv_def("RGB_CAPTURE_FX_LINE_REF", "480")));
	capture_fx_rgb_b_ = std::atoi(getenv_def("RGB_CAPTURE_FX_B", "40"));
	capture_fx_rgb_g_ = std::atoi(getenv_def("RGB_CAPTURE_FX_G", "40"));
	capture_fx_rgb_r_ = std::atoi(getenv_def("RGB_CAPTURE_FX_R", "220"));
}

void LeptonThread::notifyCaptureStreamFxStart() {
	if (!capture_fx_enabled_) return;
	using clock = std::chrono::steady_clock;
	const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
	    clock::now().time_since_epoch()).count();
	capture_fx_t0_ns_.store(ns, std::memory_order_relaxed);
}

double LeptonThread::captureFxSmoothstep01(double t) {
	if (t <= 0.0) return 0.0;
	if (t >= 1.0) return 1.0;
	return t * t * (3.0 - 2.0 * t);
}

void LeptonThread::yuyvToRgb888(const uint8_t* yuyv, int width, int height, std::vector<uint8_t>& out_rgb) {
	out_rgb.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
	size_t rgb_i = 0;
	const int num_pixels = width * height;
	const uint8_t* p = yuyv;
	for (int i = 0; i < num_pixels; i += 2) {
		const int y0 = p[0];
		const int u = p[1] - 128;
		const int y1 = p[2];
		const int v = p[3] - 128;
		auto clamp8 = [](int x) -> uint8_t {
			if (x < 0) return 0;
			if (x > 255) return 255;
			return static_cast<uint8_t>(x);
		};
		auto yuv_to_rgb = [&](int y, int u_, int v_) {
			const int c = y - 16;
			const int d = u_;
			const int e = v_;
			int r = (298 * c + 409 * e + 128) >> 8;
			int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
			int b = (298 * c + 516 * d + 128) >> 8;
			out_rgb[rgb_i++] = clamp8(r);
			out_rgb[rgb_i++] = clamp8(g);
			out_rgb[rgb_i++] = clamp8(b);
		};
		yuv_to_rgb(y0, u, v);
		yuv_to_rgb(y1, u, v);
		p += 4;
	}
}

void LeptonThread::rgb888ToYuyv(const uint8_t* rgb, int width, int height, uint8_t* yuyv) {
	auto clamp8 = [](int x) -> uint8_t {
		if (x < 0) return 0;
		if (x > 255) return 255;
		return static_cast<uint8_t>(x);
	};
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; x += 2) {
			const size_t i0 = (static_cast<size_t>(y) * width + x) * 3;
			int r0 = rgb[i0 + 0], g0 = rgb[i0 + 1], b0 = rgb[i0 + 2];
			int r1 = r0, g1 = g0, b1 = b0;
			if (x + 1 < width) {
				const size_t i1 = i0 + 3;
				r1 = rgb[i1 + 0]; g1 = rgb[i1 + 1]; b1 = rgb[i1 + 2];
			}
			const int y0 = ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8);
			const int y1 = ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8);
			const int u = ((-38 * (r0 + r1) - 74 * (g0 + g1) + 112 * (b0 + b1) + 256) >> 9) + 128;
			const int v = ((112 * (r0 + r1) - 94 * (g0 + g1) - 18 * (b0 + b1) + 256) >> 9) + 128;
			const int row_bytes = y * width * 2 + x * 2;
			yuyv[row_bytes + 0] = clamp8(y0);
			yuyv[row_bytes + 1] = clamp8(u);
			yuyv[row_bytes + 2] = clamp8(y1);
			yuyv[row_bytes + 3] = clamp8(v);
		}
	}
}

static void captureFxDrawHLineRgb(std::vector<uint8_t>& rgb, int w, int h, int y, int x0, int x1,
    uint8_t R, uint8_t G, uint8_t B) {
	if (y < 0 || y >= h) return;
	x0 = std::max(0, x0);
	x1 = std::min(w - 1, x1);
	for (int x = x0; x <= x1; ++x) {
		const size_t i = (static_cast<size_t>(y) * w + x) * 3;
		rgb[i + 0] = R;
		rgb[i + 1] = G;
		rgb[i + 2] = B;
	}
}

static void captureFxDrawVLineRgb(std::vector<uint8_t>& rgb, int w, int h, int x, int y0, int y1,
    uint8_t R, uint8_t G, uint8_t B) {
	if (x < 0 || x >= w) return;
	y0 = std::max(0, y0);
	y1 = std::min(h - 1, y1);
	for (int y = y0; y <= y1; ++y) {
		const size_t i = (static_cast<size_t>(y) * w + x) * 3;
		rgb[i + 0] = R;
		rgb[i + 1] = G;
		rgb[i + 2] = B;
	}
}

static void captureFxDrawRectOutlineRgb(std::vector<uint8_t>& rgb, int w, int h,
    int inset, int th, uint8_t R, uint8_t G, uint8_t B) {
	const int x0 = inset;
	const int y0 = inset;
	const int x1 = w - 1 - inset;
	const int y1 = h - 1 - inset;
	if (x1 <= x0 + th || y1 <= y0 + th) return;
	for (int t = 0; t < th; ++t) {
		captureFxDrawHLineRgb(rgb, w, h, y0 + t, x0, x1, R, G, B);
		captureFxDrawHLineRgb(rgb, w, h, y1 - t, x0, x1, R, G, B);
	}
	for (int t = 0; t < th; ++t) {
		captureFxDrawVLineRgb(rgb, w, h, x0 + t, y0 + th, y1 - th, R, G, B);
		captureFxDrawVLineRgb(rgb, w, h, x1 - t, y0 + th, y1 - th, R, G, B);
	}
}

void LeptonThread::applyCaptureStreamFxYuyv(uint8_t* dst_yuyv, const uint8_t* src_yuyv, int w, int h,
    double elapsed_sec) const {
	double sp = capture_fx_speed_;
	if (sp < 0.2) sp = 0.2;
	if (sp > 5.0) sp = 5.0;
	const double inv = 1.0 / sp;
	double t_shrink = std::max(0.02, capture_fx_shrink_sec_ * inv);
	double t_hold = std::max(0.0, capture_fx_hold_sec_ * inv);
	double t_fade = std::max(0.04, capture_fx_fade_sec_ * inv);
	const double t_plus = t_shrink + t_hold;
	const double total = t_shrink + t_hold + t_fade;
	if (elapsed_sec >= total) {
		std::memcpy(dst_yuyv, src_yuyv, static_cast<size_t>(w) * static_cast<size_t>(h) * 2);
		return;
	}

	yuyvToRgb888(src_yuyv, w, h, capture_fx_rgb_);
	const int inset_max = std::max(2, static_cast<int>(std::min(w, h) * capture_fx_inset_ratio_));
	int inset = 0;
	if (elapsed_sec < t_shrink) {
		const double u = captureFxSmoothstep01(elapsed_sec / t_shrink);
		inset = static_cast<int>(inset_max * u);
	} else {
		inset = inset_max;
	}
	double fade_k = 1.0;
	if (elapsed_sec > t_plus)
		fade_k = std::max(0.0, 1.0 - (elapsed_sec - t_plus) / t_fade);
	const double base_alpha = 0.88 * fade_k;
	const int mn = std::min(w, h);
	const int ref = std::max(1, capture_fx_line_ref_min_);
	const int th = std::max(1, (capture_fx_line_px_ * mn + ref / 2) / ref);
	const uint8_t R = static_cast<uint8_t>(std::max(0, std::min(255, capture_fx_rgb_r_)));
	const uint8_t G = static_cast<uint8_t>(std::max(0, std::min(255, capture_fx_rgb_g_)));
	const uint8_t B = static_cast<uint8_t>(std::max(0, std::min(255, capture_fx_rgb_b_)));

	std::vector<uint8_t> layer(static_cast<size_t>(w) * h * 3, 0);
	captureFxDrawRectOutlineRgb(layer, w, h, inset, th, R, G, B);
	if (elapsed_sec >= t_plus) {
		const int cx = w / 2;
		const int cy = h / 2;
		const int arm = std::max(6, static_cast<int>(std::min(w, h) * 0.065));
		captureFxDrawHLineRgb(layer, w, h, cy, cx - arm, cx + arm, R, G, B);
		for (int dy = 1; dy < th; ++dy) {
			captureFxDrawHLineRgb(layer, w, h, cy - dy, cx - arm, cx + arm, R, G, B);
			captureFxDrawHLineRgb(layer, w, h, cy + dy, cx - arm, cx + arm, R, G, B);
		}
		captureFxDrawVLineRgb(layer, w, h, cx, cy - arm, cy + arm, R, G, B);
		for (int dx = 1; dx < th; ++dx) {
			captureFxDrawVLineRgb(layer, w, h, cx - dx, cy - arm, cy + arm, R, G, B);
			captureFxDrawVLineRgb(layer, w, h, cx + dx, cy - arm, cy + arm, R, G, B);
		}
	}

	const float a = static_cast<float>(base_alpha);
	const float inv_a = 1.0f - a;
	for (size_t i = 0; i < capture_fx_rgb_.size(); i += 3) {
		const uint8_t lr = layer[i + 0], lg = layer[i + 1], lb = layer[i + 2];
		if (!lr && !lg && !lb) continue;
		const float fr = capture_fx_rgb_[i] * inv_a + lr * a;
		const float fg = capture_fx_rgb_[i + 1] * inv_a + lg * a;
		const float fb = capture_fx_rgb_[i + 2] * inv_a + lb * a;
		capture_fx_rgb_[i] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, fr)));
		capture_fx_rgb_[i + 1] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, fg)));
		capture_fx_rgb_[i + 2] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, fb)));
	}
	rgb888ToYuyv(capture_fx_rgb_.data(), w, h, dst_yuyv);
}

void LeptonThread::updateVpipe()
{
	if (v4l2sink < 0 || vidsendbuf == nullptr || v4l2_bufs_[0] == nullptr) return;
	const int yuyvSize = myImageWidth * myImageHeight * 2;
	int idx;
	if (v4l2_queued_ < V4L2_NBUF) {
		idx = v4l2_queued_;
	} else {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		buf.memory = V4L2_MEMORY_MMAP;
		if (ioctl(v4l2sink, VIDIOC_DQBUF, &buf) < 0) return;
		idx = buf.index;
		v4l2_queued_--;
	}
	const uint8_t* src_yuyv = vidsendbuf;
	double elapsed_sec = 0.0;
	bool apply_fx = false;
	if (capture_fx_enabled_) {
		const std::int64_t t0 = capture_fx_t0_ns_.load(std::memory_order_relaxed);
		if (t0 != 0) {
			using clock = std::chrono::steady_clock;
			const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
			    clock::now().time_since_epoch()).count();
			elapsed_sec = (now - t0) * 1e-9;
			double sp = capture_fx_speed_;
			if (sp < 0.2) sp = 0.2;
			if (sp > 5.0) sp = 5.0;
			const double inv = 1.0 / sp;
			const double total = std::max(0.05,
			    (capture_fx_shrink_sec_ + capture_fx_hold_sec_ + capture_fx_fade_sec_) * inv);
			if (elapsed_sec >= total) {
				capture_fx_t0_ns_.store(0, std::memory_order_relaxed);
			} else {
				apply_fx = true;
				if (capture_fx_scratch_yuyv_.size() < static_cast<size_t>(yuyvSize))
					capture_fx_scratch_yuyv_.resize(static_cast<size_t>(yuyvSize));
				applyCaptureStreamFxYuyv(capture_fx_scratch_yuyv_.data(), vidsendbuf, myImageWidth, myImageHeight,
				    elapsed_sec);
				src_yuyv = capture_fx_scratch_yuyv_.data();
			}
		}
	}
	memcpy(v4l2_bufs_[idx], src_yuyv, static_cast<size_t>(yuyvSize));
	struct v4l2_buffer buf;
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = idx;
	buf.bytesused = yuyvSize;
	if (ioctl(v4l2sink, VIDIOC_QBUF, &buf) == 0) v4l2_queued_++;
}

void LeptonThread::open_vpipe() {
    const int vidsendsiz = 160 * 120 * 2;  // YUYV422
    vidsendbuf = (uchar*)malloc(vidsendsiz);

    v4l2sink = open("/dev/video12", O_RDWR | O_NONBLOCK);  // O_RDWR: STREAMON에 필요할 수 있음
    if (v4l2sink < 0) {
        fprintf(stderr, "Failed to open v4l2sink device. (%s)\n", strerror(errno));
        exit(-2);
    }

    struct v4l2_format v;
    memset(&v, 0, sizeof(v));
    v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    v.fmt.pix.width = 160;
    v.fmt.pix.height = 120;
    v.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    v.fmt.pix.sizeimage = vidsendsiz;
    if (ioctl(v4l2sink, VIDIOC_S_FMT, &v) < 0) {
        fprintf(stderr, "Failed to set format on v4l2sink. (%s)\n", strerror(errno));
        exit(-1);
    }

    // pure_thermal 9 fps
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = 9;
    ioctl(v4l2sink, VIDIOC_S_PARM, &parm);  // ignore error

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = V4L2_NBUF;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(v4l2sink, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        exit(-1);
    }
    for (unsigned int i = 0; i < req.count && i < static_cast<unsigned>(V4L2_NBUF); i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(v4l2sink, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "VIDIOC_QUERYBUF failed: %s\n", strerror(errno));
            exit(-1);
        }
        v4l2_buf_len_ = buf.length;
        v4l2_bufs_[i] = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, v4l2sink, buf.m.offset);
        if (v4l2_bufs_[i] == MAP_FAILED) {
            fprintf(stderr, "mmap failed: %s\n", strerror(errno));
            exit(-1);
        }
    }
    // STREAMON 전에 초기 프레임 큐 (consumer 연결 시 데이터 대기)
    for (int i = 0; i < V4L2_NBUF; i++) {
        uint8_t* p = (uint8_t*)v4l2_bufs_[i];
        for (size_t j = 0; j < v4l2_buf_len_; j += 4) {
            p[j] = 0; p[j+1] = 128; p[j+2] = 0; p[j+3] = 128;  // YUYV black
        }
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.bytesused = vidsendsiz;
        if (ioctl(v4l2sink, VIDIOC_QBUF, &buf) == 0) v4l2_queued_++;
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(v4l2sink, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "VIDIOC_STREAMON failed: %s\n", strerror(errno));
        exit(-1);
    }
}





