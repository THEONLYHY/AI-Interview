#ifndef INCLUDE_UI_SESSION_FACTORY_H_
#define INCLUDE_UI_SESSION_FACTORY_H_

#include <memory>

#include <QString>

#include "interview/dialog_session.h"
#include "ui/session_options.h"

namespace interview::ui {

struct SessionFactoryResult {
    std::unique_ptr<interview::session::DialogSession> session;
    QString error;

    bool Ok() const { return session != nullptr && error.isEmpty(); }
};

class SessionFactory {
public:
    static SessionFactoryResult Create(const SessionOptions& options);
};

}  // namespace interview::ui

#endif  // INCLUDE_UI_SESSION_FACTORY_H_
