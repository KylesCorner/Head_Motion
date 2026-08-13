#include "headmotion/gui/MainWindow.hpp"

#include "headmotion/app/Commands.hpp"
#include "headmotion/app/DevicePortResolver.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Widget.H>

#include <array>
#include <exception>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>

namespace headmotion::gui {

namespace {

struct SampleRateOption {
    float hz;
    const char* label;
};

constexpr std::array SAMPLE_RATES = {
    SampleRateOption{25.0f,   "25 Hz"},
    SampleRateOption{50.0f,   "50 Hz"},
    SampleRateOption{100.0f,  "100 Hz"},
    SampleRateOption{200.0f,  "200 Hz"},
    SampleRateOption{400.0f,  "400 Hz"},
    SampleRateOption{800.0f,  "800 Hz"},
    SampleRateOption{1600.0f, "1600 Hz"},
    SampleRateOption{3200.0f, "3200 Hz"}
};

constexpr int DEFAULT_SAMPLE_RATE_INDEX = 1;
constexpr double UI_REFRESH_SECONDS = 0.1;

} // namespace

MainWindow::MainWindow() {
    window_ = new Fl_Double_Window(
        620,
        390,
        "HeadMotion MMS+"
    );

    buildUi();

    window_->end();

    wireCallbacks();

    // Worker threads only update thread-safe state.
    // FLTK widgets are refreshed here on the GUI thread.
    Fl::add_timeout(
        UI_REFRESH_SECONDS,
        timerCallback,
        this
    );

    refreshDevice();
}

MainWindow::~MainWindow() {
    Fl::remove_timeout(
        timerCallback,
        this
    );

    // A running backend operation may still reference this object.
    if (worker_.joinable()) {
        worker_.join();
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
    auto makeLabel = [](
        int x,
        int y,
        int width,
        int height,
        const char* text
    ) {
        auto* label = new Fl_Box(
            x,
            y,
            width,
            height,
            text
        );

        label->align(
            FL_ALIGN_LEFT |
            FL_ALIGN_INSIDE
        );

        return label;
    };

    device_box_ = new Fl_Box(
        20,
        20,
        430,
        35,
        "Device: not selected"
    );

    device_box_->align(
        FL_ALIGN_LEFT |
        FL_ALIGN_INSIDE
    );

    scan_button_ = new Fl_Button(
        470,
        20,
        130,
        35,
        "Scan Device"
    );

    makeLabel(
        20,
        75,
        100,
        30,
        "Sample rate:"
    );

    sample_rate_choice_ = new Fl_Choice(
        120,
        75,
        150,
        30
    );

    for (const auto& rate : SAMPLE_RATES) {
        sample_rate_choice_->add(
            rate.label
        );
    }

    sample_rate_choice_->value(
        DEFAULT_SAMPLE_RATE_INDEX
    );

    reset_button_ = new Fl_Button(
        20,
        125,
        175,
        40,
        "Reset Loggers"
    );

    start_button_ = new Fl_Button(
        215,
        125,
        175,
        40,
        "Start Recording"
    );

    stop_button_ = new Fl_Button(
        410,
        125,
        190,
        40,
        "Stop Recording"
    );

    makeLabel(
        20,
        190,
        100,
        30,
        "Output:"
    );

    output_input_ = new Fl_Input(
        120,
        190,
        340,
        30
    );

    output_input_->value(
        "data/sync"
    );

    browse_button_ = new Fl_Button(
        480,
        190,
        120,
        30,
        "Browse..."
    );

    download_button_ = new Fl_Button(
        20,
        240,
        580,
        40,
        "Download Recording"
    );

    progress_ = new Fl_Progress(
        20,
        300,
        580,
        30
    );

    progress_->minimum(0.0);
    progress_->maximum(100.0);
    progress_->value(0.0);
    progress_->copy_label("0%");

    status_box_ = new Fl_Box(
        20,
        340,
        580,
        35,
        "Ready"
    );

    status_box_->align(
        FL_ALIGN_LEFT |
        FL_ALIGN_INSIDE |
        FL_ALIGN_WRAP
    );
}

void MainWindow::wireCallbacks() {
    scan_button_->callback(
        [](Fl_Widget*, void* context) {
            static_cast<MainWindow*>(context)->
                runScan();
        },
        this
    );

    reset_button_->callback(
        [](Fl_Widget*, void* context) {
            static_cast<MainWindow*>(context)->
                runReset();
        },
        this
    );

    start_button_->callback(
        [](Fl_Widget*, void* context) {
            static_cast<MainWindow*>(context)->
                runStart();
        },
        this
    );

    stop_button_->callback(
        [](Fl_Widget*, void* context) {
            static_cast<MainWindow*>(context)->
                runStop();
        },
        this
    );

    browse_button_->callback(
        [](Fl_Widget*, void* context) {
            static_cast<MainWindow*>(context)->
                chooseOutputDirectory();
        },
        this
    );

    download_button_->callback(
        [](Fl_Widget*, void* context) {
            static_cast<MainWindow*>(context)->
                runDownload();
        },
        this
    );
}

void MainWindow::setStatus(
    std::string status
) {
    std::lock_guard<std::mutex> lock(
        state_mutex_
    );

    status_text_ = std::move(status);
}

void MainWindow::setDevice(
    std::string device
) {
    std::lock_guard<std::mutex> lock(
        state_mutex_
    );

    device_text_ = std::move(device);
}

void MainWindow::refreshDevice() {
    try {
        const auto port =
            headmotion::app::resolveDevicePort(
                std::nullopt
            );

        setDevice(
            "Device: " + port
        );
    } catch (const std::exception&) {
        setDevice(
            "Device: not connected"
        );
    }
}

void MainWindow::resetProgress() {
    entries_left_ = 0;
    total_entries_ = 0;
}

void MainWindow::runScan() {
    resetProgress();

    launchOperation(
        "Scanning for MMS+",
        [this] {
            const int result =
                headmotion::app::
                    runScanPortsCommand();

            if (result == 0) {
                refreshDevice();
            }

            return result;
        }
    );
}

void MainWindow::runReset() {
    resetProgress();

    launchOperation(
        "Resetting loggers",
        [] {
            const auto port =
                headmotion::app::resolveDevicePort(
                    std::nullopt
                );

            return headmotion::app::
                runRecordResetCommand(port);
        }
    );
}

void MainWindow::runStart() {
    resetProgress();

    const float sample_rate =
        selectedSampleRate();

    launchOperation(
        "Starting recording",
        [sample_rate] {
            const auto port =
                headmotion::app::resolveDevicePort(
                    std::nullopt
                );

            return headmotion::app::
                runRecordStartCommand(
                    port,
                    sample_rate,
                    0
                );
        }
    );
}

void MainWindow::runStop() {
    resetProgress();

    launchOperation(
        "Stopping recording",
        [] {
            const auto port =
                headmotion::app::resolveDevicePort(
                    std::nullopt
                );

            return headmotion::app::
                runRecordStopCommand(port);
        }
    );
}

void MainWindow::runDownload() {
    const std::string output_directory =
        output_input_->value();

    if (output_directory.empty()) {
        setStatus(
            "Choose an output directory first"
        );

        return;
    }

    resetProgress();

    launchOperation(
        "Downloading recording",
        [
            this,
            output_directory
        ] {
            const auto port =
                headmotion::app::resolveDevicePort(
                    std::nullopt
                );

            return headmotion::app::
                runSyncCommand(
                    port,
                    output_directory,
                    [this](
                        std::uint32_t entries_left,
                        std::uint32_t total_entries
                    ) {
                        entries_left_ =
                            entries_left;

                        total_entries_ =
                            total_entries;

                        if (
                            total_entries > 0 &&
                            entries_left == 0
                        ) {
                            setStatus(
                                "Download complete; "
                                "finalizing CSV files..."
                            );
                        }
                    }
                );
        }
    );
}

void MainWindow::chooseOutputDirectory() {
    Fl_Native_File_Chooser chooser;

    chooser.title(
        "Choose HeadMotion Output Directory"
    );

    chooser.type(
        Fl_Native_File_Chooser::
            BROWSE_DIRECTORY
    );

    const std::string current =
        output_input_->value();

    if (!current.empty()) {
        chooser.directory(
            current.c_str()
        );
    }

    const int result =
        chooser.show();

    if (result == 0) {
        output_input_->value(
            chooser.filename()
        );

        return;
    }

    if (result == -1) {
        setStatus(
            std::string(
                "Directory chooser failed: "
            ) +
            chooser.errmsg()
        );
    }
}

void MainWindow::launchOperation(
    std::string name,
    std::function<int()> operation
) {
    if (busy_.exchange(true)) {
        setStatus(
            "Another operation is already running"
        );

        return;
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    setStatus(
        name + "..."
    );

    worker_ = std::jthread(
        [
            this,
            name = std::move(name),
            operation = std::move(operation)
        ]() mutable {
            try {
                const int result =
                    operation();

                if (result == 0) {
                    setStatus(
                        name + " complete"
                    );
                } else {
                    setStatus(
                        name +
                        " failed with error code " +
                        std::to_string(result)
                    );
                }
            } catch (const std::exception& error) {
                setStatus(
                    name +
                    " failed: " +
                    error.what()
                );
            } catch (...) {
                setStatus(
                    name +
                    " failed with an unknown error"
                );
            }

            busy_ = false;
        }
    );
}

float MainWindow::selectedSampleRate() const {
    const int index =
        sample_rate_choice_->value();

    if (
        index < 0 ||
        static_cast<std::size_t>(index) >=
            SAMPLE_RATES.size()
    ) {
        return SAMPLE_RATES[
            DEFAULT_SAMPLE_RATE_INDEX
        ].hz;
    }

    return SAMPLE_RATES[
        static_cast<std::size_t>(index)
    ].hz;
}

void MainWindow::setControlsEnabled(
    bool enabled
) {
    Fl_Widget* controls[] = {
        scan_button_,
        reset_button_,
        start_button_,
        stop_button_,
        download_button_,
        browse_button_,
        sample_rate_choice_
    };

    for (auto* control : controls) {
        if (enabled) {
            control->activate();
        } else {
            control->deactivate();
        }
    }
}

void MainWindow::updateProgressUi(
    bool busy
) {
    const std::uint32_t total =
        total_entries_.load();

    const std::uint32_t left =
        entries_left_.load();

    if (total == 0) {
        progress_->value(0.0);

        progress_->copy_label(
            busy
                ? "Waiting for entry count..."
                : "0%"
        );

        return;
    }

    const std::uint32_t downloaded =
        total >= left
            ? total - left
            : 0;

    const double percent =
        100.0 *
        static_cast<double>(downloaded) /
        static_cast<double>(total);

    progress_->value(percent);

    std::ostringstream label;

    label
        << std::fixed
        << std::setprecision(1)
        << percent
        << "%  ("
        << downloaded
        << " / "
        << total
        << ")";

    progress_->copy_label(
        label.str().c_str()
    );
}

void MainWindow::updateUi() {
    std::string status;
    std::string device;

    {
        std::lock_guard<std::mutex> lock(
            state_mutex_
        );

        status = status_text_;
        device = device_text_;
    }

    status_box_->copy_label(
        status.c_str()
    );

    device_box_->copy_label(
        device.c_str()
    );

    const bool busy =
        busy_.load();

    setControlsEnabled(!busy);
    updateProgressUi(busy);
}

void MainWindow::timerCallback(
    void* context
) {
    auto* self =
        static_cast<MainWindow*>(context);

    self->updateUi();

    Fl::repeat_timeout(
        UI_REFRESH_SECONDS,
        timerCallback,
        context
    );
}

} // namespace headmotion::gui