#include <functional>

#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>
#include <QTextEdit>
#include <QTimer>

#include "common/logger.h"
#include "ui/config_dialog.h"
#include "ui/mainwindow.h"
#include "ui/session_options.h"

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

        auto* question_count_spin =
            dialog->findChild<QSpinBox*>(QStringLiteral("questionCountSpin"));
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

        auto* candidate_name =
            dialog.findChild<QLineEdit*>(QStringLiteral("candidateNameEdit"));
        auto* use_resume =
            dialog.findChild<QCheckBox*>(QStringLiteral("useResumeCheckBox"));
        auto* resume_path =
            dialog.findChild<QLineEdit*>(QStringLiteral("resumePathEdit"));
        auto* question_count =
            dialog.findChild<QSpinBox*>(QStringLiteral("questionCountSpin"));
        auto* llm_mode =
            dialog.findChild<QComboBox*>(QStringLiteral("llmModeCombo"));
        auto* realtime_mode =
            dialog.findChild<QComboBox*>(QStringLiteral("realtimeModeCombo"));
        auto* input_mode =
            dialog.findChild<QComboBox*>(QStringLiteral("inputModeCombo"));
        auto* config_path =
            dialog.findChild<QLineEdit*>(QStringLiteral("configPathEdit"));

        QVERIFY(candidate_name != nullptr);
        QVERIFY(use_resume != nullptr);
        QVERIFY(resume_path != nullptr);
        QVERIFY(question_count != nullptr);
        QVERIFY(llm_mode != nullptr);
        QVERIFY(realtime_mode != nullptr);
        QVERIFY(input_mode != nullptr);
        QVERIFY(config_path != nullptr);

        QCOMPARE(candidate_name->text(), QStringLiteral("Candidate"));
        QVERIFY(!use_resume->isChecked());
        QCOMPARE(question_count->minimum(), 1);
        QCOMPARE(question_count->maximum(), 10);
        QCOMPARE(question_count->value(), 3);
        QCOMPARE(config_path->text(), QStringLiteral("config/local_config.json"));

        const interview::ui::SessionOptions options = dialog.Options();
        QCOMPARE(options.candidate_name, QStringLiteral("Candidate"));
        QCOMPARE(options.resume_pdf_path, QString());
        QCOMPARE(options.question_count, 3);
        QCOMPARE(options.config_path, QStringLiteral("config/local_config.json"));
        QCOMPARE(options.llm_mode, interview::ui::LlmMode::kMock);
        QCOMPARE(options.realtime_mode, interview::ui::RealtimeMode::kMockScript);
        QCOMPARE(options.input_mode, interview::ui::InputMode::kTextMock);
    }

    void ConfigDialogRequiresResumePathOnlyWhenEnabled() {
        interview::ui::ConfigDialog dialog;
        dialog.show();

        auto* use_resume =
            dialog.findChild<QCheckBox*>(QStringLiteral("useResumeCheckBox"));
        auto* resume_path =
            dialog.findChild<QLineEdit*>(QStringLiteral("resumePathEdit"));
        auto* error_label =
            dialog.findChild<QLabel*>(QStringLiteral("validationErrorLabel"));
        auto* buttons = dialog.findChild<QDialogButtonBox*>();

        QVERIFY(use_resume != nullptr);
        QVERIFY(resume_path != nullptr);
        QVERIFY(error_label != nullptr);
        QVERIFY(buttons != nullptr);

        use_resume->setChecked(true);
        resume_path->clear();

        QSignalSpy accepted(&dialog, &QDialog::accepted);
        buttons->button(QDialogButtonBox::Ok)->click();

        QCOMPARE(accepted.count(), 0);
        QVERIFY(dialog.isVisible());
        QVERIFY2(error_label->text().contains(QStringLiteral("resume PDF")),
                 qPrintable(error_label->text()));
    }

    void ConfigDialogRequiresConfigForRealOrVoiceModes() {
        interview::ui::ConfigDialog dialog;
        dialog.show();

        auto* llm_mode =
            dialog.findChild<QComboBox*>(QStringLiteral("llmModeCombo"));
        auto* input_mode =
            dialog.findChild<QComboBox*>(QStringLiteral("inputModeCombo"));
        auto* config_path =
            dialog.findChild<QLineEdit*>(QStringLiteral("configPathEdit"));
        auto* error_label =
            dialog.findChild<QLabel*>(QStringLiteral("validationErrorLabel"));
        auto* buttons = dialog.findChild<QDialogButtonBox*>();

        QVERIFY(llm_mode != nullptr);
        QVERIFY(input_mode != nullptr);
        QVERIFY(config_path != nullptr);
        QVERIFY(error_label != nullptr);
        QVERIFY(buttons != nullptr);

        llm_mode->setCurrentText(QStringLiteral("Real LLM"));
        input_mode->setCurrentText(QStringLiteral("Voice"));
        config_path->setText(QStringLiteral("/tmp/ai_interview_config.json"));

        const interview::ui::SessionOptions options = dialog.Options();
        QCOMPARE(options.config_path,
                 QStringLiteral("/tmp/ai_interview_config.json"));
        QCOMPARE(options.llm_mode, interview::ui::LlmMode::kReal);
        QCOMPARE(options.input_mode, interview::ui::InputMode::kVoice);

        config_path->clear();

        QSignalSpy accepted(&dialog, &QDialog::accepted);
        buttons->button(QDialogButtonBox::Ok)->click();

        QCOMPARE(accepted.count(), 0);
        QVERIFY(dialog.isVisible());
        QVERIFY2(error_label->text().contains(QStringLiteral("config")),
                 qPrintable(error_label->text()));
    }

    void MainWindowRunsMockInterviewOffscreen() {
        interview::ui::MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* transcript =
            window.findChild<QTextEdit*>(QStringLiteral("transcriptView"));
        auto* progress =
            window.findChild<QProgressBar*>(QStringLiteral("questionProgress"));
        auto* start_button = FindButtonByText(window, QStringLiteral("Start"));
        auto* stop_button = FindButtonByText(window, QStringLiteral("Stop"));

        QVERIFY(transcript != nullptr);
        QVERIFY(progress != nullptr);
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
        QCOMPARE(progress->maximum(), 1);
        QCOMPARE(progress->value(), 1);
        QVERIFY(start_button->isEnabled());
        QVERIFY(!stop_button->isEnabled());
    }

    void MainWindowStopRestoresButtons() {
        interview::ui::MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* start_button = FindButtonByText(window, QStringLiteral("Start"));
        auto* stop_button = FindButtonByText(window, QStringLiteral("Stop"));

        QVERIFY(start_button != nullptr);
        QVERIFY(stop_button != nullptr);

        QueueAcceptSessionDialog(3);
        QTest::mouseClick(start_button, Qt::LeftButton);

        QVERIFY(WaitFor([stop_button] { return stop_button->isEnabled(); },
                        1000));
        QTest::mouseClick(stop_button, Qt::LeftButton);

        QVERIFY(WaitFor(
            [start_button, stop_button] {
                return start_button->isEnabled() && !stop_button->isEnabled();
            },
            3000));
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
