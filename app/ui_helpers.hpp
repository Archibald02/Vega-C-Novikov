#pragma once
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>
namespace live::ui {
inline QLabel *paragraph(const QString &text) {
    auto *w = new QLabel(text);
    w->setTextFormat(Qt::PlainText);
    w->setWordWrap(true);
    return w;
}
inline QWidget *fold(const QString &title, QWidget *content, bool expanded = false) {
    auto *frame = new QFrame;
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(0, 4, 0, 4);
    auto *button = new QToolButton;
    button->setText(title);
    button->setCheckable(true);
    button->setChecked(expanded);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setAccessibleName(title);
    layout->addWidget(button);
    layout->addWidget(content);
    content->setVisible(expanded);
    QObject::connect(button, &QToolButton::toggled, content, [button, content](bool on) {
        content->setVisible(on);
        button->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    return frame;
}
/** Prevent page scrolling from silently changing a parameter under the pointer. */
class WheelGuard final : public QObject {
  public:
    using QObject::QObject;

  protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Wheel)
            if (auto *widget = qobject_cast<QWidget *>(object); widget && !widget->hasFocus()) {
                event->ignore();
                return true;
            }
        return QObject::eventFilter(object, event);
    }
};
inline void prepare_controls(QWidget *root) {
    auto *guard = new WheelGuard(root);
    for (auto *w : root->findChildren<QWidget *>())
        if (qobject_cast<QAbstractSpinBox *>(w) || qobject_cast<QComboBox *>(w)) {
            w->setFocusPolicy(Qt::StrongFocus);
            w->installEventFilter(guard);
        }
    for (auto *form : root->findChildren<QFormLayout *>())
        for (int i = 0; i < form->rowCount(); ++i) {
            auto *label_item = form->itemAt(i, QFormLayout::LabelRole);
            auto *field_item = form->itemAt(i, QFormLayout::FieldRole);
            auto *label = label_item ? qobject_cast<QLabel *>(label_item->widget()) : nullptr;
            auto *field = field_item ? field_item->widget() : nullptr;
            if (label && field) {
                label->setBuddy(field);
                if (field->accessibleName().isEmpty())
                    field->setAccessibleName(label->text());
            }
        }
}
} // namespace live::ui
