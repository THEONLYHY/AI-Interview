#include <QApplication>

#include "common/logger.h"
#include "ui/mainwindow.h"

int main(int argc, char* argv[]) {
    interview::common::Logger::Init();

    QApplication app(argc, argv);
    interview::ui::MainWindow window;
    window.show();
    return app.exec();
}
