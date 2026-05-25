#ifndef INCLUDE_UI_SESSION_WORKER_H_
#define INCLUDE_UI_SESSION_WORKER_H_

#include <atomic>
#include <memory>

#include <QObject>
#include <QString>

#include "common/dialog_state.h"
#include "interview/dialog_session.h"
#include "ui/session_options.h"

namespace interview::ui {

class SessionWorker : public QObject {
    Q_OBJECT

public:
    explicit SessionWorker(QObject* parent = nullptr);
    ~SessionWorker() override;

public slots:
    void Start(interview::ui::SessionOptions options);
    void Stop();

signals:
    void ContentReceived(QString role, QString text, int question_index);
    void StateChanged(interview::common::DialogState state);
    void Failed(QString message);
    void Stopped();

private:
    void StopSession(bool notify);
    void EmitStoppedOnce();

    std::unique_ptr<interview::session::DialogSession> session_;
    std::atomic<bool> stopped_emitted_{false};
};

}  // namespace interview::ui

Q_DECLARE_METATYPE(interview::common::DialogState)

#endif  // INCLUDE_UI_SESSION_WORKER_H_
