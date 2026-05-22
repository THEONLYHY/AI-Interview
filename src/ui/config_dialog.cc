#include "ui/config_dialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

namespace interview::ui {

ConfigDialog::ConfigDialog(QWidget* parent)
    : QDialog(parent),
      resume_path_edit_(new QLineEdit(this)),
      question_count_spin_(new QSpinBox(this)),
      browse_button_(new QPushButton(tr("..."), this)) {
    setWindowTitle(tr("Session"));

    question_count_spin_->setRange(1, 10);
    question_count_spin_->setValue(3);

    auto* resume_row = new QHBoxLayout;
    resume_row->addWidget(resume_path_edit_);
    resume_row->addWidget(browse_button_);

    auto* form = new QFormLayout;
    form->addRow(tr("Resume PDF"), resume_row);
    form->addRow(tr("Questions"), question_count_spin_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(buttons);

    connect(browse_button_, &QPushButton::clicked,
            this, &ConfigDialog::BrowseResume);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

SessionOption ConfigDialog::Options() const {
    SessionOption options;
    options.resume_pdf_path = resume_path_edit_->text();
    options.question_count = question_count_spin_->value();
    options.use_mock_llm = true;
    options.use_voice_mode = false;
    return options;
}

void ConfigDialog::BrowseResume() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose resume PDF"), QString(), tr("PDF files (*.pdf)"));
    if (!path.isEmpty()) {
        resume_path_edit_->setText(path);
    }
}

}  // namespace interview::ui
