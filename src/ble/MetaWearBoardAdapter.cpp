#include "headmotion/ble/MetaWearBoardAdapter.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "metawear/core/status.h"
}

namespace headmotion {

namespace {

constexpr int SCAN_MS = 5000;
constexpr int INIT_TIMEOUT_MS = 10000;
constexpr int LOGGER_TIMEOUT_MS = 10000;
constexpr int DOWNLOAD_TIMEOUT_MINUTES = 30;
constexpr int RESPONSE_TIMEOUT_MS = 2000;

constexpr int DOWNLOAD_BATCH_SIZE = 255;

constexpr int32_t GYRO_IMPL_BMI160 = 0;
constexpr int32_t GYRO_IMPL_BMI270 = 1;

std::string safeString(const char *value) {
  return value == nullptr ? "" : std::string(value);
}

std::string uuidToString(uint64_t high, uint64_t low) {
  std::ostringstream oss;

  oss << std::hex << std::setfill('0') << std::nouppercase;

  oss << std::setw(8)
      << static_cast<unsigned long long>((high >> 32) & 0xffffffffULL);
  oss << "-";

  oss << std::setw(4)
      << static_cast<unsigned long long>((high >> 16) & 0xffffULL);
  oss << "-";

  oss << std::setw(4) << static_cast<unsigned long long>(high & 0xffffULL);
  oss << "-";

  oss << std::setw(4)
      << static_cast<unsigned long long>((low >> 48) & 0xffffULL);
  oss << "-";

  oss << std::setw(12)
      << static_cast<unsigned long long>(low & 0x0000ffffffffffffULL);

  return oss.str();
}

SimpleBLE::BluetoothUUID
serviceUuidFromGattChar(const MblMwGattChar *characteristic) {
  return SimpleBLE::BluetoothUUID(uuidToString(
      characteristic->service_uuid_high, characteristic->service_uuid_low));
}

SimpleBLE::BluetoothUUID
characteristicUuidFromGattChar(const MblMwGattChar *characteristic) {
  return SimpleBLE::BluetoothUUID(
      uuidToString(characteristic->uuid_high, characteristic->uuid_low));
}

MblMwGyroBoschOdr gyroOdrFromHz(float hz) {
  if (hz <= 25.0f) {
    return MBL_MW_GYRO_BOSCH_ODR_25Hz;
  }

  if (hz <= 50.0f) {
    return MBL_MW_GYRO_BOSCH_ODR_50Hz;
  }

  if (hz <= 100.0f) {
    return MBL_MW_GYRO_BOSCH_ODR_100Hz;
  }

  if (hz <= 200.0f) {
    return MBL_MW_GYRO_BOSCH_ODR_200Hz;
  }

  if (hz <= 400.0f) {
    return MBL_MW_GYRO_BOSCH_ODR_400Hz;
  }

  if (hz <= 800.0f) {
    return MBL_MW_GYRO_BOSCH_ODR_800Hz;
  }

  if (hz <= 1600.0f) {
    return MBL_MW_GYRO_BOSCH_ODR_1600Hz;
  }

  return MBL_MW_GYRO_BOSCH_ODR_3200Hz;
}

MblMwGyroBoschRange gyroRangeFromDps(float dps) {
  if (dps <= 125.0f) {
    return MBL_MW_GYRO_BOSCH_RANGE_125dps;
  }

  if (dps <= 250.0f) {
    return MBL_MW_GYRO_BOSCH_RANGE_250dps;
  }

  if (dps <= 500.0f) {
    return MBL_MW_GYRO_BOSCH_RANGE_500dps;
  }

  if (dps <= 1000.0f) {
    return MBL_MW_GYRO_BOSCH_RANGE_1000dps;
  }

  return MBL_MW_GYRO_BOSCH_RANGE_2000dps;
}

} // namespace

MetaWearBoardAdapter::MetaWearBoardAdapter() {
  connection_.context = this;
  connection_.write_gatt_char = writeGattChar;
  connection_.read_gatt_char = readGattChar;
  connection_.enable_notifications = enableNotifications;
  connection_.on_disconnect = onDisconnect;

  board_ = mbl_mw_metawearboard_create(&connection_);
  mbl_mw_metawearboard_set_time_for_response(board_, RESPONSE_TIMEOUT_MS);
}

MetaWearBoardAdapter::~MetaWearBoardAdapter() {
  disconnect();

  if (board_ != nullptr) {
    mbl_mw_metawearboard_free(board_);
    board_ = nullptr;
  }
}

Result MetaWearBoardAdapter::connect(const std::string &target) {
  try {
    if (!SimpleBLE::Adapter::bluetooth_enabled()) {
      return Result::error("Bluetooth is not enabled");
    }

    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
      return Result::error("No Bluetooth adapters found");
    }

    auto adapter = adapters.at(0);

    adapter.scan_for(SCAN_MS);
    auto peripherals = adapter.scan_get_results();

    for (auto &peripheral : peripherals) {
      if (!targetMatches(peripheral, target)) {
        continue;
      }

      peripheral.connect();

      if (!peripheral.is_connected()) {
        return Result::error(
            "BLE connect returned but peripheral is not connected");
      }

      connectedAddress_ = peripheral.address();
      peripheral_ = peripheral;

      return Result::success();
    }

    return Result::error("Could not find BLE target: " + target);
  } catch (const std::exception &e) {
    return Result::error(std::string("BLE connect failed: ") + e.what());
  }
}

Result MetaWearBoardAdapter::initialize() {
  if (board_ == nullptr) {
    return Result::error("MetaWear board object is null");
  }

  if (!peripheral_.has_value() || !peripheral_->is_connected()) {
    return Result::error("Cannot initialize before BLE connection");
  }

  InitWaiter waiter;

  mbl_mw_metawearboard_initialize(
      board_, &waiter, [](void *context, MblMwMetaWearBoard *, int32_t status) {
        auto *waiter = static_cast<InitWaiter *>(context);

        {
          std::lock_guard<std::mutex> lock(waiter->mutex);
          waiter->status = status;
          waiter->done = true;
        }

        waiter->cv.notify_all();
      });

  std::unique_lock<std::mutex> lock(waiter.mutex);
  const bool completed =
      waiter.cv.wait_for(lock, std::chrono::milliseconds(INIT_TIMEOUT_MS),
                         [&waiter] { return waiter.done; });

  if (!completed) {
    return Result::error("Timed out waiting for MetaWear initialization");
  }

  if (waiter.status != MBL_MW_STATUS_OK) {
    return Result::error("MetaWear initialization failed with status " +
                         std::to_string(waiter.status));
  }

  initialized_ = mbl_mw_metawearboard_is_initialized(board_) != 0;

  if (!initialized_) {
    return Result::error("MetaWear SDK reports board is not initialized");
  }

  return Result::success();
}

void MetaWearBoardAdapter::disconnect() {
  try {
    if (peripheral_.has_value() && peripheral_->is_connected()) {
      peripheral_->disconnect();
    }
  } catch (...) {
    // Cleanup should not throw.
  }

  initialized_ = false;
}

bool MetaWearBoardAdapter::isInitialized() const { return initialized_; }

Result MetaWearBoardAdapter::readDeviceInfo(SessionMetadata &metadataOut) {
  if (board_ == nullptr || !initialized_) {
    return Result::error("Cannot read device info before initialization");
  }

  const MblMwDeviceInformation *info =
      mbl_mw_metawearboard_get_device_information(board_);

  if (info == nullptr) {
    return Result::error("MetaWear SDK returned null device information");
  }

  metadataOut.deviceMac = connectedAddress_;
  metadataOut.manufacturer = safeString(info->manufacturer);
  metadataOut.modelNumber = safeString(info->model_number);
  metadataOut.serialNumber = safeString(info->serial_number);
  metadataOut.firmwareRevision = safeString(info->firmware_revision);
  metadataOut.hardwareRevision = safeString(info->hardware_revision);

  return Result::success();
}

Result
MetaWearBoardAdapter::configureHeadMotionSignals(const SessionConfig &config) {
  if (board_ == nullptr || !initialized_) {
    return Result::error("Cannot configure signals before initialization");
  }

  Result gyroDetectResult = detectGyroImpl();
  if (!gyroDetectResult.ok()) {
    return gyroDetectResult;
  }

  mbl_mw_acc_set_odr(board_, config.accelHz);
  mbl_mw_acc_set_range(board_, config.accelRangeG);
  mbl_mw_acc_write_acceleration_config(board_);

  Result gyroConfigResult = configureGyro(config);
  if (!gyroConfigResult.ok()) {
    return gyroConfigResult;
  }

  accelSignal_ = mbl_mw_acc_get_acceleration_data_signal(board_);
  gyroSignal_ = getGyroSignal();

  if (accelSignal_ == nullptr) {
    return Result::error("Failed to get accelerometer data signal");
  }

  if (gyroSignal_ == nullptr) {
    return Result::error("Failed to get gyroscope data signal");
  }

  return Result::success();
}

Result
MetaWearBoardAdapter::createHeadMotionLoggers(SessionMetadata &metadataInOut) {
  if (accelSignal_ == nullptr || gyroSignal_ == nullptr) {
    return Result::error("Cannot create loggers before signal configuration");
  }

  LoggerWaiter accelWaiter;

  mbl_mw_datasignal_log(accelSignal_, &accelWaiter,
                        [](void *context, MblMwDataLogger *logger) {
                          auto *waiter = static_cast<LoggerWaiter *>(context);

                          {
                            std::lock_guard<std::mutex> lock(waiter->mutex);
                            waiter->logger = logger;
                            waiter->done = true;
                          }

                          waiter->cv.notify_all();
                        });

  Result result = waitForLogger(accelWaiter, "accelerometer", accelLogger_);
  if (!result.ok()) {
    return result;
  }

  LoggerWaiter gyroWaiter;

  mbl_mw_datasignal_log(gyroSignal_, &gyroWaiter,
                        [](void *context, MblMwDataLogger *logger) {
                          auto *waiter = static_cast<LoggerWaiter *>(context);

                          {
                            std::lock_guard<std::mutex> lock(waiter->mutex);
                            waiter->logger = logger;
                            waiter->done = true;
                          }

                          waiter->cv.notify_all();
                        });

  result = waitForLogger(gyroWaiter, "gyroscope", gyroLogger_);
  if (!result.ok()) {
    return result;
  }

  metadataInOut.accelLoggerId = mbl_mw_logger_get_id(accelLogger_);
  metadataInOut.gyroLoggerId = mbl_mw_logger_get_id(gyroLogger_);

  return Result::success();
}

Result MetaWearBoardAdapter::startLogging(bool overwrite) {
  if (board_ == nullptr || !initialized_) {
    return Result::error("Cannot start logging before initialization");
  }

  if (accelLogger_ == nullptr || gyroLogger_ == nullptr) {
    return Result::error("Cannot start logging before logger creation");
  }

  mbl_mw_logging_start(board_, overwrite ? 1 : 0);

  mbl_mw_acc_enable_acceleration_sampling(board_);
  mbl_mw_acc_start(board_);

  Result gyroStartResult = startGyro();
  if (!gyroStartResult.ok()) {
    return gyroStartResult;
  }

  return Result::success();
}

Result MetaWearBoardAdapter::stopLogging() {
    if (board_ == nullptr || !initialized_) {
        return Result::error("Cannot stop logging before initialization");
    }

    // During sync, the board state is deserialized and initialized, but
    // configureHeadMotionSignals() is not called. That means gyroImpl_ may
    // still be Unknown even though the board has a valid gyro module.
    if (gyroImpl_ == GyroImpl::Unknown) {
        Result detectResult = detectGyroImpl();
        if (!detectResult.ok()) {
            return detectResult;
        }
    }

    mbl_mw_acc_stop(board_);
    mbl_mw_acc_disable_acceleration_sampling(board_);

    Result gyroStopResult = stopGyro();
    if (!gyroStopResult.ok()) {
        return gyroStopResult;
    }

    mbl_mw_logging_stop(board_);

    return Result::success();
}

Result MetaWearBoardAdapter::flushLogPage() {
  if (board_ == nullptr || !initialized_) {
    return Result::error("Cannot flush log page before initialization");
  }

  mbl_mw_logging_flush_page(board_);
  return Result::success();
}

Result MetaWearBoardAdapter::subscribeLoggers(
    const SessionMetadata &metadata,
    std::function<void(const ImuSample &)> onSample) {
  if (board_ == nullptr || !initialized_) {
    return Result::error("Cannot subscribe loggers before initialization");
  }

  if (metadata.accelLoggerId == 0xff || metadata.gyroLoggerId == 0xff) {
    return Result::error(
        "Cannot subscribe loggers: metadata has invalid logger IDs");
  }

  accelLogger_ = mbl_mw_logger_lookup_id(board_, metadata.accelLoggerId);
  gyroLogger_ = mbl_mw_logger_lookup_id(board_, metadata.gyroLoggerId);

  if (accelLogger_ == nullptr) {
    return Result::error("Could not look up accelerometer logger ID " +
                         std::to_string(metadata.accelLoggerId));
  }

  if (gyroLogger_ == nullptr) {
    return Result::error("Could not look up gyroscope logger ID " +
                         std::to_string(metadata.gyroLoggerId));
  }

  onSample_ = std::move(onSample);

  accelCallbackCtx_.self = this;
  accelCallbackCtx_.kind = ImuSample::Kind::Accel;

  gyroCallbackCtx_.self = this;
  gyroCallbackCtx_.kind = ImuSample::Kind::Gyro;

  mbl_mw_logger_subscribe(accelLogger_, &accelCallbackCtx_, handleLoggerData);
  mbl_mw_logger_subscribe(gyroLogger_, &gyroCallbackCtx_, handleLoggerData);

  return Result::success();
}

Result MetaWearBoardAdapter::downloadLogs(
    std::function<void(const DownloadProgress &)> onProgress) {
  if (board_ == nullptr || !initialized_) {
    return Result::error("Cannot download logs before initialization");
  }

  struct DownloadWaiter {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    uint32_t lastEntriesLeft = 0;
    uint32_t lastTotalEntries = 0;
    std::function<void(const DownloadProgress &)> progress;
  };

  DownloadWaiter waiter;
  waiter.progress = std::move(onProgress);

  MblMwLogDownloadHandler handler = {};
  handler.context = &waiter;

  handler.received_progress_update = [](void *context, uint32_t entriesLeft,
                                        uint32_t totalEntries) {
    auto *waiter = static_cast<DownloadWaiter *>(context);

    if (waiter->progress) {
      DownloadProgress progress;
      progress.entriesLeft = entriesLeft;
      progress.totalEntries = totalEntries;
      waiter->progress(progress);
    }

    {
      std::lock_guard<std::mutex> lock(waiter->mutex);
      waiter->lastEntriesLeft = entriesLeft;
      waiter->lastTotalEntries = totalEntries;

      if (entriesLeft == 0) {
        waiter->done = true;
      }
    }

    waiter->cv.notify_all();
  };

  handler.received_unknown_entry = [](void *, uint8_t id, int64_t,
                                      const uint8_t *, uint8_t) {
    std::cerr << "[MetaWearBoardAdapter] Unknown log entry id="
              << static_cast<int>(id) << "\n";
  };

  handler.received_unhandled_entry = [](void *, const MblMwData *) {
    std::cerr << "[MetaWearBoardAdapter] Unhandled log entry\n";
  };

  mbl_mw_logging_download(board_, DOWNLOAD_BATCH_SIZE, &handler);

  std::unique_lock<std::mutex> lock(waiter.mutex);
  const bool completed =
      waiter.cv.wait_for(lock, std::chrono::minutes(DOWNLOAD_TIMEOUT_MINUTES),
                         [&waiter] { return waiter.done; });

  if (!completed) {
    return Result::error("Timed out waiting for log download to complete");
  }

  return Result::success();
}

Result MetaWearBoardAdapter::clearLogEntries() {
  if (board_ == nullptr || !initialized_) {
    return Result::error("Cannot clear log entries before initialization");
  }

  mbl_mw_logging_clear_entries(board_);
  return Result::success();
}
Result MetaWearBoardAdapter::tearDownBoard() {
  if (board_ == nullptr || !initialized_) {
    return Result::error("Cannot tear down board before initialization");
  }

  accelSignal_ = nullptr;
  gyroSignal_ = nullptr;
  accelLogger_ = nullptr;
  gyroLogger_ = nullptr;

  mbl_mw_metawearboard_tear_down(board_);

  return Result::success();
}
void MetaWearBoardAdapter::handleLoggerData(void *context,
                                            const MblMwData *data) {
  auto *loggerContext = static_cast<LoggerCallbackContext *>(context);

  if (loggerContext == nullptr || loggerContext->self == nullptr ||
      data == nullptr || data->value == nullptr) {
    return;
  }

  auto *self = loggerContext->self;

  if (!self->onSample_) {
    return;
  }

  if (data->type_id != MBL_MW_DT_ID_CARTESIAN_FLOAT) {
    std::cerr << "[MetaWearBoardAdapter] Ignoring logger data type_id="
              << static_cast<int>(data->type_id) << " kind="
              << (loggerContext->kind == ImuSample::Kind::Accel ? "accel"
                                                                : "gyro")
              << "\n";
    return;
  }

  const auto *xyz = static_cast<const MblMwCartesianFloat *>(data->value);

  ImuSample sample;
  sample.kind = loggerContext->kind;
  sample.epochMs = static_cast<int64_t>(data->epoch);
  sample.x = xyz->x;
  sample.y = xyz->y;
  sample.z = xyz->z;

  self->onSample_(sample);
}

Result MetaWearBoardAdapter::waitForLogger(LoggerWaiter &waiter,
                                           const std::string &name,
                                           MblMwDataLogger *&outLogger) {
  std::unique_lock<std::mutex> lock(waiter.mutex);

  const bool completed =
      waiter.cv.wait_for(lock, std::chrono::milliseconds(LOGGER_TIMEOUT_MS),
                         [&waiter] { return waiter.done; });

  if (!completed) {
    return Result::error("Timed out waiting for " + name + " logger");
  }

  if (waiter.logger == nullptr) {
    return Result::error("SDK returned null " + name + " logger");
  }

  outLogger = waiter.logger;
  return Result::success();
}

bool MetaWearBoardAdapter::targetMatches(SimpleBLE::Peripheral &peripheral,
                                         const std::string &target) {
  const std::string targetLower = lower(target);
  const std::string idLower = lower(peripheral.identifier());
  const std::string addrLower = lower(peripheral.address());

  return idLower == targetLower || addrLower == targetLower ||
         idLower.find(targetLower) != std::string::npos;
}

std::string MetaWearBoardAdapter::lower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  return value;
}

void MetaWearBoardAdapter::writeGattChar(void *context, const void *,
                                         MblMwGattCharWriteType writeType,
                                         const MblMwGattChar *characteristic,
                                         const uint8_t *value, uint8_t length) {
  auto *self = static_cast<MetaWearBoardAdapter *>(context);

  if (self == nullptr || !self->peripheral_.has_value() ||
      characteristic == nullptr) {
    return;
  }

  try {
    std::vector<uint8_t> bytes(value, value + length);

    const auto serviceUuid = serviceUuidFromGattChar(characteristic);
    const auto characteristicUuid =
        characteristicUuidFromGattChar(characteristic);

    if (writeType == MBL_MW_GATT_CHAR_WRITE_WITHOUT_RESPONSE) {
      self->peripheral_->write_command(serviceUuid, characteristicUuid, bytes);
    } else {
      self->peripheral_->write_request(serviceUuid, characteristicUuid, bytes);
    }
  } catch (const std::exception &e) {
    std::cerr << "[MetaWearBoardAdapter] writeGattChar failed: " << e.what()
              << "\n";
  }
}

void MetaWearBoardAdapter::readGattChar(void *context, const void *caller,
                                        const MblMwGattChar *characteristic,
                                        MblMwFnIntVoidPtrArray handler) {
  auto *self = static_cast<MetaWearBoardAdapter *>(context);

  if (self == nullptr || !self->peripheral_.has_value() ||
      characteristic == nullptr || handler == nullptr) {
    return;
  }

  try {
    const auto serviceUuid = serviceUuidFromGattChar(characteristic);
    const auto characteristicUuid =
        characteristicUuidFromGattChar(characteristic);

    auto bytes = self->peripheral_->read(serviceUuid, characteristicUuid);

    handler(caller, reinterpret_cast<const uint8_t *>(bytes.data()),
            static_cast<uint8_t>(bytes.size()));
  } catch (const std::exception &e) {
    std::cerr << "[MetaWearBoardAdapter] readGattChar failed: " << e.what()
              << "\n";
  }
}

void MetaWearBoardAdapter::enableNotifications(
    void *context, const void *caller, const MblMwGattChar *characteristic,
    MblMwFnIntVoidPtrArray handler, MblMwFnVoidVoidPtrInt ready) {
  auto *self = static_cast<MetaWearBoardAdapter *>(context);

  if (self == nullptr || !self->peripheral_.has_value() ||
      characteristic == nullptr || handler == nullptr) {
    if (ready != nullptr) {
      ready(caller, -1);
    }
    return;
  }

  try {
    const auto serviceUuid = serviceUuidFromGattChar(characteristic);
    const auto characteristicUuid =
        characteristicUuidFromGattChar(characteristic);

    self->peripheral_->notify(
        serviceUuid, characteristicUuid,
        [caller, handler](SimpleBLE::ByteArray bytes) {
          handler(caller, reinterpret_cast<const uint8_t *>(bytes.data()),
                  static_cast<uint8_t>(bytes.size()));
        });

    if (ready != nullptr) {
      ready(caller, 0);
    }
  } catch (const std::exception &e) {
    std::cerr << "[MetaWearBoardAdapter] enableNotifications failed: "
              << e.what() << "\n";

    if (ready != nullptr) {
      ready(caller, -1);
    }
  }
}

void MetaWearBoardAdapter::onDisconnect(void *context, const void *caller,
                                        MblMwFnVoidVoidPtrInt handler) {
  auto *self = static_cast<MetaWearBoardAdapter *>(context);

  if (self == nullptr || !self->peripheral_.has_value() || handler == nullptr) {
    return;
  }

  try {
    self->peripheral_->set_callback_on_disconnected(
        [caller, handler]() { handler(caller, 0); });
  } catch (const std::exception &e) {
    std::cerr << "[MetaWearBoardAdapter] onDisconnect setup failed: "
              << e.what() << "\n";
  }
}
extern "C" void mbl_mw_memory_free(void *ptr);

Result
MetaWearBoardAdapter::serializeBoardState(std::vector<uint8_t> &stateOut) {
  stateOut.clear();

  if (board_ == nullptr || !initialized_) {
    return Result::error("Cannot serialize board state before initialization");
  }

  uint32_t size = 0;
  uint8_t *bytes = mbl_mw_metawearboard_serialize(board_, &size);

  if (bytes == nullptr || size == 0) {
    return Result::error("MetaWear SDK returned empty serialized board state");
  }

  stateOut.assign(bytes, bytes + size);
  mbl_mw_memory_free(bytes);

  return Result::success();
}

Result
MetaWearBoardAdapter::deserializeBoardState(const std::vector<uint8_t> &state) {
  if (board_ == nullptr) {
    return Result::error("Cannot deserialize board state: board is null");
  }

  if (state.empty()) {
    return Result::error("Cannot deserialize empty board state");
  }

  std::vector<uint8_t> mutableState = state;

  const int32_t status = mbl_mw_metawearboard_deserialize(
      board_, mutableState.data(), static_cast<uint32_t>(mutableState.size()));

  if (status != MBL_MW_STATUS_OK) {
    return Result::error(
        "MetaWear board-state deserialize failed with status " +
        std::to_string(status));
  }

  return Result::success();
}
Result MetaWearBoardAdapter::detectGyroImpl() {
    if (board_ == nullptr || !initialized_) {
        return Result::error("Cannot detect gyro implementation before initialization");
    }

    const int32_t impl = mbl_mw_metawearboard_lookup_module(board_, MBL_MW_MODULE_GYRO);

    std::cerr << "[MetaWearBoardAdapter] Raw gyro implementation value: "
              << impl << "\n";

    if (impl < 0) {
        gyroImpl_ = GyroImpl::Unknown;
        return Result::error("Gyroscope module is not present on this board");
    }

    if (impl == GYRO_IMPL_BMI160) {
        gyroImpl_ = GyroImpl::Bmi160;
        std::cerr << "[MetaWearBoardAdapter] Gyro implementation: BMI160\n";
        return Result::success();
    }

    if (impl == GYRO_IMPL_BMI270) {
        gyroImpl_ = GyroImpl::Bmi270;
        std::cerr << "[MetaWearBoardAdapter] Gyro implementation: BMI270\n";
        return Result::success();
    }

    gyroImpl_ = GyroImpl::Unknown;
    return Result::error("Unknown gyroscope implementation: " + std::to_string(impl));
}

Result MetaWearBoardAdapter::configureGyro(const SessionConfig &config) {
  if (gyroImpl_ == GyroImpl::Bmi160) {
    mbl_mw_gyro_bmi160_set_odr(board_, gyroOdrFromHz(config.gyroHz));
    mbl_mw_gyro_bmi160_set_range(board_, gyroRangeFromDps(config.gyroRangeDps));
    mbl_mw_gyro_bmi160_write_config(board_);
    return Result::success();
  }

  if (gyroImpl_ == GyroImpl::Bmi270) {
    mbl_mw_gyro_bmi270_set_odr(board_, gyroOdrFromHz(config.gyroHz));
    mbl_mw_gyro_bmi270_set_range(board_, gyroRangeFromDps(config.gyroRangeDps));
    mbl_mw_gyro_bmi270_write_config(board_);
    return Result::success();
  }

  return Result::error("Cannot configure gyro: unknown implementation");
}

MblMwDataSignal *MetaWearBoardAdapter::getGyroSignal() {
  if (gyroImpl_ == GyroImpl::Bmi160) {
    return mbl_mw_gyro_bmi160_get_rotation_data_signal(board_);
  }

  if (gyroImpl_ == GyroImpl::Bmi270) {
    return mbl_mw_gyro_bmi270_get_rotation_data_signal(board_);
  }

  return nullptr;
}

Result MetaWearBoardAdapter::startGyro() {
  if (gyroImpl_ == GyroImpl::Bmi160) {
    mbl_mw_gyro_bmi160_enable_rotation_sampling(board_);
    mbl_mw_gyro_bmi160_start(board_);
    return Result::success();
  }

  if (gyroImpl_ == GyroImpl::Bmi270) {
    mbl_mw_gyro_bmi270_enable_rotation_sampling(board_);
    mbl_mw_gyro_bmi270_start(board_);
    return Result::success();
  }

  return Result::error("Cannot start gyro: unknown implementation");
}

Result MetaWearBoardAdapter::stopGyro() {
  if (gyroImpl_ == GyroImpl::Bmi160) {
    mbl_mw_gyro_bmi160_stop(board_);
    mbl_mw_gyro_bmi160_disable_rotation_sampling(board_);
    return Result::success();
  }

  if (gyroImpl_ == GyroImpl::Bmi270) {
    mbl_mw_gyro_bmi270_stop(board_);
    mbl_mw_gyro_bmi270_disable_rotation_sampling(board_);
    return Result::success();
  }

  return Result::error("Cannot stop gyro: unknown implementation");
}
} // namespace headmotion
