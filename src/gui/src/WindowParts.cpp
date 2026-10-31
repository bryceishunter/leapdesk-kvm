/*
 * Leapdesk KVM -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Leapdesk KVM contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "WindowParts.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>

#include <algorithm>

namespace {

const char kMuted[] = "leapdeskMuted";

QColor blend(const QColor& from, const QColor& to, double amount)
{
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * amount,
                            from.greenF() + (to.greenF() - from.greenF()) * amount,
                            from.blueF() + (to.blueF() - from.blueF()) * amount);
}

QColor accentColor(const QPalette& palette)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    return palette.color(QPalette::Accent);
#else
    return palette.color(QPalette::Highlight);
#endif
}

// the outlines of the role pictures, drawn on a 34 by 34 grid
void drawRolePicture(QPainter& painter, RoleCard::Kind kind, const QRectF& box, const QColor& color)
{
    painter.save();
    painter.translate(box.topLeft());
    painter.scale(box.width() / 34.0, box.height() / 34.0);
    QPen pen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    if (kind == RoleCard::Kind::Server) {
        // a monitor over a keyboard
        painter.drawRoundedRect(QRectF(4, 4, 26, 16), 2, 2);
        painter.drawLine(QPointF(13, 24), QPointF(21, 24));
        painter.drawLine(QPointF(17, 20), QPointF(17, 24));
        painter.drawRoundedRect(QRectF(5, 27, 24, 4), 1, 1);
        for (int x = 9; x <= 25; x += 4)
            painter.drawPoint(QPointF(x, 29));
    } else {
        // a monitor with an arrow coming into it
        painter.drawRoundedRect(QRectF(9, 6, 22, 16), 2, 2);
        painter.drawLine(QPointF(16, 27), QPointF(24, 27));
        painter.drawLine(QPointF(20, 22), QPointF(20, 27));
        painter.drawLine(QPointF(2, 14), QPointF(13, 14));
        QPainterPath head;
        head.moveTo(9.5, 10.5);
        head.lineTo(13, 14);
        head.lineTo(9.5, 17.5);
        painter.drawPath(head);
    }
    painter.restore();
}

const int kCardPadding = 12;
const int kCardPicture = 30;
const int kCardGap = 12;

} // namespace

bool isDarkPalette(const QPalette& palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

QColor toneColor(Tone tone, const QPalette& palette)
{
    const bool dark = isDarkPalette(palette);
    switch (tone) {
    case Tone::Ok: return dark ? QColor(0x4A, 0xDE, 0x80) : QColor(0x15, 0x80, 0x3D);
    case Tone::Warn: return dark ? QColor(0xFB, 0xBF, 0x24) : QColor(0xB4, 0x53, 0x09);
    case Tone::Bad: return dark ? QColor(0xF8, 0x71, 0x71) : QColor(0xB9, 0x1C, 0x1C);
    case Tone::Idle: break;
    }
    return mutedTextColor(palette);
}

QColor mutedTextColor(const QPalette& palette)
{
    return blend(palette.color(QPalette::WindowText), palette.color(QPalette::Window), 0.38);
}

QFont monospaceFont(const QFont& base)
{
    QFont font = base;
    font.setFamilies({QStringLiteral("Consolas"), QStringLiteral("Cascadia Mono"), QStringLiteral("Menlo"),
                      QStringLiteral("DejaVu Sans Mono")});
    font.setStyleHint(QFont::Monospace);
    return font;
}

void markMuted(QLabel* label)
{
    label->setProperty(kMuted, true);
    // the window's colours, which a change of theme reaches first
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, mutedTextColor(label->window()->palette()));
    label->setPalette(palette);
}

void applyMutedText(QWidget* root)
{
    const auto labels = root->findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->property(kMuted).toBool())
            markMuted(label);
    }
}

void makeSectionTitle(QLabel* label)
{
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * 1.25);
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
}

StatusBanner::StatusBanner(QWidget* parent) :
    QFrame(parent)
{
}

void StatusBanner::paintEvent(QPaintEvent* event)
{
    QFrame::paintEvent(event);
    QPainter painter(this);
    const QPalette& pal = palette();
    painter.fillRect(rect(), blend(pal.color(QPalette::Window), pal.color(QPalette::WindowText), 0.05));
    painter.setPen(blend(pal.color(QPalette::Window), pal.color(QPalette::WindowText), 0.14));
    painter.drawLine(rect().bottomLeft(), rect().bottomRight());
}

StatusDot::StatusDot(QWidget* parent) :
    QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void StatusDot::setTone(Tone tone)
{
    m_Tone = tone;
    update();
}

QSize StatusDot::sizeHint() const
{
    const int size = fontMetrics().height() * 3 / 4;
    return QSize(size, size);
}

void StatusDot::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color = toneColor(m_Tone, palette());
    const QRectF box = QRectF(rect()).adjusted(1, 1, -1, -1);
    // a soft ring, as the tray icon's badge has
    QColor ring = color;
    ring.setAlphaF(0.25);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ring);
    painter.drawEllipse(box);
    painter.setBrush(color);
    painter.drawEllipse(box.adjusted(box.width() * 0.2, box.height() * 0.2, -box.width() * 0.2, -box.height() * 0.2));
}

CheckMark::CheckMark(Tone tone, QWidget* parent) :
    QWidget(parent),
    m_Tone(tone)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QSize CheckMark::sizeHint() const
{
    const int size = fontMetrics().height();
    return QSize(size, size);
}

void CheckMark::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color = toneColor(m_Tone, palette());
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    QColor ground = color;
    ground.setAlphaF(isDarkPalette(palette()) ? 0.22 : 0.14);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ground);
    painter.drawEllipse(box);

    // the mark, on a 16 by 16 grid
    painter.translate(box.topLeft());
    painter.scale(box.width() / 16.0, box.height() / 16.0);
    painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    switch (m_Tone) {
    case Tone::Ok: {
        QPainterPath tick;
        tick.moveTo(4.5, 8.3);
        tick.lineTo(7, 10.8);
        tick.lineTo(11.5, 5.5);
        painter.drawPath(tick);
        break;
    }
    case Tone::Warn:
        painter.drawLine(QPointF(8, 4.5), QPointF(8, 8.8));
        painter.drawPoint(QPointF(8, 11.4));
        break;
    case Tone::Bad:
        painter.drawLine(QPointF(5.5, 5.5), QPointF(10.5, 10.5));
        painter.drawLine(QPointF(10.5, 5.5), QPointF(5.5, 10.5));
        break;
    case Tone::Idle:
        painter.drawPoint(QPointF(8, 8));
        break;
    }
}

Chip::Chip(const QString& text, Tone tone, QWidget* parent) :
    QLabel(text, parent),
    m_Tone(tone)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAlignment(Qt::AlignCenter);
}

QSize Chip::sizeHint() const
{
    const QFontMetrics metrics = fontMetrics();
    return QSize(metrics.horizontalAdvance(text()) + metrics.height(), metrics.height() + 4);
}

void Chip::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette& pal = palette();
    const bool neutral = m_Tone == Tone::Idle;
    QColor ground = neutral ? pal.color(QPalette::WindowText) : toneColor(m_Tone, pal);
    ground.setAlphaF(neutral ? 0.08 : (isDarkPalette(pal) ? 0.2 : 0.12));
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ground);
    painter.drawRoundedRect(box, box.height() / 2, box.height() / 2);
    painter.setPen(neutral ? pal.color(QPalette::WindowText) : toneColor(m_Tone, pal));
    painter.drawText(rect(), Qt::AlignCenter, text());
}

RoleCard::RoleCard(QWidget* parent) :
    QAbstractButton(parent)
{
    setCheckable(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

void RoleCard::setKind(Kind kind)
{
    m_Kind = kind;
    update();
}

void RoleCard::setDescription(const QString& description)
{
    m_Description = description;
    setAccessibleDescription(description);
    updateGeometry();
    update();
}

QFont RoleCard::titleFont() const
{
    QFont font = this->font();
    font.setWeight(QFont::DemiBold);
    font.setPointSizeF(font.pointSizeF() * 1.1);
    return font;
}

QRect RoleCard::textRect(const QRect& card) const
{
    return card.adjusted(kCardPadding + kCardPicture + kCardGap, kCardPadding, -kCardPadding, -kCardPadding);
}

int RoleCard::heightForWidth(int width) const
{
    const QRect text = textRect(QRect(0, 0, width, 1000));
    const int title = QFontMetrics(titleFont()).height();
    const int description = fontMetrics().boundingRect(QRect(0, 0, text.width(), 1000),
                                                       Qt::TextWordWrap, m_Description).height();
    return 2 * kCardPadding + std::max(kCardPicture, title + 2 + description);
}

QSize RoleCard::sizeHint() const
{
    const int width = 260;
    return QSize(width, heightForWidth(width));
}

QSize RoleCard::minimumSizeHint() const
{
    const int width = 180;
    return QSize(width, heightForWidth(width));
}

void RoleCard::nextCheckState()
{
    setChecked(true);
}

bool RoleCard::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::HoverEnter: m_Hovered = true; update(); break;
    case QEvent::HoverLeave: m_Hovered = false; update(); break;
    default: break;
    }
    return QAbstractButton::event(event);
}

void RoleCard::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette& pal = palette();
    const QColor accent = accentColor(pal);
    const QColor text = pal.color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::WindowText);
    const bool checked = isChecked();

    const QRectF card = QRectF(rect()).adjusted(1, 1, -1, -1);
    QColor ground = pal.color(QPalette::Base);
    if (checked)
        ground = blend(ground, accent, isDarkPalette(pal) ? 0.16 : 0.08);
    else if (m_Hovered && isEnabled())
        ground = blend(ground, pal.color(QPalette::WindowText), 0.03);
    QColor border = checked ? accent
                  : blend(pal.color(QPalette::Window), pal.color(QPalette::WindowText), m_Hovered ? 0.35 : 0.2);
    painter.setPen(QPen(border, checked ? 2 : 1));
    painter.setBrush(ground);
    painter.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);

    if (hasFocus()) {
        painter.setPen(QPen(pal.color(QPalette::WindowText), 1, Qt::DotLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(card.adjusted(3.5, 3.5, -3.5, -3.5), 4, 4);
    }

    drawRolePicture(painter, m_Kind, QRectF(kCardPadding, kCardPadding, kCardPicture, kCardPicture),
                    checked ? accent : text);

    const QRect area = textRect(rect());
    painter.setPen(text);
    painter.setFont(titleFont());
    const int title = QFontMetrics(titleFont()).height();
    painter.drawText(QRect(area.left(), area.top(), area.width(), title), Qt::AlignLeft | Qt::AlignVCenter,
                     this->text().remove('&'));
    painter.setFont(font());
    painter.setPen(isEnabled() ? mutedTextColor(pal) : text);
    painter.drawText(QRect(area.left(), area.top() + title + 2, area.width(), area.height() - title - 2),
                     Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, m_Description);
}
