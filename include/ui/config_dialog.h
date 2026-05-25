#ifndef INCLUDE_UI_CONFIG_DIALOG_H_
#define INCLUDE_UI_CONFIG_DIALOG_H_

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

#include "ui/session_options.h"

namespace interview::ui {
class ConfigDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConfigDialog(QWidget* parent = nullptr);

    SessionOptions Options() const;

public slots:
    void accept() override;

private:
    void BrowseResume();
    void BrowseConfig();
    void UpdateResumeControls(bool enabled);
    bool ValidateOptions();

    QLineEdit* candidate_name_edit_ = nullptr;
    QCheckBox* use_resume_check_ = nullptr;
    QLineEdit* resume_path_edit_ = nullptr;
    QSpinBox* question_count_spin_ = nullptr;
    QPushButton* browse_button_ = nullptr;
    QComboBox* llm_mode_combo_ = nullptr;
    QComboBox* realtime_mode_combo_ = nullptr;
    QComboBox* input_mode_combo_ = nullptr;
    QLineEdit* config_path_edit_ = nullptr;
    QPushButton* config_browse_button_ = nullptr;
    QLabel* validation_error_label_ = nullptr;
};

}  // namespace interview::ui

#endif
