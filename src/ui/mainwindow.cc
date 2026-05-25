#include "ui/mainwindow.h"

#include <algorithm>

#include <QAction>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QTextEdit>
#include <QThread>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include "ui/config_dialog.h"
#include "ui/session_worker.h"

namespace interview::ui {
namespace {

QString RoleColor(const QString& role) {
    const QString normalized = role.toLower();
    if (normalized == QStringLiteral("system")) {
        return QStringLiteral("#51606a");
    }
    if (normalized == QStringLiteral("error")) {
        return QStringLiteral("#b00020");
    }
    if (normalized == QStringLiteral("question")) {
        return QStringLiteral("#0f5c8c");
    }
    if (normalized == QStringLiteral("followup")) {
        return QStringLiteral("#6c4f00");
    }
    if (normalized == QStringLiteral("candidate")) {
        return QStringLiteral("#247a4d");
    }
    if (normalized == QStringLiteral("feedback")) {
        return QStringLiteral("#7a3f89");
    }
    if (normalized == QStringLiteral("summary")) {
        return QStringLiteral("#2f5d62");
    }
    return QStringLiteral("#303030");
}

QString RoleLabel(const QString& role, int question_index) {
    QString label = role.toLower();
    if (question_index > 0) {
        label += QStringLiteral(" #") + QString::number(question_index);
    }
    return label;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      transcript_(new QTextEdit(this)),
      state_label_(new QLabel(this)),
      start_button_(new QPushButton(tr("Start"), this)),
      stop_button_(new QPushButton(tr("Stop"), this)),
      start_action_(new QAction(tr("Start"), this)),
      stop_action_(new QAction(tr("Stop"), this)),
      progress_bar_(new QProgressBar(this)) {
    setWindowTitle(tr("AI Interview"));
    resize(2000, 1200);

    transcript_->setObjectName(QStringLiteral("transcriptView"));
    state_label_->setObjectName(QStringLiteral("stateLabel"));
    start_button_->setObjectName(QStringLiteral("startButton"));
    stop_button_->setObjectName(QStringLiteral("stopButton"));
    progress_bar_->setObjectName(QStringLiteral("questionProgress"));

    transcript_->setReadOnly(true);
    transcript_->setAcceptRichText(false);
    progress_bar_->setRange(0, 3);
    progress_bar_->setValue(0);
    stop_button_->setEnabled(false);

    auto* session_menu = menuBar()->addMenu(tr("Session"));
    session_menu->addAction(start_action_);
    session_menu->addAction(stop_action_);
    stop_action_->setEnabled(false);
    session_menu->addSeparator();
    session_menu->addAction(tr("Quit"), this, &QWidget::close);

    auto* toolbar = addToolBar(tr("Session"));
    toolbar->addWidget(start_button_);
    toolbar->addWidget(stop_button_);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    auto* status_row = new QHBoxLayout;
    status_row->addWidget(new QLabel(tr("State:"), this));
    status_row->addWidget(state_label_);
    status_row->addStretch(1);
    layout->addLayout(status_row);
    layout->addWidget(transcript_, 1);
    layout->addWidget(progress_bar_);
    setCentralWidget(central);

    statusBar()->showMessage(tr("Ready"));
    UpdateState(interview::common::DialogState::kInit);

    connect(start_button_, &QPushButton::clicked,
            this, &MainWindow::StartInterview);
    connect(stop_button_, &QPushButton::clicked,
            this, &MainWindow::StopInterview);
    connect(start_action_, &QAction::triggered,
            this, &MainWindow::StartInterview);
    connect(stop_action_, &QAction::triggered,
            this, &MainWindow::StopInterview);
}

MainWindow::~MainWindow() {
    ShutdownWorker(true);
}

void MainWindow::StartInterview() {
    ConfigDialog config_dialog(this);
    if (config_dialog.exec() != QDialog::Accepted) {
        return;
    }
    const SessionOptions options = config_dialog.Options();

    ShutdownWorker(true);
    transcript_->clear();
    question_count_ = std::max(1, options.question_count);
    progress_bar_->setRange(0, question_count_);
    progress_bar_->setValue(0);
    SetRunningControls(true);
    statusBar()->showMessage(tr("Starting interview"));

    auto* thread = new QThread(this);
    auto* worker = new SessionWorker;
    worker->moveToThread(thread);
    worker_thread_ = thread;
    worker_ = worker;

    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(worker, &SessionWorker::ContentReceived,
            this, &MainWindow::AppendContent);
    connect(worker, &SessionWorker::StateChanged,
            this, &MainWindow::UpdateState);
    connect(worker, &SessionWorker::Failed,
            this, &MainWindow::HandleFailure);
    connect(worker, &SessionWorker::Stopped,
            this, &MainWindow::HandleStopped);

    thread->start();
    QMetaObject::invokeMethod(
        worker,
        [worker, options] {
            worker->Start(options);
        },
        Qt::QueuedConnection);
}

void MainWindow::StopInterview() {
    if (!worker_) {
        SetRunningControls(false);
        return;
    }
    start_button_->setEnabled(false);
    stop_button_->setEnabled(false);
    statusBar()->showMessage(tr("Stopping interview"));
    QMetaObject::invokeMethod(worker_.data(), &SessionWorker::Stop,
                              Qt::QueuedConnection);
}

void MainWindow::AppendContent(const QString& role,
                               const QString& text,
                               int question_index) {
    if (question_index > 0) {
        progress_bar_->setValue(std::min(question_index, question_count_));
    }

    const QString label = RoleLabel(role, question_index).toHtmlEscaped();
    QString body = text.toHtmlEscaped();
    body.replace(QStringLiteral("\n"), QStringLiteral("<br>"));

    transcript_->append(
        QStringLiteral(
            "<p style=\"margin:6px 0;\"><span style=\"color:%1;"
            "font-weight:600;\">%2:</span> %3</p>")
            .arg(RoleColor(role), label, body));
}

void MainWindow::UpdateState(interview::common::DialogState state) {
    state_label_->setText(QString::fromStdString(
        interview::common::DialogStateToString(state)));

    if (state == interview::common::DialogState::kCompleted) {
        SetRunningControls(false);
        statusBar()->showMessage(tr("Interview complete"));
        FinishWorker(false);
    } else if (state == interview::common::DialogState::kStopped) {
        SetRunningControls(false);
        statusBar()->showMessage(tr("Interview stopped"));
    }
}

void MainWindow::HandleFailure(const QString& message) {
    AppendContent(QStringLiteral("error"), message, -1);
    SetRunningControls(false);
    statusBar()->showMessage(message);
    FinishWorker(false);
}

void MainWindow::HandleStopped() {
    SetRunningControls(false);
    statusBar()->showMessage(tr("Interview stopped"));
    FinishWorker(false);
}

void MainWindow::FinishWorker(bool wait) {
    if (!worker_thread_) {
        worker_ = nullptr;
        return;
    }

    QThread* thread = worker_thread_;
    worker_ = nullptr;
    worker_thread_ = nullptr;
    thread->quit();
    if (wait && thread != QThread::currentThread()) {
        thread->wait(5000);
    }
}

void MainWindow::ShutdownWorker(bool wait) {
    if (worker_ && worker_thread_ && worker_thread_->isRunning()) {
        const Qt::ConnectionType connection_type =
            wait && worker_thread_.data() != QThread::currentThread()
                ? Qt::BlockingQueuedConnection
                : Qt::QueuedConnection;
        QMetaObject::invokeMethod(worker_.data(), &SessionWorker::Stop,
                                  connection_type);
    }
    FinishWorker(wait);
    SetRunningControls(false);
}

void MainWindow::SetRunningControls(bool running) {
    start_button_->setEnabled(!running);
    stop_button_->setEnabled(running);
    start_action_->setEnabled(!running);
    stop_action_->setEnabled(running);
}

}  // namespace interview::ui
