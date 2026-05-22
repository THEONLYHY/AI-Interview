#ifndef INCLUDE_UI_CONFIG_DIALOG_H_
#define INCLUDE_UI_CONFIG_DIALOG_H_

#include <QDialog>
#include <QString>

class QLineEdit;
class QPushButton;
class QSpinBox;

namespace interview::ui {

struct SessionOption {
    QString resume_pdf_path;
    int question_count = 3;
    bool use_mock_llm = true;
    bool use_voice_mode = false;
};

class ConfigDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConfigDialog(QWidget* parent = nullptr);

    SessionOption Options() const;

private:
    void BrowseResume();

    QLineEdit* resume_path_edit_ = nullptr;
    QSpinBox* question_count_spin_ = nullptr;
    QPushButton* browse_button_ = nullptr;
};

}  // namespace interview::ui

#endif
