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

#pragma once

#include <QAbstractButton>
#include <QFrame>
#include <QLabel>

class QPalette;
class QToolButton;

// Pieces of the main window drawn in the system's own colours, with the status colours from
// the style guide: green, amber and red, each a shade for light windows and one for dark.

enum class Tone { Ok, Warn, Bad, Idle };

bool isDarkPalette(const QPalette& palette);
QColor toneColor(Tone tone, const QPalette& palette);
// text that says less than the text around it, such as help under a field
QColor mutedTextColor(const QPalette& palette);
// the font for names, addresses and fingerprints
QFont monospaceFont(const QFont& base);

// Labels marked with markMuted() take mutedTextColor; applyMutedText() sets it on every one
// under root, and again whenever the system's colours change.
void markMuted(QLabel* label);
void applyMutedText(QWidget* root);
// a section heading such as "Connection"
void makeSectionTitle(QLabel* label);

// The band across the top of the window, behind the status dot, its words and the buttons.
class StatusBanner : public QFrame
{
    Q_OBJECT

public:
    explicit StatusBanner(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
};

// A dot in a tone's colour.
class StatusDot : public QWidget
{
    Q_OBJECT

public:
    explicit StatusDot(QWidget* parent = nullptr);
    void setTone(Tone tone);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Tone m_Tone = Tone::Idle;
};

// A circle with a tick, an exclamation mark, a cross or a dot, for the line of a check.
class CheckMark : public QWidget
{
    Q_OBJECT

public:
    explicit CheckMark(Tone tone, QWidget* parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Tone m_Tone;
};

// A short word on a rounded background in a tone's colour, such as "Connected".
class Chip : public QLabel
{
    Q_OBJECT

public:
    explicit Chip(const QString& text, Tone tone, QWidget* parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Tone m_Tone;
};

// A section whose heading opens and closes it, for what's needed less often.
class CollapsibleSection : public QWidget
{
    Q_OBJECT

public:
    explicit CollapsibleSection(QWidget* parent = nullptr);
    void setTitle(const QString& title);
    void setExpanded(bool expanded);
    bool isExpanded() const;
    // where the section's contents go
    QWidget* body() const { return m_Body; }

private:
    QToolButton* m_Toggle;
    QWidget* m_Body;
};

// One of the two big choices, Server or Client: a picture, the name in its text and a line
// about it. Checking it is what choosing it means; it can't be unchecked by clicking it.
class RoleCard : public QAbstractButton
{
    Q_OBJECT

public:
    enum class Kind { Server, Client };

    explicit RoleCard(QWidget* parent = nullptr);
    void setKind(Kind kind);
    void setDescription(const QString& description);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void nextCheckState() override;
    bool event(QEvent* event) override;

private:
    QRect textRect(const QRect& card) const;
    QFont titleFont() const;

    Kind m_Kind = Kind::Server;
    QString m_Description;
    bool m_Hovered = false;
};
