#include "window_internal.h"

namespace novelty {
QWidget *pathField(QLineEdit *edit, QWidget *parent, bool directory, const QString &caption, QLineEdit *initialFolder) {
    auto box = new QWidget(parent); auto row = new QHBoxLayout(box); row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(edit); auto browse = new QPushButton("Browse…", box); row->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, box, [edit, box, directory, caption, initialFolder] {
        const auto start = edit->text().isEmpty() && initialFolder ? initialFolder->text() : edit->text();
        auto path = directory ? QFileDialog::getExistingDirectory(box, caption, start)
                              : QFileDialog::getOpenFileName(box, caption, start);
        if (!path.isEmpty()) edit->setText(path);
    }); return box;
}
QComboBox *backendBox(QWidget *parent, const QString &current, bool allowNative) {
    auto box = new QComboBox(parent); box->addItem("umu — standalone Proton", "umu");
    box->addItem("Steam — existing shortcut", "steam");
    if (allowNative) box->addItem("Native Linux executable", "native");
    const auto index = box->findData(current);
    box->setCurrentIndex(index < 0 ? 0 : index); return box;
}
QComboBox *runnerBox(QWidget *parent, const QString &current, const QString &libraryRoot) {
    auto box = new QComboBox(parent); box->setEditable(true); box->setMinimumContentsLength(22);
    box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    for (auto path : installedRunners(libraryRoot)) box->addItem(path);
    box->setCurrentText(current); return box;
}
QString askPresetId(QWidget *parent, const QString &title, const QString &suggested) {
    QDialog dialog(parent); dialog.setWindowTitle(title);
    auto layout = new QVBoxLayout(&dialog);
    auto field = new QLineEdit(suggested, &dialog);
    layout->addWidget(new QLabel("Preset ID (lowercase letters, digits, dots, dashes or underscores)", &dialog));
    layout->addWidget(field);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return dialog.exec() == QDialog::Accepted ? field->text().trimmed() : QString();
}
QStringList metadataStrings(const QJsonValue &value) {
    QStringList result; for (const auto &item : value.toArray()) if (!item.toString().isEmpty()) result.append(item.toString());
    return result;
}
} // namespace novelty
