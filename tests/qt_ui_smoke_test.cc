#include <functional>

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTest>
#include <QTimer>

#include "common/logger.h"
#include "ui/config_dialog.h"
#include "ui/mainwindow.h"

namespace {

bool WaitFor(const std::function<bool()>& condition, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (condition()) {
            return true;
        }
        QTest::qWait(20);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return condition();
}

QPushButton* FindButtonByText(QWidget& root, const QString& text) {
    const auto buttons = root.findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

void QueueAcceptSessionDialog(int question_count) {
    QTimer::singleShot(50, [question_count] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            qFatal("session dialog did not open");
        }

        auto* question_count_spin = dialog->findChild<QSpinBox*>();
        if (question_count_spin == nullptr) {
            qFatal("session dialog has no question count spin box");
        }
        question_count_spin->setValue(question_count);

        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (buttons == nullptr) {
            qFatal("session dialog has no button box");
        }
        auto* ok_button = buttons->button(QDialogButtonBox::Ok);
        if (ok_button == nullptr) {
            qFatal("session dialog has no OK button");
        }
        ok_button->click();
    });
}

class QtUiSmokeTest : public QObject {
    Q_OBJECT

private slots:
    void ConfigDialogDefaultsUseMockTextSession() {
        interview::ui::ConfigDialog dialog;

        auto* resume_path = dialog.findChild<QLineEdit*>();
        auto* question_count = dialog.findChild<QSpinBox*>();

        QVERIFY(resume_path != nullptr);
        QVERIFY(question_count != nullptr);
        QCOMPARE(question_count->minimum(), 1);
        QCOMPARE(question_count->maximum(), 10);
        QCOMPARE(question_count->value(), 3);

        const interview::ui::SessionOption options = dialog.Options();
        QCOMPARE(options.resume_pdf_path, QString());
        QCOMPARE(options.question_count, 3);
        QVERIFY(options.use_mock_llm);
        QVERIFY(!options.use_voice_mode);
    }

    void MainWindowRunsMockInterviewOffscreen() {
        interview::ui::MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* transcript = window.findChild<QPlainTextEdit*>();
        auto* start_button = FindButtonByText(window, QStringLiteral("Start"));
        auto* stop_button = FindButtonByText(window, QStringLiteral("Stop"));

        QVERIFY(transcript != nullptr);
        QVERIFY(start_button != nullptr);
        QVERIFY(stop_button != nullptr);
        QVERIFY(start_button->isEnabled());
        QVERIFY(!stop_button->isEnabled());

        QueueAcceptSessionDialog(1);
        QTest::mouseClick(start_button, Qt::LeftButton);

        QVERIFY2(WaitFor(
                     [transcript] {
                         return transcript->toPlainText().contains(
                             QStringLiteral("summary:"));
                     },
                     6000),
                 qPrintable(transcript->toPlainText()));

        const QString text = transcript->toPlainText();
        QVERIFY2(text.contains(QStringLiteral("system:")),
                 qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("question #1:")),
                 qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("candidate #1:")),
                 qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("feedback #1:")),
                 qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("summary:")),
                 qPrintable(text));
        QVERIFY(start_button->isEnabled());
        QVERIFY(!stop_button->isEnabled());
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    qputenv("XDG_CACHE_HOME", QByteArray("/tmp"));
    interview::common::Logger::Init();

    QApplication app(argc, argv);
    QtUiSmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "qt_ui_smoke_test.moc"
