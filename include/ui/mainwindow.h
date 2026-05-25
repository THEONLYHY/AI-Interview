#ifndef INCLUDE_UI_MAINWINDOW_H_
#define INCLUDE_UI_MAINWINDOW_H_

#include <QMainWindow>
#include <QPointer>

#include "common/dialog_state.h"
#include "ui/session_options.h"

class QAction;
class QPushButton;
class QProgressBar;
class QTextEdit;
class QLabel;
class QThread;

namespace interview::ui {

class SessionWorker;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void StartInterview();
    void StopInterview();
    void AppendContent(const QString& role, const QString& text, int question_index);
    void UpdateState(interview::common::DialogState state);
    void HandleFailure(const QString& message);
    void HandleStopped();
    void FinishWorker(bool wait);
    void ShutdownWorker(bool wait);
    void SetRunningControls(bool running);

    QTextEdit* transcript_ = nullptr;
    QLabel* state_label_ = nullptr;
    QPushButton* start_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;
    QAction* start_action_ = nullptr;
    QAction* stop_action_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;

    QPointer<QThread> worker_thread_;
    QPointer<SessionWorker> worker_;
    int question_count_ = 0;
};

}  // namespace interview::ui

#endif  // INCLUDE_UI_MAINWINDOW_H_
