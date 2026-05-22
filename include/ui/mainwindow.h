#ifndef INCLUDE_UI_MAINWINDOW_H_
#define INCLUDE_UI_MAINWINDOW_H_

#include <memory>

#include <QMainWindow>

#include "common/dialog_state.h"

class QPushButton;
class QPlainTextEdit;
class QLabel;

namespace interview::session {
class DialogSession;
}  // namespace interview::session

namespace interview::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void StartMockInterview();
    void StopInterview();
    void AppendContent(const QString& role, const QString& text, int question_index);
    void UpdateState(interview::common::DialogState state);

    QPlainTextEdit* transcript_ = nullptr;
    QLabel* state_label_ = nullptr;
    QPushButton* start_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;

    std::unique_ptr<interview::session::DialogSession> dialog_;
};

}  // namespace interview::ui

#endif  // INCLUDE_UI_MAINWINDOW_H_
