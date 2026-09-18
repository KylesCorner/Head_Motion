#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class Fl_Box;
class Fl_Button;
class Fl_Choice;
class Fl_Double_Window;
class Fl_Input;
class Fl_Progress;
class Fl_Scroll;
class Fl_Widget;

namespace headmotion::app {
class CommandOutput;
struct CommandEvent;
}

namespace headmotion::gui {

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    MainWindow(
        const MainWindow&
    ) = delete;

    MainWindow& operator=(
        const MainWindow&
    ) = delete;

    void show(
        int argc,
        char** argv
    );

private:
    struct DeviceOperation {
        std::string key;
        std::string device_id;
        std::string port;
        std::string identity;

        std::atomic<bool> busy{false};
        std::atomic<std::uint32_t> entries_left{0};
        std::atomic<std::uint32_t> total_entries{0};

        mutable std::mutex state_mutex;
        std::string status = "Idle";

        std::jthread worker;
    };

    using DevicePtr =
        std::shared_ptr<DeviceOperation>;

    struct DeviceRow {
        MainWindow* owner = nullptr;
        std::string device_key;

        Fl_Box* device_id_box = nullptr;
        Fl_Box* port_box = nullptr;
        Fl_Box* status_box = nullptr;
        Fl_Box* progress_box = nullptr;
        Fl_Box* active_box = nullptr;

        Fl_Button* reset_button = nullptr;
        Fl_Button* start_button = nullptr;
        Fl_Button* stop_button = nullptr;
        Fl_Button* sync_button = nullptr;
    };

    using DeviceCommand =
        std::function<int(
            const DevicePtr&,
            headmotion::app::CommandOutput&
        )>;

    void buildUi();
    void wireCallbacks();

    void rebuildDeviceRows();
    void clearDeviceRows();

    void runScan();

    void runResetAll();
    void runStartAll();
    void runStopAll();
    void runSyncAll();

    void runResetOne(
        const std::string& device_key
    );

    void runStartOne(
        const std::string& device_key
    );

    void runStopOne(
        const std::string& device_key
    );

    void runSyncOne(
        const std::string& device_key
    );

    void launchDevices(
        std::vector<DevicePtr> devices,
        std::string command_name,
        std::string display_name,
        DeviceCommand operation
    );

    void launchDeviceOperation(
        const DevicePtr& device,
        std::string command_name,
        std::string display_name,
        DeviceCommand operation
    );

    void handleDeviceEvent(
        const DevicePtr& device,
        const headmotion::app::CommandEvent& event
    );

    DevicePtr findDevice(
        const std::string& device_key
    ) const;

    std::vector<DevicePtr>
        deviceSnapshot() const;

    std::vector<DevicePtr>
        idleDeviceSnapshot() const;

    std::size_t busyDeviceCount() const;
    std::size_t idleDeviceCount() const;
    bool anyDeviceBusy() const;

    void chooseOutputDirectory();
    std::string outputDirectory() const;
    float selectedSampleRate() const;

    void setStatus(
        std::string status
    );

    void updateDeviceRows();
    void updateProgressUi();
    void updateUi();
    void setControlsEnabled();

    static void timerCallback(
        void* context
    );

private:
    Fl_Double_Window* window_ = nullptr;

    Fl_Button* scan_button_ = nullptr;
    Fl_Button* reset_all_button_ = nullptr;
    Fl_Button* start_all_button_ = nullptr;
    Fl_Button* stop_all_button_ = nullptr;
    Fl_Button* sync_all_button_ = nullptr;

    Fl_Choice* sample_rate_choice_ = nullptr;
    Fl_Input* output_input_ = nullptr;
    Fl_Button* browse_button_ = nullptr;

    Fl_Box* device_count_box_ = nullptr;

    Fl_Box* header_device_id_ = nullptr;
    Fl_Box* header_port_ = nullptr;
    Fl_Box* header_status_ = nullptr;
    Fl_Box* header_progress_ = nullptr;
    Fl_Box* header_active_ = nullptr;
    Fl_Box* header_actions_ = nullptr;

    Fl_Scroll* device_scroll_ = nullptr;

    Fl_Progress* progress_ = nullptr;
    Fl_Box* status_box_ = nullptr;

    std::vector<
        std::unique_ptr<DeviceRow>
    > device_rows_;

    std::jthread scan_worker_;

    std::atomic<bool> scan_busy_{false};
    std::atomic<bool> rows_dirty_{false};
    std::atomic<bool> operation_failed_{false};

    mutable std::mutex devices_mutex_;
    std::unordered_map<
        std::string,
        DevicePtr
    > devices_;

    mutable std::mutex state_mutex_;
    std::string status_text_ = "Ready";
};

} // namespace headmotion::gui
