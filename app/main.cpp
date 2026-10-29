#include <QApplication>

#include "MainWindow.hpp"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Signal Analyzer"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    MainWindow window;
    window.show();
    // signal_analyzer [recording.sacap]: replay a capture right away.
    if (const QStringList args = QApplication::arguments(); args.size() > 1) {
        window.replay(args.at(1));
    }
    return QApplication::exec();
}
