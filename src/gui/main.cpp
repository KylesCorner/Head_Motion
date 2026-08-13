#include "headmotion/gui/MainWindow.hpp"

#include <FL/Fl.H>

int main(int argc, char** argv) {
    headmotion::gui::MainWindow window;

    window.show(argc, argv);

    return Fl::run();
}