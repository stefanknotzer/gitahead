//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "TextEditor.h"
#include "app/Application.h"
#include "conf/Settings.h"
#include <QFocusEvent>
#include <QMainWindow>
#include <QScrollBar>
#include <QStyle>
#include <QWindow>

using namespace Scintilla;

extern LexerModule lmLPeg;

TextEditor::TextEditor(QWidget *parent)
  : ScintillaIFace(parent)
{
  // Load colors.
  Theme *theme = Application::theme();
  mOursColor = theme->diff(Theme::Diff::Ours);
  mTheirsColor = theme->diff(Theme::Diff::Theirs);
  mAdditionColor = theme->diff(Theme::Diff::Addition);
  mDeletionColor = theme->diff(Theme::Diff::Deletion);

  // Load icons.
  QStyle *style = this->style();
  mNoteIcon = style->standardIcon(QStyle::SP_MessageBoxInformation);
  mWarningIcon = style->standardIcon(QStyle::SP_MessageBoxWarning);
  mErrorIcon = style->standardIcon(QStyle::SP_MessageBoxCritical);

  // Register the LPeg lexer.
  static bool initialized = false;
  if (!initialized) {
    Catalogue::AddLexerModule(&lmLPeg);
    initialized = true;
  }

  setScrollWidth(256);
  setScrollWidthTracking(true);
  setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);

  setMarginLeft(4);
  setMarginTypeN(LineNumber, SC_MARGIN_NUMBER);
  setMarginTypeN(LineNumbers, SC_MARGIN_TEXT);
  setMarginTypeN(ErrorMargin, SC_MARGIN_SYMBOL);
  for (int i = 0; i <= SC_MAX_MARGIN; ++i) {
    setMarginWidthN(i, 0);
    setMarginMaskN(i, 0);
  }

  int mask = 0;
  for (int i = NoteMarker; i <= ErrorMarker; ++i)
    mask |= (1 << i);
  setMarginMaskN(ErrorMargin, mask);
  setMarginSensitiveN(ErrorMargin, true);

  setSelEOLFilled(true);
  setSelBack(true, palette().color(QPalette::Highlight));
  setVirtualSpaceOptions(SCVS_RECTANGULARSELECTION);

  // Unset default zoom in/out shortcuts.
  clearCmdKey(SCK_ADD + (SCI_CTRL << 16));
  clearCmdKey(SCK_SUBTRACT + (SCI_CTRL << 16));

  // Set find indicators.
  indicSetStyle(FindAll, INDIC_STRAIGHTBOX);
  indicSetFore(FindAll, Qt::white);
  indicSetAlpha(FindAll, 255);
  indicSetUnder(FindAll, true);

  indicSetStyle(FindCurrent, INDIC_STRAIGHTBOX);
  indicSetFore(FindCurrent, Qt::yellow);
  indicSetAlpha(FindCurrent, 255);
  indicSetUnder(FindCurrent, true);

  // Set word diff indicators.
  indicSetStyle(WordAddition, INDIC_STRAIGHTBOX);
  indicSetFore(WordAddition, theme->diff(Theme::Diff::WordAddition));
  indicSetAlpha(WordAddition, 255);
  indicSetUnder(WordAddition, true);

  indicSetStyle(WordDeletion, INDIC_STRAIGHTBOX);
  indicSetFore(WordDeletion, theme->diff(Theme::Diff::WordDeletion));
  indicSetAlpha(WordDeletion, 255);
  indicSetUnder(WordDeletion, true);

  indicSetStyle(NoteIndicator, INDIC_SQUIGGLE);
  indicSetFore(NoteIndicator, theme->diff(Theme::Diff::Note));
  indicSetAlpha(NoteIndicator, 255);
  indicSetUnder(NoteIndicator, true);

  indicSetStyle(WarningIndicator, INDIC_STRAIGHTBOX);
  indicSetFore(WarningIndicator, theme->diff(Theme::Diff::Warning));
  indicSetAlpha(WarningIndicator, 255);
  indicSetUnder(WarningIndicator, true);

  indicSetStyle(ErrorIndicator, INDIC_STRAIGHTBOX);
  indicSetFore(ErrorIndicator, theme->diff(Theme::Diff::Error));
  indicSetAlpha(ErrorIndicator, 255);
  indicSetUnder(ErrorIndicator, true);

  // Initialize LPeg lexer.
  QColor base = palette().color(QPalette::Base);
  QColor text = palette().color(QPalette::Text);
  bool dark = (text.lightnessF() > base.lightnessF());

  setLexerLanguage("lpeg");
  setProperty("lexer.lpeg.home", Settings::lexerDir().path());
  setProperty("lexer.lpeg.themes", theme->dir().path());
  setProperty("lexer.lpeg.theme", theme->name());
  setProperty("lexer.lpeg.theme.mode", dark ? "dark" : "light");
  setCaretFore(text);

  // Apply default settings.
  applySettings();
  connect(Settings::instance(), &Settings::settingsChanged,
          this, &TextEditor::applySettings);

  // Update geometry when the scroll bar becomes visible.
  connect(horizontalScrollBar(), &QScrollBar::rangeChanged,
          this, &TextEditor::updateGeometry);
}

void TextEditor::applySettings()
{
  // Set default font and size.
  Settings *settings = Settings::instance();
  QString family = settings->value("editor/font/family").toString();
  int pointSize = settings->value("editor/font/size").toInt();
  styleSetFont(STYLE_DEFAULT, QFont(family, pointSize));
  setUseTabs(settings->value("editor/indent/tabs").toBool());
  setIndent(settings->value("editor/indent/width").toInt());
  setTabWidth(settings->value("editor/indent/tabwidth").toInt());

  // Initialize markers.
  markerDefine(Context, SC_MARK_EMPTY);
  markerDefine(Ours, SC_MARK_BACKGROUND);
  markerDefine(Theirs, SC_MARK_BACKGROUND);
  markerDefine(Addition, SC_MARK_BACKGROUND);
  markerDefine(Deletion, SC_MARK_BACKGROUND);

  markerSetBack(Ours, mOursColor);
  markerSetBack(Theirs, mTheirsColor);
  markerSetBack(Addition, mAdditionColor);
  markerSetBack(Deletion, mDeletionColor);

  // Initialize error markers.
  loadMarkerIcon(NoteMarker, mNoteIcon);
  loadMarkerIcon(WarningMarker, mWarningIcon);
  loadMarkerIcon(ErrorMarker, mErrorIcon);

  // Set LPeg lexer language.
  QByteArray lexer = this->lexer().toUtf8();
  uintptr_t ptr = reinterpret_cast<uintptr_t>(lexer.constData());
  privateLexerCall(SCI_GETDIRECTFUNCTION, directFunction());
  privateLexerCall(SCI_SETDOCPOINTER, directPointer());
  privateLexerCall(SCI_SETLEXERLANGUAGE, ptr);

  // Set annotation styles.
  QFont regular = font();
  QFont bold = regular;
  bold.setBold(true);
  QFont italic = regular;
  italic.setItalic(true);

  // Missing newline style.
  styleSetFont(EofNewline, italic);

  // Remote comment styles
  Theme *theme = Application::theme();
  styleSetFont(CommentBody, regular);
  styleSetFore(CommentBody, theme->remoteComment(Theme::Comment::Body));
  styleSetBack(CommentBody, theme->remoteComment(Theme::Comment::Background));

  styleSetFont(CommentAuthor, bold);
  styleSetFore(CommentAuthor, theme->remoteComment(Theme::Comment::Author));
  styleSetBack(CommentAuthor, theme->remoteComment(Theme::Comment::Background));

  styleSetFont(CommentTimestamp, regular);
  styleSetFore(CommentTimestamp, theme->remoteComment(Theme::Comment::Timestamp));
  styleSetBack(CommentTimestamp, theme->remoteComment(Theme::Comment::Background));

  // Emit own signal.
  emit settingsChanged();

  // Size hint may have changed.
  updateGeometry();
}

QString TextEditor::lexer() const
{
  return Settings::instance()->lexer(mPath);
}

void TextEditor::setLineCount(int lines)
{
  mLineCount = lines;
}

void TextEditor::setLexer(const QString &path)
{
  mPath = path;
  applySettings();
}

void TextEditor::load(const QString &path, const QString &text)
{
  setScrollWidth(256);
  setLexer(path);
  setText(text);

  // Clear undo.
  setSavePoint();
  emptyUndoBuffer();

  // Notify layout of size change.
  updateGeometry();
}

void TextEditor::clearHighlights()
{
  setIndicatorCurrent(FindAll);
  indicatorClearRange(0, length());

  setIndicatorCurrent(FindCurrent);
  indicatorClearRange(0, length());

  // Restore styles.
  applySettings();

  emit highlightActivated(false);
}

int TextEditor::highlightAll(const QString &text)
{
  clearHighlights();
  if (text.isEmpty())
    return 0;

  // Darken styles.
  markerSetBack(Addition, mAdditionColor.darker(120));
  markerSetBack(Deletion, mDeletionColor.darker(120));
  markerSetBack(Ours, mOursColor.darker(120));
  markerSetBack(Theirs, mTheirsColor.darker(120));
  for (int i = 0; i <= STYLE_DEFAULT; i++)
    styleSetBack(i, styleBack(i).darker(120));

  emit highlightActivated(true);

  setIndicatorCurrent(FindAll);

  QByteArray utf8 = text.toUtf8();
  const char *data = utf8.constData();

  int matches = 0;
  int max = length();
  QPair<int,int> match = findText(0, data, 0, max);
  while (match.first >= 0) {
    // Search again from the end of the last range.
    indicatorFillRange(match.first, match.second - match.first);
    match = findText(0, data, match.second, max);
    ++matches;
  }

  return matches;
}

int TextEditor::find(const QString &text, bool forward, bool indicator)
{
  QByteArray utf8 = text.toUtf8();
  const char *data = utf8.constData();

  searchAnchor();
  int pos = forward ? searchNext(0, data) : searchPrev(0, data);

  if (pos >= 0)
    scrollCaret();

  if (indicator) {
    // Clear previous indicator.
    setIndicatorCurrent(FindCurrent);
    indicatorClearRange(0, length());

    if (pos >= 0) {
      int start = selectionStart();
      indicatorFillRange(start, selectionEnd() - start);
    }
  }

  return pos;
}

QList<TextEditor::Diagnostic> TextEditor::diagnostics(int line)
{
  return mDiagnostics.value(line);
}

void TextEditor::addDiagnostic(int line, const Diagnostic &diag)
{
  int marker;
  int indicator;
  switch (diag.kind) {
    case Note:
      marker = NoteMarker;
      indicator = NoteIndicator;
      break;

    case Warning:
      marker = WarningMarker;
      indicator = WarningIndicator;
      break;

    case Error:
      marker = ErrorMarker;
      indicator = ErrorIndicator;
      break;
  }

  // Add indictator.
  setIndicatorCurrent(indicator);
  indicatorFillRange(positionFromLine(line) + diag.range.pos, diag.range.len);

  // Add marker.
  int height = textHeight(line);
  setMarginWidthN(ErrorMargin, height + 4);

  int current = diagnosticMarker(line);
  if (current >= 0)
    markerDelete(line, current);
  markerAdd(line, qMax(marker, current));

  // Remember diagnostic.
  mDiagnostics[line].append(diag);

  // Signal addition.
  emit diagnosticAdded(line, diag);
}

QSize TextEditor::viewportSizeHint() const
{
  // Return placeholder size if the content isn't loaded.
  QSize size = ScintillaIFace::viewportSizeHint();
  if (length() == 0 && mLineCount >= 0) {
    int height = const_cast<TextEditor *>(this)->textHeight(0);
    return QSize(size.width(), mLineCount * height);
  }

  // Total height is the y position of the start of the last line
  // plus the height of the line itself and any annotation lines.
  // Documents always have at least one line by definition.
  int line = lineCount() - 1;
  int lines = annotationLines(line) + 1;
  int height = const_cast<TextEditor *>(this)->textHeight(line);
  int y = const_cast<TextEditor *>(this)->pointFromPosition(length()).y();

  // Windows: (empty) space for the scroll bar is always visible,
  // even when the scroll bar is not visible.
  // To avoid wasting pixels a negative height is set.
#ifdef Q_OS_WIN
  int scrollBarHeight = -horizontalScrollBar()->height();
#else
  int scrollBarHeight = 0;
#endif

  // Horizontal scroll bar is visible.
  if (horizontalScrollBar()->maximum() > 0)
    scrollBarHeight += horizontalScrollBar()->height();

  return QSize(size.width(), y + (lines * height) + scrollBarHeight);
}

int TextEditor::diagnosticMarker(int line)
{
  int marks = markers(line);
  if (marks & (1 << NoteMarker))
    return NoteMarker;

  if (marks & (1 << WarningMarker))
    return WarningMarker;

  if (marks & (1 << ErrorMarker))
    return ErrorMarker;

  return -1;
}

void TextEditor::loadMarkerIcon(Marker marker, const QIcon &icon)
{
  qreal dpr = 1.0;
  if (QWidget *window = this->window()) {
    if (QWindow *handle = window->windowHandle())
      dpr = handle->devicePixelRatio();
  }

  qreal height = textHeight(0);
  qreal scaled = height * dpr;

  QPixmap pixmap = icon.pixmap(height, height);
  pixmap.setDevicePixelRatio(dpr);
  QPixmap scaledPixmap = pixmap.scaled(
    scaled, scaled, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  markerDefineImage(marker, scaledPixmap.toImage());
}
