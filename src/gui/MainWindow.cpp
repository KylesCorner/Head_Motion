#include "headmotion/gui/MainWindow.hpp"

#include "headmotion/app/CommandOutput.hpp"
#include "headmotion/app/Commands.hpp"
#include "headmotion/session/BoardStateStore.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace headmotion::gui {

namespace {

struct SampleRateOption {
    float hz;
    const char* label;
};

constexpr std::array SAMPLE_RATES = {
    SampleRateOption{25.0f,  "25 Hz"},
    SampleRateOption{50.0f,  "50 Hz"},
    SampleRateOption{100.0f, "100 Hz"},
    SampleRateOption{200.0f, "200 Hz"}
};

constexpr int DEFAULT_SAMPLE_RATE_INDEX = 1;
constexpr double UI_REFRESH_SECONDS = 0.10;
constexpr const char* DEFAULT_OUTPUT_DIRECTORY = "data/sync";

constexpr int WINDOW_W = 1360;
constexpr int WINDOW_H = 760;

constexpr int MARGIN = 16;
constexpr int TOP_BUTTON_Y = 16;
constexpr int TOP_BUTTON_H = 36;
constexpr int TOP_BUTTON_W = 120;
constexpr int TOP_BUTTON_GAP = 12;

constexpr int FILTER_ROW_Y = 64;
constexpr int FILTER_ROW_H = 32;

constexpr int COUNT_ROW_Y = 106;
constexpr int COUNT_ROW_H = 24;

constexpr int HEADER_Y = 138;
constexpr int HEADER_H = 28;

constexpr int SCROLL_Y = 166;
constexpr int SCROLL_H = 470;

constexpr int PROGRESS_Y = 648;
constexpr int PROGRESS_H = 28;

constexpr int STATUS_Y = 684;
constexpr int STATUS_H = 52;

constexpr int ROW_H = 34;
constexpr int ROW_GAP = 6;

constexpr int COL_DEVICE_ID_X = 16;
constexpr int COL_DEVICE_ID_W = 140;

constexpr int COL_PORT_X = 162;
constexpr int COL_PORT_W = 190;

constexpr int COL_STATUS_X = 358;
constexpr int COL_STATUS_W = 220;

constexpr int COL_PROGRESS_X = 584;
constexpr int COL_PROGRESS_W = 110;

constexpr int COL_ACTIVE_X = 700;
constexpr int COL_ACTIVE_W = 80;

constexpr int COL_ACTIONS_X = 788;
constexpr int ACTION_BUTTON_W = 80;
constexpr int ACTION_BUTTON_GAP = 8;

std::filesystem::path outputDirectoryConfigPath() {
    return
        headmotion::session::BoardStateStore::configRoot() /
        "gui" /
        "last_download_path.txt";
}

std::string loadSavedOutputDirectory() {
    const auto path = outputDirectoryConfigPath();
    std::ifstream in(path);

    if (!in) {
        return DEFAULT_OUTPUT_DIRECTORY;
    }

    std::string directory;
    std::getline(in, directory);

    if (directory.empty()) {
        return DEFAULT_OUTPUT_DIRECTORY;
    }

    return directory;
}

void saveOutputDirectory(
    const std::string& directory
) {
    if (directory.empty()) {
        return;
    }

    const auto path = outputDirectoryConfigPath();

    if (path.has_parent_path()) {
        std::filesystem::create_directories(
            path.parent_path()
        );
    }

    std::ofstream out(
        path,
        std::ios::out | std::ios::trunc
    );

    if (!out) {
        throw std::runtime_error(
            "Failed to open output directory preference file: " +
            path.string()
        );
    }

    out << directory << '\n';

    if (!out) {
        throw std::runtime_error(
            "Failed to save output directory preference: " +
            path.string()
        );
    }
}

const headmotion::app::CommandEvent::Field* findField(
    const headmotion::app::CommandEvent& event,
    const std::string& key
) {
    for (const auto& field : event.fields) {
        if (field.key == key) {
            return &field;
        }
    }

    return nullptr;
}

std::string stringField(
    const headmotion::app::CommandEvent& event,
    const std::string& key,
    std::string fallback = {}
) {
    const auto* field = findField(event, key);

    if (field == nullptr) {
        return fallback;
    }

    if (const auto* value = std::get_if<std::string>(&field->value)) {
        return *value;
    }

    return fallback;
}

std::uint64_t uintField(
    const headmotion::app::CommandEvent& event,
    const std::string& key,
    std::uint64_t fallback = 0
) {
    const auto* field = findField(event, key);

    if (field == nullptr) {
        return fallback;
    }

    if (const auto* value = std::get_if<std::uint64_t>(&field->value)) {
        return *value;
    }

    if (const auto* value = std::get_if<std::int64_t>(&field->value)) {
        if (*value >= 0) {
            return static_cast<std::uint64_t>(*value);
        }
    }

    return fallback;
}

bool boolField(
    const headmotion::app::CommandEvent& event,
    const std::string& key,
    bool fallback = false
) {
    const auto* field = findField(event, key);

    if (field == nullptr) {
        return fallback;
    }

    if (const auto* value = std::get_if<bool>(&field->value)) {
        return *value;
    }

    return fallback;
}

std::string humanizeStage(
    std::string stage
) {
    for (char& ch : stage) {
        if (ch == '_') {
            ch = ' ';
        }
    }

    if (!stage.empty()) {
        stage[0] = static_cast<char>(
            std::toupper(
                static_cast<unsigned char>(stage[0])
            )
        );
    }

    return stage;
}

std::string safePathComponent(
    std::string value
) {
    if (value.empty()) {
        return "unknown-device";
    }

    for (char& ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);

        if (!std::isalnum(uch) && ch != '-' && ch != '_') {
            ch = '_';
        }
    }

    return value;
}

std::string formatPercent(
    double percent
) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << percent << "%";
    return out.str();
}

std::string truncateToPixelWidth(
    const std::string& text,
    int max_width
) {
    if (text.empty() || max_width <= 0) {
        return {};
    }

    if (fl_width(text.c_str()) <= max_width) {
        return text;
    }

    constexpr const char* ELLIPSIS = "...";
    const int ellipsis_width =
        static_cast<int>(
            fl_width(ELLIPSIS)
        );

    if (ellipsis_width >= max_width) {
        return ELLIPSIS;
    }

    std::string result;

    for (char ch : text) {
        const std::string candidate =
            result + ch + ELLIPSIS;

        if (
            fl_width(candidate.c_str()) >
            max_width
        ) {
            break;
        }

        result.push_back(ch);
    }

    result += ELLIPSIS;
    return result;
}

void setCellText(
    Fl_Box* box,
    const std::string& full_text
) {
    if (box == nullptr) {
        return;
    }

    const int inner_width =
        std::max(1, box->w() - 10);

    const std::string clipped =
        truncateToPixelWidth(
            full_text,
            inner_width
        );

    box->copy_label(clipped.c_str());
    box->tooltip(full_text.c_str());
}

std::filesystem::path commandLogPath(
    const std::string& output_directory,
    const std::string& command_name,
    const std::string& device_id,
    const std::string& port
) {
    const std::filesystem::path logs_dir =
        std::filesystem::path(
            output_directory.empty()
                ? DEFAULT_OUTPUT_DIRECTORY
                : output_directory
        ) /
        "logs";

    if (command_name == "scan") {
        return logs_dir / "scan.jsonl";
    }

    const std::string device_token =
        safePathComponent(
            !device_id.empty()
                ? device_id
                : port
        );

    return logs_dir /
        (
            safePathComponent(
                command_name
            ) +
            "_" +
            device_token +
            ".jsonl"
        );
}

std::string makeDeviceKey(
    const std::string& device_id,
    const std::string& port
) {
    if (!device_id.empty()) {
        return device_id;
    }

    return port;
}

} // namespace

MainWindow::MainWindow() {
    window_ = new Fl_Double_Window(
        WINDOW_W,
        WINDOW_H,
        "HeadMotion MMS+"
    );

    buildUi();
    window_->end();
    wireCallbacks();

    Fl::add_timeout(
        UI_REFRESH_SECONDS,
        timerCallback,
        this
    );
}

MainWindow::~MainWindow() {
    try {
        if (output_input_ != nullptr) {
            saveOutputDirectory(output_input_->value());
        }
    }
    catch (...) {
    }

    Fl::remove_timeout(
        timerCallback,
        this
    );

    if (scan_worker_.joinable()) {
        scan_worker_.join();
    }

    const auto devices = deviceSnapshot();
    for (const auto& device : devices) {
        if (device->worker.joinable()) {
            device->worker.join();
        }
    }

    delete window_;
}

void MainWindow::show(
    int argc,
    char** argv
) {
    window_->show(argc, argv);
}

void MainWindow::buildUi() {
    scan_button_ = new Fl_Button(
        MARGIN,
        TOP_BUTTON_Y,
        TOP_BUTTON_W,
        TOP_BUTTON_H,
        "Scan"
    );

    reset_all_button_ = new Fl_Button(
        MARGIN + (TOP_BUTTON_W + TOP_BUTTON_GAP) * 1,
        TOP_BUTTON_Y,
        TOP_BUTTON_W,
        TOP_BUTTON_H,
        "Reset All"
    );

    start_all_button_ = new Fl_Button(
        MARGIN + (TOP_BUTTON_W + TOP_BUTTON_GAP) * 2,
        TOP_BUTTON_Y,
        150,
        TOP_BUTTON_H,
        "Record Start All"
    );

    stop_all_button_ = new Fl_Button(
        MARGIN + (TOP_BUTTON_W + TOP_BUTTON_GAP) * 2 + 150 + TOP_BUTTON_GAP,
        TOP_BUTTON_Y,
        140,
        TOP_BUTTON_H,
        "Record Stop All"
    );

    sync_all_button_ = new Fl_Button(
        WINDOW_W - MARGIN - 140,
        TOP_BUTTON_Y,
        140,
        TOP_BUTTON_H,
        "Sync All"
    );

    auto* sample_rate_label = new Fl_Box(
        MARGIN,
        FILTER_ROW_Y,
        100,
        FILTER_ROW_H,
        "Sample rate:"
    );
    sample_rate_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    sample_rate_choice_ = new Fl_Choice(
        MARGIN + 100,
        FILTER_ROW_Y,
        120,
        FILTER_ROW_H
    );

    for (const auto& rate : SAMPLE_RATES) {
        sample_rate_choice_->add(rate.label);
    }

    sample_rate_choice_->value(DEFAULT_SAMPLE_RATE_INDEX);

    auto* output_label = new Fl_Box(
        280,
        FILTER_ROW_Y,
        90,
        FILTER_ROW_H,
        "Output path:"
    );
    output_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    output_input_ = new Fl_Input(
        370,
        FILTER_ROW_Y,
        WINDOW_W - 370 - 110 - 110 - MARGIN - 12,
        FILTER_ROW_H
    );

    output_input_->value(loadSavedOutputDirectory().c_str());

    browse_button_ = new Fl_Button(
        output_input_->x() + output_input_->w() + 12,
        FILTER_ROW_Y,
        98,
        FILTER_ROW_H,
        "Browse..."
    );

    device_count_box_ = new Fl_Box(
        MARGIN,
        COUNT_ROW_Y,
        WINDOW_W - MARGIN * 2,
        COUNT_ROW_H,
        "Devices: 0"
    );
    device_count_box_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    header_device_id_ = new Fl_Box(
        COL_DEVICE_ID_X,
        HEADER_Y,
        COL_DEVICE_ID_W,
        HEADER_H,
        "Device ID"
    );
    header_device_id_->box(FL_THIN_UP_BOX);
    header_device_id_->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);

    header_port_ = new Fl_Box(
        COL_PORT_X,
        HEADER_Y,
        COL_PORT_W,
        HEADER_H,
        "COM / TTY"
    );
    header_port_->box(FL_THIN_UP_BOX);
    header_port_->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);

    header_status_ = new Fl_Box(
        COL_STATUS_X,
        HEADER_Y,
        COL_STATUS_W,
        HEADER_H,
        "Status"
    );
    header_status_->box(FL_THIN_UP_BOX);
    header_status_->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);

    header_progress_ = new Fl_Box(
        COL_PROGRESS_X,
        HEADER_Y,
        COL_PROGRESS_W,
        HEADER_H,
        "Progress"
    );
    header_progress_->box(FL_THIN_UP_BOX);
    header_progress_->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);

    header_active_ = new Fl_Box(
        COL_ACTIVE_X,
        HEADER_Y,
        COL_ACTIVE_W,
        HEADER_H,
        "Active"
    );
    header_active_->box(FL_THIN_UP_BOX);
    header_active_->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);

    header_actions_ = new Fl_Box(
        COL_ACTIONS_X,
        HEADER_Y,
        WINDOW_W - COL_ACTIONS_X - MARGIN,
        HEADER_H,
        "Individual Commands"
    );
    header_actions_->box(FL_THIN_UP_BOX);
    header_actions_->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);

    device_scroll_ = new Fl_Scroll(
        MARGIN,
        SCROLL_Y,
        WINDOW_W - MARGIN * 2,
        SCROLL_H
    );
    device_scroll_->box(FL_DOWN_FRAME);
    device_scroll_->type(Fl_Scroll::VERTICAL_ALWAYS);
    device_scroll_->end();

    progress_ = new Fl_Progress(
        MARGIN,
        PROGRESS_Y,
        WINDOW_W - MARGIN * 2,
        PROGRESS_H
    );
    progress_->minimum(0.0);
    progress_->maximum(100.0);
    progress_->value(0.0);
    progress_->copy_label("0%");

    status_box_ = new Fl_Box(
        MARGIN,
        STATUS_Y,
        WINDOW_W - MARGIN * 2,
        STATUS_H,
        "Ready"
    );
    status_box_->box(FL_DOWN_BOX);
    status_box_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
}

void MainWindow::wireCallbacks() {
    scan_button_->callback(
        [](Fl_Widget*, void* ctx) {
            static_cast<MainWindow*>(ctx)->runScan();
        },
        this
    );

    reset_all_button_->callback(
        [](Fl_Widget*, void* ctx) {
            static_cast<MainWindow*>(ctx)->runResetAll();
        },
        this
    );

    start_all_button_->callback(
        [](Fl_Widget*, void* ctx) {
            static_cast<MainWindow*>(ctx)->runStartAll();
        },
        this
    );

    stop_all_button_->callback(
        [](Fl_Widget*, void* ctx) {
            static_cast<MainWindow*>(ctx)->runStopAll();
        },
        this
    );

    sync_all_button_->callback(
        [](Fl_Widget*, void* ctx) {
            static_cast<MainWindow*>(ctx)->runSyncAll();
        },
        this
    );

    browse_button_->callback(
        [](Fl_Widget*, void* ctx) {
            static_cast<MainWindow*>(ctx)->chooseOutputDirectory();
        },
        this
    );
}

void MainWindow::setStatus(
    std::string status
) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_text_ = std::move(status);
}

MainWindow::DevicePtr MainWindow::findDevice(
    const std::string& device_key
) const {
    std::lock_guard<std::mutex> lock(devices_mutex_);

    const auto it = devices_.find(device_key);
    if (it == devices_.end()) {
        return nullptr;
    }

    return it->second;
}

std::vector<MainWindow::DevicePtr> MainWindow::deviceSnapshot() const {
    std::lock_guard<std::mutex> lock(devices_mutex_);

    std::vector<DevicePtr> result;
    result.reserve(devices_.size());

    for (const auto& [key, device] : devices_) {
        (void)key;
        result.push_back(device);
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const DevicePtr& a, const DevicePtr& b) {
            const std::string a_name =
                !a->device_id.empty() ? a->device_id : a->port;
            const std::string b_name =
                !b->device_id.empty() ? b->device_id : b->port;
            return a_name < b_name;
        }
    );

    return result;
}

std::vector<MainWindow::DevicePtr> MainWindow::idleDeviceSnapshot() const {
    const auto devices = deviceSnapshot();

    std::vector<DevicePtr> idle;
    for (const auto& device : devices) {
        if (!device->busy.load()) {
            idle.push_back(device);
        }
    }

    return idle;
}

std::size_t MainWindow::busyDeviceCount() const {
    const auto devices = deviceSnapshot();

    std::size_t count = 0;
    for (const auto& device : devices) {
        if (device->busy.load()) {
            ++count;
        }
    }

    return count;
}

std::size_t MainWindow::idleDeviceCount() const {
    return idleDeviceSnapshot().size();
}

bool MainWindow::anyDeviceBusy() const {
    return busyDeviceCount() != 0;
}

void MainWindow::clearDeviceRows() {
    for (auto& row : device_rows_) {
        delete row->device_id_box;
        delete row->port_box;
        delete row->status_box;
        delete row->progress_box;
        delete row->active_box;
        delete row->reset_button;
        delete row->start_button;
        delete row->stop_button;
        delete row->sync_button;
    }

    device_rows_.clear();
}

void MainWindow::rebuildDeviceRows() {
    clearDeviceRows();

    const auto devices = deviceSnapshot();

    device_scroll_->begin();

    const int base_x = device_scroll_->x();
    const int base_y = device_scroll_->y();
    int y = base_y + 8;

    for (const auto& device : devices) {
        auto row = std::make_unique<DeviceRow>();
        row->owner = this;
        row->device_key = device->key;

        row->device_id_box = new Fl_Box(
            base_x + COL_DEVICE_ID_X,
            y,
            COL_DEVICE_ID_W,
            ROW_H,
            ""
        );
        row->device_id_box->box(FL_THIN_DOWN_BOX);
        row->device_id_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        row->port_box = new Fl_Box(
            base_x + COL_PORT_X,
            y,
            COL_PORT_W,
            ROW_H,
            ""
        );
        row->port_box->box(FL_THIN_DOWN_BOX);
        row->port_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        row->status_box = new Fl_Box(
            base_x + COL_STATUS_X,
            y,
            COL_STATUS_W,
            ROW_H,
            ""
        );
        row->status_box->box(FL_THIN_DOWN_BOX);
        row->status_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        row->progress_box = new Fl_Box(
            base_x + COL_PROGRESS_X,
            y,
            COL_PROGRESS_W,
            ROW_H,
            ""
        );
        row->progress_box->box(FL_THIN_DOWN_BOX);
        row->progress_box->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);

        row->active_box = new Fl_Box(
            base_x + COL_ACTIVE_X,
            y,
            COL_ACTIVE_W,
            ROW_H,
            ""
        );
        row->active_box->box(FL_THIN_DOWN_BOX);
        row->active_box->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);

        int button_x = base_x + COL_ACTIONS_X;

        row->reset_button = new Fl_Button(
            button_x,
            y,
            ACTION_BUTTON_W,
            ROW_H,
            "Reset"
        );
        button_x += ACTION_BUTTON_W + ACTION_BUTTON_GAP;

        row->start_button = new Fl_Button(
            button_x,
            y,
            ACTION_BUTTON_W,
            ROW_H,
            "Start"
        );
        button_x += ACTION_BUTTON_W + ACTION_BUTTON_GAP;

        row->stop_button = new Fl_Button(
            button_x,
            y,
            ACTION_BUTTON_W,
            ROW_H,
            "Stop"
        );
        button_x += ACTION_BUTTON_W + ACTION_BUTTON_GAP;

        row->sync_button = new Fl_Button(
            button_x,
            y,
            ACTION_BUTTON_W,
            ROW_H,
            "Sync"
        );

        row->reset_button->callback(
            [](Fl_Widget*, void* data) {
                auto* row = static_cast<DeviceRow*>(data);
                row->owner->runResetOne(row->device_key);
            },
            row.get()
        );

        row->start_button->callback(
            [](Fl_Widget*, void* data) {
                auto* row = static_cast<DeviceRow*>(data);
                row->owner->runStartOne(row->device_key);
            },
            row.get()
        );

        row->stop_button->callback(
            [](Fl_Widget*, void* data) {
                auto* row = static_cast<DeviceRow*>(data);
                row->owner->runStopOne(row->device_key);
            },
            row.get()
        );

        row->sync_button->callback(
            [](Fl_Widget*, void* data) {
                auto* row = static_cast<DeviceRow*>(data);
                row->owner->runSyncOne(row->device_key);
            },
            row.get()
        );

        device_rows_.push_back(std::move(row));
        y += ROW_H + ROW_GAP;
    }

    device_scroll_->end();
    device_scroll_->redraw();
}

void MainWindow::runScan() {
    if (scan_busy_.exchange(true)) {
        setStatus("A device scan is already running");
        return;
    }

    if (anyDeviceBusy()) {
        scan_busy_ = false;
        setStatus("Wait for active device operations to finish before scanning");
        return;
    }

    if (scan_worker_.joinable()) {
        scan_worker_.join();
    }

    setStatus("Scanning for MMS+ devices...");

    const std::string log_output_directory =
        outputDirectory();

    scan_worker_ = std::jthread(
        [
            this,
            log_output_directory
        ] {
            struct DiscoveredDevice {
                std::string device_id;
                std::string port;
                std::string identity;
            };

            std::vector<DiscoveredDevice> discovered;

            try {
                auto gui_sink =
                    [
                        this,
                        &discovered
                    ](
                        const headmotion::app::
                            CommandEvent& event
                    ) {
                        if (
                            event.type ==
                            "device"
                        ) {
                            DiscoveredDevice device;
                            device.device_id =
                                stringField(
                                    event,
                                    "device_id"
                                );
                            device.port =
                                stringField(
                                    event,
                                    "port"
                                );
                            device.identity =
                                stringField(
                                    event,
                                    "identity"
                                );

                            if (
                                !device.port.empty()
                            ) {
                                discovered.push_back(
                                    std::move(device)
                                );
                            }

                            return;
                        }

                        if (
                            event.type ==
                            "error"
                        ) {
                            setStatus(
                                stringField(
                                    event,
                                    "message",
                                    "Device scan failed"
                                )
                            );
                        }
                    };

                const auto log_path =
                    commandLogPath(
                        log_output_directory,
                        "scan",
                        {},
                        {}
                    );

                auto file_sink =
                    headmotion::app::
                        CommandOutput::
                        jsonFileSink(
                            log_path
                        );

                headmotion::app::
                    CommandOutput output(
                        "scan",
                        headmotion::app::
                            CommandOutput::
                            combineSinks(
                                {
                                    std::move(
                                        gui_sink
                                    ),
                                    std::move(
                                        file_sink
                                    )
                                }
                            )
                    );

                const int result =
                    headmotion::app::runScanPortsCommand(output);

                if (result == 0) {
                    std::unordered_map<std::string, DevicePtr> next_devices;

                    for (auto& found : discovered) {
                        auto device = std::make_shared<DeviceOperation>();
                        device->device_id = std::move(found.device_id);
                        device->port = std::move(found.port);
                        device->identity = std::move(found.identity);
                        device->key = makeDeviceKey(device->device_id, device->port);

                        next_devices[device->key] = device;
                    }

                    const auto count = next_devices.size();

                    {
                        std::lock_guard<std::mutex> lock(devices_mutex_);
                        devices_ = std::move(next_devices);
                    }

                    rows_dirty_ = true;

                    setStatus(
                        "Scan complete: found " +
                        std::to_string(count) +
                        " MMS+ device" +
                        (count == 1 ? "" : "s")
                    );
                }
                else {
                    setStatus(
                        "Device scan failed with error code " +
                        std::to_string(result)
                    );
                }
            }
            catch (const std::exception& error) {
                setStatus(
                    std::string("Device scan failed: ") +
                    error.what()
                );
            }
            catch (...) {
                setStatus("Device scan failed with an unknown error");
            }

            scan_busy_ = false;
        }
    );
}

void MainWindow::launchDevices(
    std::vector<DevicePtr> devices,
    std::string command_name,
    std::string display_name,
    DeviceCommand operation
) {
    if (devices.empty()) {
        setStatus("No idle devices are available for " + display_name);
        return;
    }

    operation_failed_ = false;

    setStatus(
        display_name +
        " launched on " +
        std::to_string(devices.size()) +
        " device" +
        (devices.size() == 1 ? "" : "s")
    );

    for (const auto& device : devices) {
        launchDeviceOperation(
            device,
            command_name,
            display_name,
            operation
        );
    }
}

void MainWindow::launchDeviceOperation(
    const DevicePtr& device,
    std::string command_name,
    std::string display_name,
    DeviceCommand operation
) {
    if (device == nullptr) {
        return;
    }

    if (device->busy.exchange(true)) {
        return;
    }

    if (device->worker.joinable()) {
        device->worker.join();
    }

    device->entries_left = 0;
    device->total_entries = 0;

    const std::string log_output_directory =
        outputDirectory();

    {
        std::lock_guard<std::mutex> lock(device->state_mutex);
        device->status = display_name + " queued";
    }

    device->worker = std::jthread(
        [this,
         device,
         log_output_directory,
         command_name = std::move(command_name),
         display_name = std::move(display_name),
         operation = std::move(operation)]() mutable {
            try {
                auto gui_sink =
                    [
                        this,
                        device
                    ](
                        const headmotion::app::
                            CommandEvent& event
                    ) {
                        handleDeviceEvent(
                            device,
                            event
                        );
                    };

                const auto log_path =
                    commandLogPath(
                        log_output_directory,
                        command_name,
                        device->device_id,
                        device->port
                    );

                auto file_sink =
                    headmotion::app::
                        CommandOutput::
                        jsonFileSink(
                            log_path
                        );

                headmotion::app::
                    CommandOutput output(
                        command_name,
                        headmotion::app::
                            CommandOutput::
                            combineSinks(
                                {
                                    std::move(
                                        gui_sink
                                    ),
                                    std::move(
                                        file_sink
                                    )
                                }
                            )
                    );

                const int result =
                    operation(
                        device,
                        output
                    );

                if (result != 0) {
                    operation_failed_ = true;

                    std::lock_guard<std::mutex> lock(device->state_mutex);
                    device->status =
                        display_name +
                        " failed (" +
                        std::to_string(result) +
                        ")";
                }
            }
            catch (const std::exception& error) {
                operation_failed_ = true;

                {
                    std::lock_guard<std::mutex> lock(device->state_mutex);
                    device->status =
                        display_name +
                        " failed: " +
                        error.what();
                }

                setStatus(
                    (device->device_id.empty() ? device->port : device->device_id) +
                    ": " +
                    error.what()
                );
            }
            catch (...) {
                operation_failed_ = true;

                {
                    std::lock_guard<std::mutex> lock(device->state_mutex);
                    device->status = display_name + " failed";
                }

                setStatus(
                    (device->device_id.empty() ? device->port : device->device_id) +
                    ": operation failed"
                );
            }

            device->busy = false;

            if (busyDeviceCount() == 0) {
                if (operation_failed_.load()) {
                    setStatus(display_name + " finished with errors");
                }
                else {
                    setStatus(display_name + " complete");
                }
            }
        }
    );
}

void MainWindow::handleDeviceEvent(
    const DevicePtr& device,
    const headmotion::app::CommandEvent& event
) {
    if (device == nullptr) {
        return;
    }

    if (event.type == "started") {
        std::lock_guard<std::mutex> lock(device->state_mutex);
        device->status = "Started";
        return;
    }

    if (event.type == "progress") {
        device->entries_left =
            static_cast<std::uint32_t>(uintField(event, "entries_left"));
        device->total_entries =
            static_cast<std::uint32_t>(uintField(event, "entries_total"));
        return;
    }

    if (event.type == "status") {
        const std::string stage = stringField(event, "stage");

        if (!stage.empty()) {
            std::lock_guard<std::mutex> lock(device->state_mutex);
            device->status = humanizeStage(stage);
        }

        return;
    }

    if (event.type == "output") {
        const std::string path = stringField(event, "path");
        if (!path.empty()) {
            std::lock_guard<std::mutex> lock(device->state_mutex);
            device->status = "Wrote output";
        }

        return;
    }

    if (event.type == "error") {
        const std::string message =
            stringField(event, "message", "Command failed");

        {
            std::lock_guard<std::mutex> lock(device->state_mutex);
            device->status = message;
        }

        operation_failed_ = true;

        setStatus(
            (device->device_id.empty() ? device->port : device->device_id) +
            ": " +
            message
        );

        return;
    }

    if (event.type == "completed") {
        const bool success = boolField(event, "success", false);
        const std::uint64_t exit_code = uintField(event, "exit_code");

        std::lock_guard<std::mutex> lock(device->state_mutex);

        if (success) {
            device->status = "Complete";
        }
        else {
            operation_failed_ = true;
            device->status =
                "Failed (" +
                std::to_string(exit_code) +
                ")";
        }

        return;
    }
}

void MainWindow::runResetAll() {
    if (scan_busy_.load()) {
        setStatus("Wait for the device scan to finish");
        return;
    }

    launchDevices(
        idleDeviceSnapshot(),
        "record-reset",
        "Reset",
        [](const DevicePtr& current, headmotion::app::CommandOutput& output) {
            return headmotion::app::runRecordResetCommand(
                current->port,
                output
            );
        }
    );
}

void MainWindow::runStartAll() {
    if (scan_busy_.load()) {
        setStatus("Wait for the device scan to finish");
        return;
    }

    const float sample_rate = selectedSampleRate();

    launchDevices(
        idleDeviceSnapshot(),
        "record-start",
        "Start recording",
        [sample_rate](const DevicePtr& current, headmotion::app::CommandOutput& output) {
            return headmotion::app::runRecordStartCommand(
                current->port,
                sample_rate,
                0,
                output
            );
        }
    );
}

void MainWindow::runStopAll() {
    if (scan_busy_.load()) {
        setStatus("Wait for the device scan to finish");
        return;
    }

    launchDevices(
        idleDeviceSnapshot(),
        "record-stop",
        "Stop recording",
        [](const DevicePtr& current, headmotion::app::CommandOutput& output) {
            return headmotion::app::runRecordStopCommand(
                current->port,
                output
            );
        }
    );
}

void MainWindow::runSyncAll() {
    if (scan_busy_.load()) {
        setStatus("Wait for the device scan to finish");
        return;
    }

    const std::string base_output = outputDirectory();

    if (base_output.empty()) {
        setStatus("Choose an output directory first");
        return;
    }

    try {
        saveOutputDirectory(base_output);
        std::filesystem::create_directories(base_output);
    }
    catch (const std::exception& error) {
        setStatus(
            std::string("Could not prepare output directory: ") +
            error.what()
        );
        return;
    }

    launchDevices(
        idleDeviceSnapshot(),
        "sync",
        "Sync",
        [base_output](const DevicePtr& current, headmotion::app::CommandOutput& output) {
            return headmotion::app::runSyncCommand(
                current->port,
                base_output,
                false,
                output
            );
        }
    );
}

void MainWindow::runResetOne(
    const std::string& device_key
) {
    if (scan_busy_.load()) {
        setStatus("Wait for the device scan to finish");
        return;
    }

    const auto device = findDevice(device_key);

    if (device == nullptr) {
        setStatus("Selected device is no longer available");
        return;
    }

    launchDeviceOperation(
        device,
        "record-reset",
        "Reset",
        [](const DevicePtr& current, headmotion::app::CommandOutput& output) {
            return headmotion::app::runRecordResetCommand(
                current->port,
                output
            );
        }
    );
}

void MainWindow::runStartOne(
    const std::string& device_key
) {
    if (scan_busy_.load()) {
        setStatus("Wait for the device scan to finish");
        return;
    }

    const auto device = findDevice(device_key);

    if (device == nullptr) {
        setStatus("Selected device is no longer available");
        return;
    }

    const float sample_rate = selectedSampleRate();

    launchDeviceOperation(
        device,
        "record-start",
        "Start recording",
        [sample_rate](const DevicePtr& current, headmotion::app::CommandOutput& output) {
            return headmotion::app::runRecordStartCommand(
                current->port,
                sample_rate,
                0,
                output
            );
        }
    );
}

void MainWindow::runStopOne(
    const std::string& device_key
) {
    if (scan_busy_.load()) {
        setStatus("Wait for the device scan to finish");
        return;
    }

    const auto device = findDevice(device_key);

    if (device == nullptr) {
        setStatus("Selected device is no longer available");
        return;
    }

    launchDeviceOperation(
        device,
        "record-stop",
        "Stop recording",
        [](const DevicePtr& current, headmotion::app::CommandOutput& output) {
            return headmotion::app::runRecordStopCommand(
                current->port,
                output
            );
        }
    );
}

void MainWindow::runSyncOne(
    const std::string& device_key
) {
    if (scan_busy_.load()) {
        setStatus("Wait for the device scan to finish");
        return;
    }

    const auto device = findDevice(device_key);

    if (device == nullptr) {
        setStatus("Selected device is no longer available");
        return;
    }

    const std::string base_output = outputDirectory();

    if (base_output.empty()) {
        setStatus("Choose an output directory first");
        return;
    }

    try {
        saveOutputDirectory(base_output);
        std::filesystem::create_directories(base_output);
    }
    catch (const std::exception& error) {
        setStatus(
            std::string("Could not prepare output directory: ") +
            error.what()
        );
        return;
    }

    launchDeviceOperation(
        device,
        "sync",
        "Sync",
        [base_output](const DevicePtr& current, headmotion::app::CommandOutput& output) {
            return headmotion::app::runSyncCommand(
                current->port,
                base_output,
                false,
                output
            );
        }
    );
}

void MainWindow::chooseOutputDirectory() {
    Fl_Native_File_Chooser chooser;

    chooser.title("Choose HeadMotion Output Directory");
    chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);

    const std::string current = outputDirectory();
    if (!current.empty()) {
        chooser.directory(current.c_str());
    }

    const int result = chooser.show();

    if (result == 0) {
        const std::string selected = chooser.filename();
        output_input_->value(selected.c_str());

        try {
            saveOutputDirectory(selected);
        }
        catch (const std::exception& error) {
            setStatus(
                std::string("Selected directory, but failed to save preference: ") +
                error.what()
            );
        }

        return;
    }

    if (result == -1) {
        setStatus(
            std::string("Directory chooser failed: ") +
            chooser.errmsg()
        );
    }
}

std::string MainWindow::outputDirectory() const {
    if (output_input_ == nullptr) {
        return {};
    }

    return output_input_->value();
}

float MainWindow::selectedSampleRate() const {
    const int index = sample_rate_choice_->value();

    if (index < 0 ||
        static_cast<std::size_t>(index) >= SAMPLE_RATES.size()) {
        return SAMPLE_RATES[DEFAULT_SAMPLE_RATE_INDEX].hz;
    }

    return SAMPLE_RATES[static_cast<std::size_t>(index)].hz;
}

void MainWindow::updateDeviceRows() {
    const auto devices = deviceSnapshot();

    for (auto& row : device_rows_) {
        const auto device = findDevice(row->device_key);

        if (device == nullptr) {
            setCellText(row->device_id_box, "Unavailable");
            setCellText(row->port_box, "-");
            setCellText(row->status_box, "Unavailable");
            setCellText(row->progress_box, "-");
            setCellText(row->active_box, "No");
            row->reset_button->deactivate();
            row->start_button->deactivate();
            row->stop_button->deactivate();
            row->sync_button->deactivate();
            continue;
        }

        const std::string device_name =
            !device->device_id.empty() ? device->device_id : "(unavailable)";

        setCellText(row->device_id_box, device_name);
        setCellText(row->port_box, device->port);

        std::string status;
        {
            std::lock_guard<std::mutex> lock(device->state_mutex);
            status = device->status;
        }

        setCellText(row->status_box, status);

        const std::uint32_t total = device->total_entries.load();
        const std::uint32_t left = device->entries_left.load();

        if (total > 0) {
            const std::uint32_t done = total >= left ? total - left : 0;
            const double percent =
                100.0 *
                static_cast<double>(done) /
                static_cast<double>(total);

            setCellText(row->progress_box, formatPercent(percent));
        }
        else {
            setCellText(row->progress_box, "-");
        }

        const bool busy = device->busy.load();
        setCellText(row->active_box, busy ? "Yes" : "No");

        const bool enable_row_actions = !scan_busy_.load() && !busy;

        if (enable_row_actions) {
            row->reset_button->activate();
            row->start_button->activate();
            row->stop_button->activate();
            row->sync_button->activate();
        }
        else {
            row->reset_button->deactivate();
            row->start_button->deactivate();
            row->stop_button->deactivate();
            row->sync_button->deactivate();
        }
    }
}

void MainWindow::updateProgressUi() {
    const auto devices = deviceSnapshot();

    std::uint64_t total = 0;
    std::uint64_t left = 0;
    std::size_t active_progress_devices = 0;

    for (const auto& device : devices) {
        const std::uint32_t device_total = device->total_entries.load();
        const std::uint32_t device_left = device->entries_left.load();

        if (device_total == 0) {
            continue;
        }

        total += device_total;
        left += std::min(device_left, device_total);
        ++active_progress_devices;
    }

    if (total == 0) {
        progress_->value(0.0);

        if (busyDeviceCount() > 0) {
            const std::string label =
                "Waiting for progress... (" +
                std::to_string(busyDeviceCount()) +
                " active)";
            progress_->copy_label(label.c_str());
        }
        else {
            progress_->copy_label("0%");
        }

        return;
    }

    const std::uint64_t done = total >= left ? total - left : 0;
    const double percent =
        100.0 *
        static_cast<double>(done) /
        static_cast<double>(total);

    progress_->value(percent);

    std::ostringstream label;
    label
        << std::fixed
        << std::setprecision(1)
        << percent
        << "%  ("
        << done
        << " / "
        << total
        << ", "
        << active_progress_devices
        << " device";

    if (active_progress_devices != 1) {
        label << "s";
    }

    label << ")";

    progress_->copy_label(label.str().c_str());
}

void MainWindow::setControlsEnabled() {
    const std::size_t total_devices = deviceSnapshot().size();
    const std::size_t idle_devices = idleDeviceCount();
    const bool scan_enabled = !scan_busy_.load() && !anyDeviceBusy();
    const bool actions_enabled = !scan_busy_.load() && idle_devices > 0;

    if (scan_enabled) {
        scan_button_->activate();
    }
    else {
        scan_button_->deactivate();
    }

    if (actions_enabled) {
        reset_all_button_->activate();
        start_all_button_->activate();
        stop_all_button_->activate();
        sync_all_button_->activate();
    }
    else {
        reset_all_button_->deactivate();
        start_all_button_->deactivate();
        stop_all_button_->deactivate();
        sync_all_button_->deactivate();
    }

    if (!scan_busy_.load()) {
        sample_rate_choice_->activate();
        browse_button_->activate();
        output_input_->activate();
    }
    else {
        sample_rate_choice_->deactivate();
        browse_button_->deactivate();
        output_input_->deactivate();
    }

    std::ostringstream devices_label;
    devices_label
        << "Devices: "
        << total_devices
        << " | Idle: "
        << idle_devices
        << " | Busy: "
        << busyDeviceCount();

    device_count_box_->copy_label(devices_label.str().c_str());
}

void MainWindow::updateUi() {
    if (rows_dirty_.exchange(false)) {
        rebuildDeviceRows();
    }

    std::string status;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        status = status_text_;
    }

    status_box_->copy_label(status.c_str());

    setControlsEnabled();
    updateDeviceRows();
    updateProgressUi();
}

void MainWindow::timerCallback(
    void* context
) {
    auto* self = static_cast<MainWindow*>(context);

    self->updateUi();

    Fl::repeat_timeout(
        UI_REFRESH_SECONDS,
        timerCallback,
        context
    );
}

} // namespace headmotion::gui
