#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class Fl_Box;
class Fl_Button;
class Fl_Choice;
class Fl_Double_Window;
class Fl_Input;
class Fl_Progress;

namespace headmotion::gui {

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    void show(int argc, char** argv);

private:
    void buildUi();
    void wireCallbacks();

    void runScan();
    void runReset();
    void runStart();
    void runStop();
    void runDownload();

    void chooseOutputDirectory();
    void refreshDevice();

    void launchOperation(
        std::string name,
        std::function<int()> operation
    );

    void resetProgress();

    void setStatus(std::string status);
    void setDevice(std::string device);

    float selectedSampleRate() const;

    void setControlsEnabled(bool enabled);
    void updateProgressUi(bool busy);
    void updateUi();

    static void timerCallback(void* context);

private:
    Fl_Double_Window* window_ = nullptr;

    Fl_Box* device_box_ = nullptr;
    Fl_Button* scan_button_ = nullptr;

    Fl_Choice* sample_rate_choice_ = nullptr;

    Fl_Button* reset_button_ = nullptr;
    Fl_Button* start_button_ = nullptr;
    Fl_Button* stop_button_ = nullptr;

    Fl_Input* output_input_ = nullptr;
    Fl_Button* browse_button_ = nullptr;

    Fl_Button* download_button_ = nullptr;
    Fl_Progress* progress_ = nullptr;

    Fl_Box* status_box_ = nullptr;

    std::jthread worker_;

    std::atomic<bool> busy_{false};

    std::atomic<std::uint32_t> entries_left_{0};
    std::atomic<std::uint32_t> total_entries_{0};

    mutable std::mutex state_mutex_;

    std::string status_text_ = "Ready";
    std::string device_text_ = "Device: not selected";
};

} // namespace headmotion::gui