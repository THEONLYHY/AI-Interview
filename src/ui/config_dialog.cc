#include "ui/config_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVariant>

namespace interview::ui {

ConfigDialog::ConfigDialog(QWidget* parent)
    : QDialog(parent),
      candidate_name_edit_(new QLineEdit(this)),
      use_resume_check_(new QCheckBox(tr("Use resume"), this)),
      resume_path_edit_(new QLineEdit(this)),
      question_count_spin_(new QSpinBox(this)),
      browse_button_(new QPushButton(tr("..."), this)),
      llm_mode_combo_(new QComboBox(this)),
      realtime_mode_combo_(new QComboBox(this)),
      input_mode_combo_(new QComboBox(this)),
      config_path_edit_(new QLineEdit(this)),
      config_browse_button_(new QPushButton(tr("..."), this)),
      validation_error_label_(new QLabel(this)) {
    setWindowTitle(tr("Session"));

    candidate_name_edit_->setObjectName(QStringLiteral("candidateNameEdit"));
    use_resume_check_->setObjectName(QStringLiteral("useResumeCheckBox"));
    resume_path_edit_->setObjectName(QStringLiteral("resumePathEdit"));
    question_count_spin_->setObjectName(QStringLiteral("questionCountSpin"));
    browse_button_->setObjectName(QStringLiteral("resumeBrowseButton"));
    llm_mode_combo_->setObjectName(QStringLiteral("llmModeCombo"));
    realtime_mode_combo_->setObjectName(QStringLiteral("realtimeModeCombo"));
    input_mode_combo_->setObjectName(QStringLiteral("inputModeCombo"));
    config_path_edit_->setObjectName(QStringLiteral("configPathEdit"));
    config_browse_button_->setObjectName(QStringLiteral("configBrowseButton"));
    validation_error_label_->setObjectName(
        QStringLiteral("validationErrorLabel"));

    candidate_name_edit_->setText(QStringLiteral("Candidate"));
    question_count_spin_->setRange(1, 10);
    question_count_spin_->setValue(3);
    config_path_edit_->setText(QStringLiteral("config/local_config.json"));
    // 调试用
    llm_mode_combo_->addItem(tr("Mock LLM"),
                             static_cast<int>(LlmMode::kMock));
    llm_mode_combo_->addItem(tr("Real LLM"),
                             static_cast<int>(LlmMode::kReal));
    realtime_mode_combo_->addItem(tr("Mock Script"),
                                  static_cast<int>(RealtimeMode::kMockScript));
    realtime_mode_combo_->addItem(tr("Real WSS"),
                                  static_cast<int>(RealtimeMode::kRealWss));
    input_mode_combo_->addItem(tr("Text Mock"),
                               static_cast<int>(InputMode::kTextMock));
    input_mode_combo_->addItem(tr("Voice"), static_cast<int>(InputMode::kVoice));
    // 设置默认值
    llm_mode_combo_->setCurrentIndex(
                    llm_mode_combo_->findData(static_cast<int>(LlmMode::kReal)));
    realtime_mode_combo_->setCurrentIndex(
                    realtime_mode_combo_->findData(static_cast<int>(RealtimeMode::kRealWss)));
    input_mode_combo_->setCurrentIndex(
                    input_mode_combo_->findData(static_cast<int>(InputMode::kVoice)));

    config_path_edit_->setText(QStringLiteral("config/local_config.json"));

    validation_error_label_->setStyleSheet(QStringLiteral("color: #b00020;"));
    validation_error_label_->setWordWrap(true);
    validation_error_label_->hide();

    auto* resume_row = new QHBoxLayout;
    resume_row->addWidget(resume_path_edit_);
    resume_row->addWidget(browse_button_);

    auto* config_row = new QHBoxLayout;
    config_row->addWidget(config_path_edit_);
    config_row->addWidget(config_browse_button_);

    auto* form = new QFormLayout;
    form->addRow(tr("Candidate"), candidate_name_edit_);
    form->addRow(QString(), use_resume_check_);
    form->addRow(tr("Resume PDF"), resume_row);
    form->addRow(tr("Questions"), question_count_spin_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(validation_error_label_);
    root->addWidget(buttons);

    UpdateResumeControls(false);

    connect(use_resume_check_, &QCheckBox::toggled,
            this, &ConfigDialog::UpdateResumeControls);
    connect(browse_button_, &QPushButton::clicked,
            this, &ConfigDialog::BrowseResume);
    connect(config_browse_button_, &QPushButton::clicked,
            this, &ConfigDialog::BrowseConfig);
    connect(buttons, &QDialogButtonBox::accepted, this, &ConfigDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

SessionOptions ConfigDialog::Options() const {
    SessionOptions options;
    options.candidate_name = candidate_name_edit_->text().trimmed();
    if (options.candidate_name.isEmpty()) {
        options.candidate_name = QStringLiteral("Candidate");
    }
    options.resume_pdf_path =
        use_resume_check_->isChecked() ? resume_path_edit_->text().trimmed()
                                       : QString();
    options.question_count = question_count_spin_->value();
    options.config_path = config_path_edit_->text().trimmed();
    options.llm_mode = static_cast<LlmMode>(llm_mode_combo_->currentData().toInt());
    options.realtime_mode =
        static_cast<RealtimeMode>(realtime_mode_combo_->currentData().toInt());
    options.input_mode =
        static_cast<InputMode>(input_mode_combo_->currentData().toInt());
    return options;
}

void ConfigDialog::accept() {
    if (!ValidateOptions()) {
        return;
    }
    QDialog::accept();
}

void ConfigDialog::BrowseResume() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose resume PDF"), QString(), tr("PDF files (*.pdf)"));
    if (!path.isEmpty()) {
        resume_path_edit_->setText(path);
    }
}

void ConfigDialog::BrowseConfig() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose config file"), QString(), tr("JSON files (*.json)"));
    if (!path.isEmpty()) {
        config_path_edit_->setText(path);
    }
}

void ConfigDialog::UpdateResumeControls(bool enabled) {
    resume_path_edit_->setEnabled(enabled);
    browse_button_->setEnabled(enabled);
}

bool ConfigDialog::ValidateOptions() {
    validation_error_label_->clear();
    validation_error_label_->hide();

    const SessionOptions options = Options();
    if (use_resume_check_->isChecked() && options.resume_pdf_path.isEmpty()) {
        validation_error_label_->setText(
            tr("resume PDF path is required when resume is enabled."));
        validation_error_label_->show();
        return false;
    }

    const bool needs_config =
        options.llm_mode == LlmMode::kReal ||
        options.realtime_mode == RealtimeMode::kRealWss ||
        options.input_mode == InputMode::kVoice;
    if (needs_config && options.config_path.isEmpty()) {
        validation_error_label_->setText(
            tr("config path is required for real or voice modes."));
        validation_error_label_->show();
        return false;
    }

    return true;
}

}  // namespace interview::ui
