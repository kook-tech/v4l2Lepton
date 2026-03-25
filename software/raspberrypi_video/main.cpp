#include <iostream>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <atomic>
#include <libgen.h>  // basename() 사용
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <optional>

#include <mosquitto.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

#include "Palettes.h"
#include "LeptonThread.h"

// 전역 종료 플래그
static std::atomic<bool> g_shutdown_requested{false};

// 시그널 핸들러
void signalHandler(int signal) {
	std::cerr << "\n[raspberrypi_video] 시그널 " << signal << " 수신, 종료 요청..." << std::endl;
	g_shutdown_requested = true;
}

static uint64_t now_ms() {
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

static bool ensure_parent_dir(const QString& path, QString* err) {
	QFileInfo fi(path);
	QDir d = fi.dir();
	if (d.exists()) return true;
	if (d.mkpath(".")) return true;
	if (err) *err = QString("Failed to create directory: %1").arg(d.absolutePath());
	return false;
}

static void yuyv_to_rgb888(const uint8_t* yuyv, int width, int height, std::vector<uint8_t>& out_rgb) {
	// YUYV422: Y0 U Y1 V (2 pixels per 4 bytes)
	out_rgb.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
	size_t rgb_i = 0;
	const int num_pixels = width * height;
	for (int i = 0; i < num_pixels; i += 2) {
		const int y0 = yuyv[0];
		const int u = yuyv[1] - 128;
		const int y1 = yuyv[2];
		const int v = yuyv[3] - 128;

		auto clamp = [](int x) -> uint8_t {
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
			out_rgb[rgb_i++] = clamp(r);
			out_rgb[rgb_i++] = clamp(g);
			out_rgb[rgb_i++] = clamp(b);
		};

		yuv_to_rgb(y0, u, v);
		yuv_to_rgb(y1, u, v);
		yuyv += 4;
	}
}

static bool save_jpg_from_yuyv(const QString& jpg_path, const std::vector<uint8_t>& yuyv, int width, int height, int quality, QString* err) {
	if (!ensure_parent_dir(jpg_path, err)) return false;
	if (static_cast<int>(yuyv.size()) < width * height * 2) {
		if (err) *err = "YUYV buffer too small";
		return false;
	}
	std::vector<uint8_t> rgb;
	yuyv_to_rgb888(yuyv.data(), width, height, rgb);

	QImage img(reinterpret_cast<const uchar*>(rgb.data()), width, height, QImage::Format_RGB888);
	QImage detached = img.copy();

	QSaveFile f(jpg_path);
	if (!f.open(QIODevice::WriteOnly)) {
		if (err) *err = QString("Failed to open jpg for write: %1").arg(jpg_path);
		return false;
	}
	if (!detached.save(&f, "JPG", quality)) {
		if (err) *err = QString("Failed to encode jpg: %1").arg(jpg_path);
		return false;
	}
	if (!f.commit()) {
		if (err) *err = QString("Failed to commit jpg: %1").arg(jpg_path);
		return false;
	}
	return true;
}

// raw16(centiKelvin) + 팔레트 → RGB PNG (YUYV 경유 없음, 무손실)
static bool save_png_from_raw(
    const QString& png_path,
    const std::vector<uint16_t>& raw_u16,
    int width,
    int height,
    uint16_t scale_min_ck,
    uint16_t scale_max_ck,
    int type_colormap,
    QString* err)
{
	if (png_path.isEmpty()) return true;
	if (!ensure_parent_dir(png_path, err)) return false;
	if (static_cast<int>(raw_u16.size()) < width * height) {
		if (err) *err = "raw_u16 buffer too small";
		return false;
	}

	const int* cmap = nullptr;
	int cmap_size = 0;

	if (type_colormap == 1) {
		cmap = colormap_rainbow;
		cmap_size = get_size_colormap_rainbow();
	} else if (type_colormap == 2) {
		cmap = colormap_grayscale;
		cmap_size = get_size_colormap_grayscale();
	} else if (type_colormap == 4) {
		// custom 팔레트: customizePalette2가 colormap_custom을 채운 상태
		cmap = colormap_custom;
		cmap_size = get_size_colormap_custom();
	} else {
		// 그 외에는 ironblack 기본 팔레트 사용
		cmap = colormap_ironblack;
		cmap_size = get_size_colormap_ironblack();
	}

	if (!cmap || cmap_size < 3) {
		if (err) *err = "invalid colormap";
		return false;
	}

	std::vector<uint8_t> rgb;
	rgb.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);

	const float diff = static_cast<float>(static_cast<int>(scale_max_ck) - static_cast<int>(scale_min_ck));
	const float scale = (diff > 0.0f) ? (3000.0f / diff) : 1.0f;

	for (int i = 0; i < width * height; ++i) {
		const uint16_t v = raw_u16[static_cast<size_t>(i)];
		uint8_t r = 0, g = 0, b = 0;

		if (v == 0) {
			// invalid → black
			r = g = b = 0;
		} else if (v <= scale_min_ck) {
			// min 이하 → white
			r = g = b = 255;
		} else if (v >= scale_max_ck) {
			// max 이상 → black
			r = g = b = 0;
		} else {
			float vf = (static_cast<float>(v) - static_cast<float>(scale_min_ck)) * scale;
			if (vf < 0.0f) vf = 0.0f;
			if (vf > 2999.0f) vf = 2999.0f;
			const int idx = static_cast<int>(vf);
			const int base = idx * 3;
			const int ofs_r = (base + 0 < cmap_size) ? base + 0 : cmap_size - 1;
			const int ofs_g = (base + 1 < cmap_size) ? base + 1 : cmap_size - 1;
			const int ofs_b = (base + 2 < cmap_size) ? base + 2 : cmap_size - 1;
			r = static_cast<uint8_t>(cmap[ofs_r]);
			g = static_cast<uint8_t>(cmap[ofs_g]);
			b = static_cast<uint8_t>(cmap[ofs_b]);
		}

		const size_t base_rgb = static_cast<size_t>(i) * 3;
		rgb[base_rgb + 0] = r;
		rgb[base_rgb + 1] = g;
		rgb[base_rgb + 2] = b;
	}

	QImage img(reinterpret_cast<const uchar*>(rgb.data()), width, height, QImage::Format_RGB888);
	QImage detached = img.copy();

	QSaveFile f(png_path);
	if (!f.open(QIODevice::WriteOnly)) {
		if (err) *err = QString("Failed to open png for write: %1").arg(png_path);
		return false;
	}
	if (!detached.save(&f, "PNG")) {
		if (err) *err = QString("Failed to encode png: %1").arg(png_path);
		return false;
	}
	if (!f.commit()) {
		if (err) *err = QString("Failed to commit png: %1").arg(png_path);
		return false;
	}
	return true;
}

static bool save_raw16_dump(const QString& raw_path, const std::vector<uint16_t>& raw_u16, QString* err) {
	if (!ensure_parent_dir(raw_path, err)) return false;
	QSaveFile f(raw_path);
	if (!f.open(QIODevice::WriteOnly)) {
		if (err) *err = QString("Failed to open raw for write: %1").arg(raw_path);
		return false;
	}
	const char* bytes = reinterpret_cast<const char*>(raw_u16.data());
	const qint64 nbytes = static_cast<qint64>(raw_u16.size() * sizeof(uint16_t));
	if (f.write(bytes, nbytes) != nbytes) {
		if (err) *err = QString("Failed to write raw bytes: %1").arg(raw_path);
		return false;
	}
	if (!f.commit()) {
		if (err) *err = QString("Failed to commit raw: %1").arg(raw_path);
		return false;
	}
	return true;
}

static bool save_meta_json(
    const QString& meta_path,
    int width,
    int height,
    uint64_t frame_ts_ms,
    bool frame_valid,
    uint16_t range_min_ck,
    uint16_t range_max_ck,
    float scale,
    int type_colormap,
    int type_lepton,
    const QString& jpg_path,
    const QString& raw_path,
    QString* err)
{
	if (meta_path.isEmpty()) return true;
	if (!ensure_parent_dir(meta_path, err)) return false;

	QJsonObject o;
	o["sensor_type"] = "THERMAL";
	o["frame_ts_ms"] = static_cast<qint64>(frame_ts_ms);
	o["frame_valid"] = frame_valid;
	o["width"] = width;
	o["height"] = height;
	o["raw_format"] = "uint16";
	o["raw_endianness"] = "little";
	o["raw_unit"] = "centi_kelvin";
	o["range_min_ck"] = static_cast<int>(range_min_ck);
	o["range_max_ck"] = static_cast<int>(range_max_ck);
	o["scale"] = scale;
	o["colormap_type"] = type_colormap;
	o["lepton_type"] = type_lepton;
	o["jpg_path"] = jpg_path;
	o["raw_path"] = raw_path;

	QSaveFile f(meta_path);
	if (!f.open(QIODevice::WriteOnly)) {
		if (err) *err = QString("Failed to open meta for write: %1").arg(meta_path);
		return false;
	}
	const QByteArray json = QJsonDocument(o).toJson(QJsonDocument::Compact);
	if (f.write(json) != json.size()) {
		if (err) *err = QString("Failed to write meta: %1").arg(meta_path);
		return false;
	}
	if (!f.commit()) {
		if (err) *err = QString("Failed to commit meta: %1").arg(meta_path);
		return false;
	}
	return true;
}

class ThermalCaptureMqtt {
public:
	explicit ThermalCaptureMqtt(LeptonThread* lepton) : lepton_(lepton) {}
	~ThermalCaptureMqtt() { stop(); }

	bool start() {
		const char* host = std::getenv("MQTT_HOST");
		const char* port_s = std::getenv("MQTT_PORT");
		const char* cmd_topic = std::getenv("IR_CMD_TOPIC");
		const char* result_topic = std::getenv("IR_RESULT_TOPIC");
		const char* capture_timeout_s = std::getenv("IR_CAPTURE_TIMEOUT_MS");

		mqtt_host_ = host ? host : "127.0.0.1";
		mqtt_port_ = port_s ? std::atoi(port_s) : 1883;
		// Default to "ir" for consistency with the rest of the stack.
		cmd_topic_ = cmd_topic ? cmd_topic : "local/ir/cmd";
		result_topic_ = result_topic ? result_topic : "local/ir/capture_result";
		capture_timeout_ms_ = capture_timeout_s ? std::atoi(capture_timeout_s) : 2000;
		if (capture_timeout_ms_ <= 0) capture_timeout_ms_ = 2000;

		mosquitto_lib_init();
		mosq_ = mosquitto_new(nullptr, true, this);
		if (!mosq_) {
			std::cerr << "[thermal_mqtt] mosquitto_new failed" << std::endl;
			return false;
		}
		mosquitto_message_callback_set(mosq_, &ThermalCaptureMqtt::on_message);
		mosquitto_connect_callback_set(mosq_, &ThermalCaptureMqtt::on_connect);
		mosquitto_disconnect_callback_set(mosq_, &ThermalCaptureMqtt::on_disconnect);
		mosquitto_reconnect_delay_set(mosq_, 1, 5, true);

		const int kConnectMaxAttempts = 30;
		for (int attempt = 1; attempt <= kConnectMaxAttempts; ++attempt) {
			const int rc = mosquitto_connect(mosq_, mqtt_host_.c_str(), mqtt_port_, 30);
			if (rc == MOSQ_ERR_SUCCESS) {
				std::cerr << "[thermal_mqtt] connect queued " << mqtt_host_ << ":" << mqtt_port_
				          << " (subscribe after CONNACK); publish: " << result_topic_ << std::endl;
				break;
			}
			std::cerr << "[thermal_mqtt] connect failed: " << mosquitto_strerror(rc)
			          << " (" << attempt << "/" << kConnectMaxAttempts << ")" << std::endl;
			if (attempt == kConnectMaxAttempts) {
				mosquitto_destroy(mosq_);
				mosq_ = nullptr;
				mosquitto_lib_cleanup();
				return false;
			}
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
		mosquitto_loop_start(mosq_);

		// Start worker thread for snapshot saving (avoid blocking mosquitto loop thread).
		stop_worker_ = false;
		worker_ = std::thread([this]() { this->worker_loop(); });
		return true;
	}

	void stop() {
		// Stop receiving callbacks first.
		if (mosq_) {
			mosquitto_loop_stop(mosq_, true);
		}

		// Stop worker thread.
		{
			std::lock_guard<std::mutex> lk(job_mu_);
			stop_worker_ = true;
		}
		job_cv_.notify_all();
		if (worker_.joinable()) {
			worker_.join();
		}

		if (mosq_) {
			mosquitto_disconnect(mosq_);
			mosquitto_destroy(mosq_);
			mosq_ = nullptr;
			mosquitto_lib_cleanup();
		}
	}

private:
	void subscribe_cmd_topics() {
		if (!mosq_) return;
		const int s = mosquitto_subscribe(mosq_, nullptr, cmd_topic_.c_str(), 1);
		if (s != MOSQ_ERR_SUCCESS) {
			std::cerr << "[thermal_mqtt] subscribe failed (" << cmd_topic_ << "): " << mosquitto_strerror(s) << std::endl;
		} else {
			std::cerr << "[thermal_mqtt] subscribed: " << cmd_topic_ << std::endl;
		}
		if (cmd_topic_ == "local/ir/cmd") {
			const int s2 = mosquitto_subscribe(mosq_, nullptr, "local/thermal/cmd", 1);
			if (s2 != MOSQ_ERR_SUCCESS) {
				std::cerr << "[thermal_mqtt] subscribe failed (local/thermal/cmd): " << mosquitto_strerror(s2) << std::endl;
			} else {
				std::cerr << "[thermal_mqtt] subscribed: local/thermal/cmd" << std::endl;
			}
		}
	}

	static void on_connect(struct mosquitto*, void* userdata, int rc) {
		auto* self = static_cast<ThermalCaptureMqtt*>(userdata);
		if (!self) return;
		if (rc == 0) {
			std::cerr << "[thermal_mqtt] MQTT connected (CONNACK)" << std::endl;
			self->subscribe_cmd_topics();
		} else {
			std::cerr << "[thermal_mqtt] MQTT CONNACK rejected rc=" << rc << " ("
			          << mosquitto_connack_string(rc) << ")" << std::endl;
		}
	}

	static void on_disconnect(struct mosquitto*, void* /*userdata*/, int rc) {
		std::cerr << "[thermal_mqtt] disconnected rc=" << rc << std::endl;
	}

	static void on_message(struct mosquitto*, void* userdata, const struct mosquitto_message* msg) {
		auto* self = static_cast<ThermalCaptureMqtt*>(userdata);
		self->handle_message(msg);
	}

	void publish_result(const QJsonObject& o) {
		if (!mosq_) return;
		const QByteArray json = QJsonDocument(o).toJson(QJsonDocument::Compact);
		mosquitto_publish(mosq_, nullptr, result_topic_.c_str(), json.size(), json.constData(), 1, false);
	}

	void handle_message(const struct mosquitto_message* msg) {
		const QByteArray payload(static_cast<const char*>(msg->payload), msg->payloadlen);
		const QJsonDocument doc = QJsonDocument::fromJson(payload);
		if (!doc.isObject()) return;

		const QJsonObject o = doc.object();
		const QString cmd = o.value("cmd").toString();
		if (cmd != "capture_now") return;

		Job job;
		job.request_ts_ms = o.contains("ts_ms") ? o.value("ts_ms").toVariant().toLongLong() : static_cast<qint64>(now_ms());
		job.request_id = o.value("request_id").toString();

		const QJsonObject out = o.value("out").toObject();
		job.jpg_path = out.value("jpg_path").toString();
		job.png_path = out.value("png_path").toString();
		job.raw_path = out.value("raw_path").toString();
		job.meta_path = out.value("meta_path").toString();
		job.jpg_quality = out.contains("jpg_quality") ? out.value("jpg_quality").toInt(90) : 90;

		printf("[thermal_mqtt] capture_now received: request_id=%s png_path=%s raw_path=%s\n",
			job.request_id.toUtf8().constData(),
			job.png_path.toUtf8().constData(),
			job.raw_path.toUtf8().constData());

		{
			std::lock_guard<std::mutex> lk(job_mu_);
			if (pending_job_.has_value()) {
				QJsonObject resp;
				resp["type"] = "thermal_capture_result";
				resp["cmd"] = "capture_now";
				resp["status"] = "failed";
				resp["error"] = "busy";
				resp["ts_ms"] = static_cast<qint64>(now_ms());
				resp["request_ts_ms"] = job.request_ts_ms;
				if (!job.request_id.isEmpty()) resp["request_id"] = job.request_id;
				publish_result(resp);
				return;
			}
			pending_job_ = job;
		}
		job_cv_.notify_one();
	}

private:
	struct Job {
		qint64 request_ts_ms = 0;
		QString request_id;
		QString jpg_path;
		QString png_path;
		QString raw_path;
		QString meta_path;
		int jpg_quality = 90;
	};

	void worker_loop() {
		for (;;) {
			std::optional<Job> job_opt;
			{
				std::unique_lock<std::mutex> lk(job_mu_);
				job_cv_.wait(lk, [&]() { return stop_worker_ || pending_job_.has_value(); });
				if (stop_worker_) return;
				job_opt.swap(pending_job_);
			}
			if (!job_opt.has_value()) continue;
			const Job job = *job_opt;

			QJsonObject resp;
			resp["type"] = "thermal_capture_result";
			resp["cmd"] = "capture_now";
			resp["ts_ms"] = static_cast<qint64>(now_ms());
			resp["request_ts_ms"] = job.request_ts_ms;
			if (!job.request_id.isEmpty()) resp["request_id"] = job.request_id;

			// 최소 요구 조건: raw_path는 항상 필요, 그리고 jpg 또는 png 중 하나는 있어야 한다.
			if (job.raw_path.isEmpty() || (job.jpg_path.isEmpty() && job.png_path.isEmpty())) {
				resp["status"] = "failed";
				resp["error"] = "out.raw_path and at least one of out.jpg_path/png_path are required";
				publish_result(resp);
				continue;
			}

			std::vector<uint16_t> raw_u16;
			std::vector<uint8_t> yuyv;
			int w = 0, h = 0;
			uint64_t frame_ts = 0;
			bool frame_valid = false;
			// NOTE:
			// - scale_min/max: palette mapping bounds (may be configured or auto-range in LeptonThread)
			// - range_min/max: computed from raw_u16 (true frame min/max; not the palette mapping bounds)
			uint16_t scale_min_ck = 0, scale_max_ck = 0;
			float scale = 0.0f;
			int type_colormap = 0, type_lepton = 0;

			// Capture the next valid frame on-demand (avoid continuous caching overhead).
			if (!lepton_->captureNextValidFrame(raw_u16, yuyv, w, h, frame_ts, frame_valid, scale_min_ck, scale_max_ck, scale, type_colormap, type_lepton, capture_timeout_ms_)) {
				resp["status"] = "failed";
				resp["error"] = "capture_timeout";
				resp["capture_timeout_ms"] = capture_timeout_ms_;
				// Add diagnostics (low overhead): last frame status helps pinpoint why we couldn't get a valid frame in time.
				uint64_t last_ts = 0;
				bool last_valid = false;
				bool last_incomplete = false;
				int last_pixels = 0;
				int last_expected = 0;
				int last_resets = 0;
				int last_segment = -1;
				int last_wrong_seg_streak = 0;
				int last_zero_streak = 0;
				if (lepton_->getLastFrameStatus(last_ts, last_valid, last_incomplete, last_pixels, last_expected, last_resets, last_segment, last_wrong_seg_streak, last_zero_streak)) {
					resp["last_frame_ts_ms"] = static_cast<qint64>(last_ts);
					resp["last_frame_age_ms"] = static_cast<qint64>(now_ms() - last_ts);
					resp["last_frame_valid"] = last_valid;
					resp["last_frame_incomplete"] = last_incomplete;
					resp["last_pixels_processed"] = last_pixels;
					resp["last_expected_pixels"] = last_expected;
					resp["last_spi_resets"] = last_resets;
					resp["last_segment_number"] = last_segment;
					resp["last_wrong_segment_streak"] = last_wrong_seg_streak;
					resp["last_zero_value_drop_streak"] = last_zero_streak;
				}
				publish_result(resp);
				continue;
			}

			// Compute true frame min/max from raw pixels (exclude 0 which is used as "invalid" in this codebase).
			uint16_t frame_min_ck = 65535;
			uint16_t frame_max_ck = 0;
			for (uint16_t v : raw_u16) {
				if (v == 0) continue;
				if (v < frame_min_ck) frame_min_ck = v;
				if (v > frame_max_ck) frame_max_ck = v;
			}
			if (frame_min_ck == 65535) frame_min_ck = 0;

			resp["frame_ts_ms"] = static_cast<qint64>(frame_ts);
			resp["frame_valid"] = frame_valid;
			resp["width"] = w;
			resp["height"] = h;
			// range_* is the true frame min/max (raw-based), NOT palette mapping bounds.
			resp["range_min_ck"] = static_cast<int>(frame_min_ck);
			resp["range_max_ck"] = static_cast<int>(frame_max_ck);
			resp["range_min_c"] = (static_cast<double>(frame_min_ck) - 27315.0) / 100.0;
			resp["range_max_c"] = (static_cast<double>(frame_max_ck) - 27315.0) / 100.0;

			// palette mapping bounds (for debugging/replay)
			resp["scale_min_ck"] = static_cast<int>(scale_min_ck);
			resp["scale_max_ck"] = static_cast<int>(scale_max_ck);
			resp["scale_min_c"] = (static_cast<double>(scale_min_ck) - 27315.0) / 100.0;
			resp["scale_max_c"] = (static_cast<double>(scale_max_ck) - 27315.0) / 100.0;

			resp["scale"] = scale;
			resp["colormap_type"] = type_colormap;
			resp["lepton_type"] = type_lepton;
			resp["raw_format"] = "uint16";
			resp["raw_endianness"] = "little";
			resp["raw_unit"] = "centi_kelvin";

			QString err;
			if (!save_raw16_dump(job.raw_path, raw_u16, &err)) {
				resp["status"] = "failed";
				resp["error"] = err;
				publish_result(resp);
				continue;
			}
			// 옵션: 요청에 따라 PNG와 JPG를 각각 저장한다.
			if (!job.png_path.isEmpty()) {
				if (!save_png_from_raw(job.png_path, raw_u16, w, h, scale_min_ck, scale_max_ck, type_colormap, &err)) {
					resp["status"] = "failed";
					resp["error"] = err;
					publish_result(resp);
					continue;
				}
			}
			if (!job.jpg_path.isEmpty()) {
				if (!save_jpg_from_yuyv(job.jpg_path, yuyv, w, h, job.jpg_quality, &err)) {
					resp["status"] = "failed";
					resp["error"] = err;
					publish_result(resp);
					continue;
				}
			}
			// meta_path is optional; prefer carrying metadata via capture_result for actions_result.json.
			if (!job.meta_path.isEmpty()) {
				// Keep legacy meta.json support: store palette scale bounds there for compatibility.
				if (!save_meta_json(job.meta_path, w, h, frame_ts, frame_valid, scale_min_ck, scale_max_ck, scale, type_colormap, type_lepton, job.jpg_path, job.raw_path, &err)) {
					resp["status"] = "failed";
					resp["error"] = err;
					publish_result(resp);
					continue;
				}
			}

			resp["status"] = "completed";
			if (!job.jpg_path.isEmpty()) resp["jpg_path"] = job.jpg_path;
			if (!job.png_path.isEmpty()) resp["png_path"] = job.png_path;
			resp["raw_path"] = job.raw_path;
			// meta_path is optional (most clients can rely on capture_result metadata).
			if (!job.meta_path.isEmpty()) resp["meta_path"] = job.meta_path;
			publish_result(resp);
			printf("[thermal_mqtt] capture result published: status=completed request_id=%s\n",
				job.request_id.toUtf8().constData());
			// v4l2 스트림에만 캡처 이펙트 (저장된 JPG/PNG/RAW 버퍼는 비표시)
			lepton_->notifyCaptureStreamFxStart();
		}
	}

	LeptonThread* lepton_;
	struct mosquitto* mosq_ = nullptr;
	std::string mqtt_host_;
	int mqtt_port_ = 1883;
	std::string cmd_topic_;
	std::string result_topic_;
	int capture_timeout_ms_ = 2000;

	std::mutex job_mu_;
	std::condition_variable job_cv_;
	std::optional<Job> pending_job_;
	bool stop_worker_ = false;
	std::thread worker_;
};

void printUsage(char *cmd) {
    char *cmdname = basename(cmd);
    printf("Usage: %s [OPTION]...\n"
           " -h      display this help and exit\n"
           " -v4l2 x use V4L2 capture device (e.g. /dev/video0) in Y16 mode (pure_thermal)\n"
           "           when set, SPI input is disabled\n"
           " -cm x   select colormap\n"
           "           1 : rainbow\n"
           "           2 : grayscale\n"
           "           3 : ironblack [default]\n"
           " -tl x   select type of Lepton\n"
           "           2 : Lepton 2.x [default]\n"
           "           3 : Lepton 3.x\n"
           " -ss x   SPI bus speed [MHz] (10 - 30)\n"
           "           20 : 20MHz [default]\n"
           " -min x  override minimum value for scaling (0 - 65535)\n"
           " -max x  override maximum value for scaling (0 - 65535)\n"
           " -auto   enable auto-range scaling (min/max will change dynamically per-frame)\n"
           " -d x    log level (0-255)\n"
           "", cmdname);
    return;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    int typeColormap = 3;  // 기본 컬러맵: IronBlack
    int typeLepton = 2;     // 기본 Lepton 버전: 2.x
    int spiSpeed = 20;      // 기본 SPI 속도: 20MHz
    int rangeMin = -1;      
    int rangeMax = -1;
    int loglevel = 0;
    const char* v4l2_device = nullptr;
    //target temperature
    int sigMin = -1;
    int sigMax = -1;
    //custompaletteversion
    int ver = 1;
    bool enable_auto_range = false;

    // 🔹 명령줄 인자 파싱
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "-d") == 0 && (i + 1 != argc)) {
            loglevel = std::atoi(argv[i + 1]) & 0xFF;
            i++;
        } else if (strcmp(argv[i], "-v4l2") == 0 && (i + 1 != argc)) {
            v4l2_device = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "-cm") == 0 && (i + 1 != argc)) {
            int val = std::atoi(argv[i + 1]);
            if (val == 1 || val == 2 || val == 4) {
                typeColormap = val;
                i++;
            }
        } else if (strcmp(argv[i], "-tl") == 0 && (i + 1 != argc)) {
            int val = std::atoi(argv[i + 1]);
            if (val == 3) {
                typeLepton = val;
                i++;
            }
        } else if (strcmp(argv[i], "-ss") == 0 && (i + 1 != argc)) {
            int val = std::atoi(argv[i + 1]);
            if (10 <= val && val <= 30) {
                spiSpeed = val;
                i++;
            }
        } else if (strcmp(argv[i], "-min") == 0 && (i + 1 != argc)) {
            //int val = std::atoi(argv[i +1]);
	        float valf = std::atof(argv[i + 1]);
            int val = (int)((valf + 273.15f) * 100.0f + 0.5f);
	        if (0 <= val && val <= 65535) {
                rangeMin = val;
                i++;
		        printf("min :  %.2f ( °C ) -> %d ( cK ) \n", valf, val);
            }
        } else if (strcmp(argv[i], "-max") == 0 && (i + 1 != argc)) {
	    //int val = std::atoi(argv[i +1]);
            float valf = std::atof(argv[i + 1]);
            int val = (int)((valf + 273.15f) * 100.0f + 0.5f);
            if (0 <= val && val <= 65535) {
                rangeMax = val;
                i++;
		        printf("max :  %.2f ( °C ) -> %d ( cK ) \n", valf, val);
            }
        }else if (strcmp(argv[i], "-sigmin") == 0 && (i + 1 != argc)) {
            float valf = std::atof(argv[i + 1]);
            int val = (int)((valf + 273.15f) * 100.0f + 0.5f);
            if (0 <= val && val <= 65535) {
                sigMin = val;
                i++;
		        printf("sigMin :  %.2f ( °C ) -> %d ( cK ) \n", valf, val);
            }
        }else if (strcmp(argv[i], "-sigmax") == 0 && (i + 1 != argc)) {
            float valf = std::atof(argv[i + 1]);
            int val = (int)((valf + 273.15f) * 100.0f + 0.5f);
            if (0 <= val && val <= 65535) {
                sigMax = val;
                i++;
		        printf("sigMax :  %.2f ( °C ) -> %d ( cK ) \n", valf, val);
            }
        }else if (strcmp(argv[i], "-ver") == 0 && (i + 1 != argc)) {
            int val = std::atoi(argv[i + 1]);
            if (1 <= val && val <= 2) {
                ver = val;
                i++;
		        printf("customize palette version : %d \n", ver);
            }
        } else if (strcmp(argv[i], "-auto") == 0) {
            enable_auto_range = true;
        }

    }


    LeptonThread *thread = new LeptonThread();
    thread->setLogLevel(loglevel);
    if (v4l2_device) {
        thread->useV4l2Input(v4l2_device);
    }
    // sigMin/sigMax이 주어진 경우에만 PaletteCustomizing 적용
    if (sigMin >= 0 && sigMax >= 0) {
        if (ver == 1) customizePalette(sigMin, sigMax, rangeMin, rangeMax);
        else if( ver == 2 ) customizePalette2(sigMin, sigMax, rangeMin, rangeMax);
    }
    //
    thread->useColormap(typeColormap);
    thread->useLepton(typeLepton);
    thread->useSpiSpeedMhz(spiSpeed);
    // Default is FIXED scaling range (no dynamic min/max). Enable auto-range only if explicitly requested.
    if (enable_auto_range && rangeMin < 0 && rangeMax < 0) {
        thread->setAutomaticScalingRange();
    }
    if (rangeMin >= 0) thread->useRangeMinValue(rangeMin);
    if (rangeMax >= 0) thread->useRangeMaxValue(rangeMax);
    
    
    // 시그널 핸들러 등록
    signal(SIGTERM, signalHandler);
    signal(SIGINT, signalHandler);
    
    thread->start();

    ThermalCaptureMqtt thermal_mqtt(thread);
    thermal_mqtt.start();

    // 종료 신호 대기
    while (!g_shutdown_requested) {
        usleep(100000);  // 100ms 대기
    }
    
    // 정리 작업
    std::cerr << "[raspberrypi_video] 종료 중..." << std::endl;
    thermal_mqtt.stop();
    thread->stop();
    thread->wait(5000);  // 최대 5초 대기
    
    if (thread->isRunning()) {
        std::cerr << "[raspberrypi_video] 스레드가 응답하지 않아 강제 종료합니다." << std::endl;
        thread->terminate();
        thread->wait(1000);
    }
    
    delete thread;
    std::cerr << "[raspberrypi_video] 정상 종료 완료" << std::endl;
    
    return 0;
}


