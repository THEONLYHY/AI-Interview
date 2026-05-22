#include "ui/mainwindow.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QLabel>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include "common/logger.h"
#include "common/protocol.h"
#include "interview/dialog_session.h"
#include "interview/interview_session.h"
#include "services/pdf_parser.h"
#include "services/mock_llm_client.h"
#include "services/mock_realtime_client.h"
#include "ui/config_dialog.h"

namespace interview::ui {

namespace {

interview::common::ParsedResponse MakeEvent(uint32_t event_id,
                                            std::string session_id = {},
                                            std::string payload_json = {}) {
    interview::common::ParsedResponse event;
    event.event = event_id;
    event.session_id = std::move(session_id);
    event.payload_json = std::move(payload_json);
    return event;
}

std::vector<interview::common::ParsedResponse> BuildMockScript(
    const std::string& session_id,
    int question_count) {
    namespace events = interview::common::events;

    std::vector<interview::common::ParsedResponse> script;
    script.push_back(MakeEvent(events::kConnectionStarted));
    script.back().connect_id = "qt-mock-connect";
    script.push_back(MakeEvent(events::kSessionStarted, session_id));

    const char* answers[] = {
        "I use RAII to bind resource lifetime to object lifetime.",
        "Smart pointers express ownership and release memory automatically.",
        "Epoll scales better because it reports ready descriptors directly.",
        "A worker queue can distribute accepted sockets to event loops.",
        "Channel stores fd interests and dispatches callbacks in an EventLoop.",
        "Cleanup should unregister channels before closing descriptors.",
    };

    const int answer_count = question_count * 2;
    const int reusable_answers = static_cast<int>(sizeof(answers) / sizeof(answers[0]));
    for (int i = 0; i < answer_count; ++i) {
        const char* answer = answers[i % reusable_answers];
        script.push_back(MakeEvent(events::kAsrInfo, session_id));
        script.push_back(MakeEvent(events::kAsrResult, session_id, answer));
        script.push_back(MakeEvent(events::kAsrEnded, session_id));
    }
    script.push_back(MakeEvent(events::kSessionFinished, session_id));
    return script;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      transcript_(new QPlainTextEdit(this)),
      state_label_(new QLabel(this)),
      start_button_(new QPushButton(tr("Start"), this)),
      stop_button_(new QPushButton(tr("Stop"), this)) {
    setWindowTitle(tr("AI Interview"));
    resize(900, 620);

    transcript_->setReadOnly(true);

    auto* toolbar = addToolBar(tr("Session"));
    toolbar->addWidget(start_button_);
    toolbar->addWidget(stop_button_);
    stop_button_->setEnabled(false);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->addWidget(transcript_);
    setCentralWidget(central);

    statusBar()->addPermanentWidget(state_label_);
    UpdateState(interview::common::DialogState::kInit);

    connect(start_button_, &QPushButton::clicked,
            this, &MainWindow::StartMockInterview);
    connect(stop_button_, &QPushButton::clicked,
            this, &MainWindow::StopInterview);
}

MainWindow::~MainWindow() {
    StopInterview();
}

void MainWindow::StartMockInterview() {
    ConfigDialog config_dialog(this);
    if (config_dialog.exec() != QDialog::Accepted) {
        return;
    }
    const SessionOption options = config_dialog.Options();

    StopInterview();
    transcript_->clear();

    std::string resume_text;
    const std::string resume_path = options.resume_pdf_path.toStdString();
    if (!resume_path.empty()) {
        services::PDFParser parser;
        if (parser.IsValidPDF(resume_path)) {
            resume_text = parser.ExtractText(resume_path);
            transcript_->appendPlainText(
                tr("System: loaded resume PDF, chars=%1")
                    .arg(static_cast<qlonglong>(resume_text.size())));
        } else {
            transcript_->appendPlainText(
                tr("System: resume PDF was not valid, continuing without it."));
        }
    }

    auto interview_session =
        std::make_unique<interview::session::InterviewSession>(
            std::make_unique<interview::services::MockLLMClient>(),
            std::move(resume_text),
            options.question_count);

    auto realtime_client =
        std::make_unique<interview::services::MockRealtimeClient>(
            BuildMockScript("qt-mock-session", options.question_count),
            std::chrono::milliseconds(250));

    dialog_ = std::make_unique<interview::session::DialogSession>(
        std::move(interview_session), std::move(realtime_client), false);

    dialog_->SetContentCallback(
        [this](const std::string& role, const std::string& text,
               int question_index) {
            QMetaObject::invokeMethod(
                this,
                [this, role = QString::fromStdString(role),
                 text = QString::fromStdString(text), question_index] {
                    AppendContent(role, text, question_index);
                },
                Qt::QueuedConnection);
        });

    dialog_->SetStateCallback([this](interview::common::DialogState state) {
        QMetaObject::invokeMethod(
            this, [this, state] { UpdateState(state); }, Qt::QueuedConnection);
    });

    dialog_->Start();
    start_button_->setEnabled(false);
    stop_button_->setEnabled(true);
}

void MainWindow::StopInterview() {
    if (dialog_) {
        dialog_->Stop();
        dialog_.reset();
    }
    start_button_->setEnabled(true);
    stop_button_->setEnabled(false);
}

void MainWindow::AppendContent(const QString& role,
                               const QString& text,
                               int question_index) {
    QString prefix = role;
    if (question_index >= 0) {
        prefix += QStringLiteral(" #") + QString::number(question_index);
    }
    transcript_->appendPlainText(prefix + QStringLiteral(": ") + text);
}

void MainWindow::UpdateState(interview::common::DialogState state) {
    state_label_->setText(QString::fromStdString(
        interview::common::DialogStateToString(state)));
    if (state == interview::common::DialogState::kCompleted ||
        state == interview::common::DialogState::kStopped) {
        start_button_->setEnabled(true);
        stop_button_->setEnabled(false);
    }
}

}  // namespace interview::ui
