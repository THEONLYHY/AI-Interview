#include "ui/session_worker.h"

#include <exception>
#include <string>
#include <utility>

#include <QMetaType>

#include "ui/session_factory.h"

namespace interview::ui {

SessionWorker::SessionWorker(QObject* parent) : QObject(parent) {
    qRegisterMetaType<interview::ui::SessionOptions>(
        "interview::ui::SessionOptions");
    qRegisterMetaType<interview::common::DialogState>(
        "interview::common::DialogState");
}

SessionWorker::~SessionWorker() {
    StopSession(false);
}

void SessionWorker::Start(interview::ui::SessionOptions options) {
    StopSession(false);
    stopped_emitted_.store(false);

    try {
        SessionFactoryResult result = SessionFactory::Create(options);
        if (!result.Ok()) {
            emit Failed(result.error);
            EmitStoppedOnce();
            return;
        }

        session_ = std::move(result.session);
        session_->SetContentCallback(
            [this](const std::string& role, const std::string& text,
                   int question_index) {
                emit ContentReceived(QString::fromStdString(role),
                                     QString::fromStdString(text),
                                     question_index);
            });
        session_->SetStateCallback(
            [this](interview::common::DialogState state) {
                emit StateChanged(state);
                if (state == interview::common::DialogState::kStopped) {
                    EmitStoppedOnce();
                }
            });

        session_->Start();
        if (session_->State() == interview::common::DialogState::kStopped) {
            EmitStoppedOnce();
        }
    } catch (const std::exception& e) {
        StopSession(false);
        emit Failed(QStringLiteral("Session failed to start: %1")
                        .arg(QString::fromUtf8(e.what())));
        EmitStoppedOnce();
    } catch (...) {
        StopSession(false);
        emit Failed(QStringLiteral("Session failed to start: unknown error"));
        EmitStoppedOnce();
    }
}

void SessionWorker::Stop() {
    StopSession(true);
}

void SessionWorker::StopSession(bool notify) {
    if (!session_) {
        if (notify) {
            EmitStoppedOnce();
        }
        return;
    }

    if (!notify) {
        session_->SetContentCallback({});
        session_->SetStateCallback({});
    }
    session_->Stop();
    session_.reset();
    if (notify) {
        EmitStoppedOnce();
    }
}

void SessionWorker::EmitStoppedOnce() {
    bool expected = false;
    if (stopped_emitted_.compare_exchange_strong(expected, true)) {
        emit Stopped();
    }
}

}  // namespace interview::ui
