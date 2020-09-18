//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "DiffView.h"
#include "Badge.h"
#include "BlameEditor.h"
#include "EditorWindow.h"
#include "FileContextMenu.h"
#include "MenuBar.h"
#include "RepoView.h"
#include "app/Application.h"
#include "conf/Settings.h"
#include "git/Blame.h"
#include "git/Blob.h"
#include "git/Branch.h"
#include "git/Buffer.h"
#include "git/Commit.h"
#include "git/Config.h"
#include "git/FilterList.h"
#include "git/Index.h"
#include "git/Patch.h"
#include "git/Repository.h"
#include "git/Signature.h"
#include "git/Submodule.h"
#include "git/Tree.h"
#include "git2/diff.h"
#include "git2/index.h"
#include "log/LogEntry.h"
#include <QCheckBox>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QShortcut>
#include <QStyleOption>
#include <QTableWidget>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QTextLayout>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>
#include <QMimeDatabase>
#include <QTemporaryFile>

namespace {

const int kIndent = 2;

const int kArrowWidth = 9;
const int kArrowHeight = 8;

const QString kHunkFmt = "<h4>%1</h4>";

const QString kStyleSheet =
  "DiffView {"
  "  border-image: url(:/sunken.png) 4 4 4 4;"
  "  border-left-width: 4;"
  "  border-top-width: 4;"
  "  padding-left: -4;"
  "  padding-top: -4"
  "}"
  "DiffView FileWidget {"
  "  background-clip: content;"
  "  border-image: url(:/shadow.png) 8 8 8 8;"
  "  border-width: 8;"
  "  padding-left: -2;"
  "  padding-top: -2"
  "}";

const QString kButtonStyleFmt =
  "QToolButton {"
  "  background: %1"
  "}"
  "QToolButton:pressed {"
  "  background: %2"
  "}";

// Binary iconsize and pixmapsize definition.
const QSize kBinaryIconsize = QSize(64, 64);
const QSize kBinaryPicturesize = QSize(128, 64);
const uint64_t kBinaryPictureFilesize = 1024 * 1024 * 1024;

const QString kBinaryStyleFmt =
  "QLabel {"
  "  margin: 2px;"
  "  color: %1;"
  "  background: %2;"
  "  font-family: %3;"
  "  font-size: %4pt;"
  "}";

const QDir::Filters kFilters =
  QDir::Files |
  QDir::Dirs |
  QDir::Hidden |
  QDir::NoDotAndDotDot;

const QUrl::FormattingOptions kUrlFormat =
  QUrl::PreferLocalFile |
  QUrl::StripTrailingSlash |
  QUrl::NormalizePathSegments;

bool copy(const QString &source, const QDir &targetDir)
{
  // Disallow copy into self.
  if (source.startsWith(targetDir.path()))
    return false;

  QFileInfo info(source);
  QString name = info.fileName();
  QString target = targetDir.filePath(name);
  if (!info.isDir())
    return QFile::copy(source, target);

  if (!targetDir.mkdir(name))
    return false;

  foreach (const QFileInfo &entry, QDir(source).entryInfoList(kFilters)) {
    if (!copy(entry.filePath(), target))
      return false;
  }

  return true;
}

QString buttonStyle(Theme::Diff role)
{
  Theme *theme = Application::theme();
  QColor color = theme->diff(role);
  QString pressed = color.darker(115).name();
  return kButtonStyleFmt.arg(color.name(), pressed);
}

struct Annotation
{
  QString text;
  QByteArray styles;
};

class Comment : public QTextEdit
{
public:
  Comment(
    const QDateTime &date,
    const Account::Comment &comment,
    QWidget *parent = nullptr)
    : QTextEdit(parent)
  {
    setReadOnly(true);
    setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QTextCursor cursor = textCursor();
    Theme *theme = Application::theme();

    QTextCharFormat author;
    author.setForeground(theme->remoteComment(Theme::Comment::Author));
    author.setFontWeight(QFont::Bold);
    cursor.setCharFormat(author);
    cursor.insertText(comment.author + " ");

    QTextCharFormat timestamp;
    timestamp.setForeground(theme->remoteComment(Theme::Comment::Timestamp));
    cursor.setCharFormat(timestamp);
    cursor.insertText(date.toString(Qt::DefaultLocaleLongDate));

    QTextBlockFormat indent;
    indent.setLeftMargin(fontMetrics().horizontalAdvance(' ') * kIndent);
    cursor.insertBlock(indent);

    QTextCharFormat body;
    body.setForeground(theme->remoteComment(Theme::Comment::Body));
    cursor.setCharFormat(body);
    cursor.insertText(comment.body);

    connect(verticalScrollBar(), &QScrollBar::rangeChanged, [this] {
      updateGeometry();
    });
  }

  QSize minimumSizeHint() const override
  {
    return QSize();
  }

  QSize viewportSizeHint() const override
  {
    return document()->size().toSize();
  }
};

class CommentWidget : public QWidget
{
public:
  CommentWidget(const Account::Comments &comments, QWidget *parent = nullptr)
    : QWidget(parent)
  {
    setStyleSheet("QWidget { background-color: transparent; border: none }");
    setContentsMargins(4,4,4,4);

    QVBoxLayout *layout = new QVBoxLayout(this);
    foreach (const QDateTime &key, comments.keys())
      layout->addWidget(new Comment(key, comments.value(key), this));
  }
};

class Line
{
public:
  Line(char origin, int oldLine, int newLine)
    : mOrigin(origin)
  {
    mOldLine = (oldLine >= 0) ? QByteArray::number(oldLine) : QByteArray();
    mNewLine = (newLine >= 0) ? QByteArray::number(newLine) : QByteArray();
  }

  char origin() const { return mOrigin; }
  QByteArray oldLine() const { return mOldLine; }
  QByteArray newLine() const { return mNewLine; }

  bool newline() const { return mNewline; }
  void setNewline(bool newline) { mNewline = newline; }

  int matchingLine() const { return mMatchingLine; }
  void setMatchingLine(int line) { mMatchingLine = line; }

private:
  char mOrigin = -1;
  bool mNewline = true;
  int mMatchingLine = -1;
  QByteArray mOldLine;
  QByteArray mNewLine;
};

class Button : public QToolButton
{
public:
  Button(QWidget *parent = nullptr)
    : QToolButton(parent)
  {}

  QSize sizeHint() const override
  {
    QStyleOptionToolButton opt;
    initStyleOption(&opt);

    // Start with the check box dimensions.
    int width = style()->pixelMetric(QStyle::PM_IndicatorWidth, &opt, this);
    int height = style()->pixelMetric(QStyle::PM_IndicatorHeight, &opt, this);
    return QSize(width + 2, height + 4);
  }

protected:
  void initButtonPainter(QPainter *painter)
  {
    painter->setRenderHint(QPainter::Antialiasing);

    QPen pen = painter->pen();
    pen.setWidth(2);
    painter->setPen(pen);
  }
};

class EditButton : public Button
{
public:
  EditButton(
    const git::Patch &patch,
    int index,
    bool binary,
    bool lfs,
    QWidget *parent = nullptr)
    : Button(parent)
  {
    setObjectName("EditButton");

    // Add edit button menu.
    QMenu *menu = new QMenu(this);

    QString name = patch.name();
    RepoView *view = RepoView::parentView(this);

    // Calculate starting line numbers.
    int oldLine = -1;
    int newLine = -1;
    if (index >= 0 && patch.lineCount(index) > 0) {
      oldLine = patch.lineNumber(index, 0, git::Diff::OldFile);
      newLine = patch.lineNumber(index, 0, git::Diff::NewFile);
    }

    if (view->repo().workdir().exists(name)) {
      QAction *action = menu->addAction("Edit Working Copy");
      connect(action, &QAction::triggered, [this, view, name, newLine] {
        view->edit(name, newLine);
      });
    }

    // Add revision edit actions.
    QList<git::Commit> commits = view->commits();
    git::Commit commit = !commits.isEmpty() ? commits.first() : git::Commit();
    git::Blob newBlob = patch.blob(git::Diff::NewFile);
    if (newBlob.isValid()) {
      QAction *action = menu->addAction("Edit New Revision");
      connect(action, &QAction::triggered,
      [this, view, name, newLine, newBlob, commit] {
        view->openEditor(name, newLine, newBlob, commit);
      });
    }

    git::Blob oldBlob = patch.blob(git::Diff::OldFile);
    if (oldBlob.isValid()) {
      if (commit.isValid()) {
        QList<git::Commit> parents = commit.parents();
        if (!parents.isEmpty())
          commit = parents.first();
      }

      QAction *action = menu->addAction("Edit Old Revision");
      connect(action, &QAction::triggered,
      [this, view, name, oldLine, oldBlob, commit] {
        view->openEditor(name, oldLine, oldBlob, commit);
      });
    }

    // Connect button click to the first menu action.
    setEnabled(!menu->isEmpty() && !binary && !lfs);
    connect(this, &EditButton::clicked, [menu] {
      menu->actions().first()->trigger();
    });

    setMenu(menu);
  }

protected:
  void paintEvent(QPaintEvent *event) override
  {
    QPainter painter(this);
    initButtonPainter(&painter);

    qreal r = 6;
    qreal x = width() / 2;
    qreal y = height() / 2;

    QPainterPath path;
    path.moveTo(x - r, y + r);
    path.lineTo(x - r, y + r - 4);
    path.lineTo(x + r - 4, y - r);
    path.lineTo(x + r, y - r + 4);
    path.lineTo(x - r + 4, y + r);
    path.lineTo(x - r, y + r);
    painter.drawPath(path);

    painter.setPen(QPen(painter.pen().color(), 1, Qt::SolidLine, Qt::FlatCap));
    painter.drawLine(x - r + 1, y + r - 5, x - r + 5, y + r - 1);
    painter.drawLine(x + r - 6, y - r + 2, x + r - 2, y - r + 6);
  }
};

class DiscardButton : public Button
{
public:
  DiscardButton(QWidget *parent = nullptr)
    : Button(parent)
  {
    setObjectName("DiscardButton");
  }

protected:
  void paintEvent(QPaintEvent *event) override
  {
    QPainter painter(this);
    initButtonPainter(&painter);

    qreal r = 4;
    qreal x = width() / 2;
    qreal y = height() / 2;

    QPainterPath path;
    path.moveTo(x - r, y - r);
    path.lineTo(x + r, y + r);
    path.moveTo(x + r, y - r);
    path.lineTo(x - r, y + r);
    painter.drawPath(path);
  }
};

class DisclosureButton : public Button
{
public:
  DisclosureButton(QWidget *parent = nullptr)
    : Button(parent)
  {
    setCheckable(true);
    setChecked(true);
  }

protected:
  void paintEvent(QPaintEvent *event) override
  {
    QPainter painter(this);
    initButtonPainter(&painter);

    qreal x = width() / 2;
    qreal y = height() / 2;

    QPainterPath path;
    if (isChecked()) {
      path.moveTo(x - 3.5, y - 2);
      path.lineTo(x, y + 2);
      path.lineTo(x + 3.5, y - 2);
    } else {
      path.moveTo(x - 2, y + 3.5);
      path.lineTo(x + 2, y);
      path.lineTo(x - 2, y - 3.5);
    }

    painter.drawPath(path);
  }
};

class Editor : public TextEditor
{
public:
  Editor(QWidget *parent = nullptr)
    : TextEditor(parent)
  {}

protected:
  void focusOutEvent(QFocusEvent *event) override
  {
    if (event->reason() != Qt::PopupFocusReason)
      clearSelections();

    TextEditor::focusOutEvent(event);
  }
};

class Arrow : public QFrame
{
public:
  Arrow(QWidget *parent = nullptr)
    : QFrame(parent)
  {
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Minimum);
  }

  QSize sizeHint() const override
  {
    return QSize(kArrowWidth, kArrowHeight);
  }

protected:
  void paintEvent(QPaintEvent *event) override
  {
    Q_UNUSED(event);

    // Draw arrow.
    QPainter painter(this);
    QRect rect = this->rect();

    int x1 = rect.left();
    int x2 = rect.right();
    int y = rect.height() / 2;

    QPainterPath path;
    path.moveTo(x1, y);
    path.lineTo(x2, y);
    path.moveTo(x2 - 3, y - 3);
    path.lineTo(x2, y);
    path.lineTo(x2 - 3, y + 3);

    QPen pen = painter.pen();
    pen.setWidthF(1.5);
    painter.setPen(pen);
    painter.drawPath(path);
  }
};

class ImageWidget : public QFrame
{
  Q_OBJECT

public:
  class Header : public QFrame
  {
  public:
    Header(
      QWidget *parent = nullptr)
      : QFrame(parent)
    {
      setObjectName("ImageHeader");
      QLabel *label = new QLabel(kHunkFmt.arg(ImageWidget::tr("Binary Picture/Icon")), this);

      mButton = new DisclosureButton(this);
      mButton->setToolTip(
        mButton->isChecked() ? ImageWidget::tr("Collapse Picture/Icon") : ImageWidget::tr("Expand Picture/Icon"));
      connect(mButton, &DisclosureButton::toggled, [this] {
        mButton->setToolTip(
          mButton->isChecked() ? ImageWidget::tr("Collapse Picture/Icon") : ImageWidget::tr("Expand Picture/Icon"));
      });

      QHBoxLayout *layout = new QHBoxLayout(this);
      layout->setContentsMargins(4,4,4,4);
      layout->addWidget(label);
      layout->addStretch();
      layout->addWidget(mButton);
    }

    DisclosureButton *button() const { return mButton; }

  protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
      Q_UNUSED(event);

      if (mButton->isEnabled())
        mButton->toggle();
    }

  private:
    DisclosureButton *mButton;
  };

  class Image : public QFrame
  {
  public:
    Image(
      QPixmap &oldpix,
      QSize &oldsize,
      QMimeType &oldmime,
      QPixmap &newpix,
      QSize &newsize,
      QMimeType &newmime,
      QWidget *parent = nullptr)
      : QFrame(parent)
    {
      // Calculate visibility.
      bool sameimage = (oldpix.toImage() == newpix.toImage());

      // Set default font and size.
      Settings *settings = Settings::instance();
      QString fontfamily = settings->value("editor/font/family").toString();
      QString fontsize = settings->value("editor/font/size").toString();

      QHBoxLayout *layout = new QHBoxLayout(this);
      layout->setContentsMargins(4,2,4,2);
      layout->setSpacing(4);

      // Add old pixmap and size.
      if (!oldpix.isNull()) {
        QVBoxLayout *pixlay = new QVBoxLayout();
        pixlay->setSpacing(4);
        QLabel *pixlabel = new QLabel(this);
        pixlabel->setPixmap(oldpix);
        pixlay->addWidget(pixlabel, 0, Qt::AlignHCenter);

        if (!oldsize.isEmpty()) {
          QLabel *sizelabel = new QLabel(this);
          if (sameimage)
            sizelabel->setStyleSheet(kBinaryStyleFmt.arg(palette().color(QPalette::BrightText).name(),
                                                         "",
                                                         fontfamily, fontsize));
          else
            sizelabel->setStyleSheet(kBinaryStyleFmt.arg(palette().color(QPalette::BrightText).name(),
                                                         Application::theme()->diff(Theme::Diff::Deletion).name(),
                                                         fontfamily, fontsize));

          sizelabel->setText(QString::number(oldsize.width()) + " x " + QString::number(oldsize.height()));
          pixlay->addWidget(sizelabel, 0, Qt::AlignHCenter);
        }
        if (oldmime.isValid()) {
          QLabel *mimelabel = new QLabel(this);
          if (sameimage)
            mimelabel->setStyleSheet(kBinaryStyleFmt.arg(palette().color(QPalette::BrightText).name(),
                                                         "",
                                                         fontfamily, fontsize));
          else
            mimelabel->setStyleSheet(kBinaryStyleFmt.arg(palette().color(QPalette::BrightText).name(),
                                                         Application::theme()->diff(Theme::Diff::Deletion).name(),
                                                         fontfamily, fontsize));

          mimelabel->setText(oldmime.name());
          pixlay->addWidget(mimelabel, 0, Qt::AlignHCenter);
        }
        layout->addItem(pixlay);
      }

      // Add arrow.
      if (!oldpix.isNull() && !newpix.isNull() && !sameimage)
        layout->addWidget(new Arrow(this), 0, Qt::AlignVCenter);

      // Add new pixmap and size.
      if (!newpix.isNull() && !sameimage) {
        QVBoxLayout *pixlay = new QVBoxLayout();
        pixlay->setSpacing(4);
        QLabel *pixlabel = new QLabel(this);
        pixlabel->setPixmap(newpix);
        pixlay->addWidget(pixlabel, 0, Qt::AlignHCenter);

        if (!newsize.isEmpty()) {
          QLabel *sizelabel = new QLabel(this);
          sizelabel->setStyleSheet(kBinaryStyleFmt.arg(palette().color(QPalette::BrightText).name(),
                                                       Application::theme()->diff(Theme::Diff::Addition).name(),
                                                       fontfamily, fontsize));
          sizelabel->setText(QString::number(newsize.width()) + " x " + QString::number(newsize.height()));
          pixlay->addWidget(sizelabel, 0, Qt::AlignHCenter);
        }
        if (newmime.isValid()) {
          QLabel *mimelabel = new QLabel(this);
          mimelabel->setStyleSheet(kBinaryStyleFmt.arg(palette().color(QPalette::BrightText).name(),
                                                       Application::theme()->diff(Theme::Diff::Addition).name(),
                                                       fontfamily, fontsize));

          mimelabel->setText(newmime.name());
          pixlay->addWidget(mimelabel, 0, Qt::AlignHCenter);
        }
        layout->addItem(pixlay);
      }

      layout->addStretch();
    }
  };

  ImageWidget(
    const git::Patch &patch,
    bool lfs,
    bool scaled,
    QWidget *parent = nullptr)
    : QFrame(parent)
  {
    setObjectName("ImageWidget");
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(0);

    mHeader = new Header(this);
    layout->addWidget(mHeader);

    QSize oldsize(0, 0);
    QSize newsize(0, 0);
    QMimeType oldmime, newmime;

    // Load pixmaps.
    QPixmap oldpix = loadPixmap(patch, git::Diff::OldFile, oldsize, oldmime, lfs, scaled);
    QPixmap newpix = loadPixmap(patch, git::Diff::NewFile, newsize, newmime, lfs, scaled);

    mImage = new Image(oldpix, oldsize, oldmime,
                       newpix, newsize, newmime);
    layout->addWidget(mImage);
    connect(mHeader->button(), &DisclosureButton::toggled,
            mImage, &QWidget::setVisible);
  }

  DisclosureButton *button() const { return mHeader->button(); }

private:
  QPixmap loadPixmap(const git::Patch &patch, git::Diff::File type, QSize &size, QMimeType &mime, bool lfs, bool scaled)
  {
    QPixmap pixmap;
    git::Blob blob = patch.blob(type);

    if (blob.isValid()) {
      QByteArray data = blob.content();

      // LFS smudge will fail on very large files:
      // QByteArray can only hold up to 2^31 bytes (2 GB).
      if (lfs)
        data = patch.repo().lfsSmudge(data, patch.name());

      if ((unsigned)data.size() < kBinaryPictureFilesize) {
        // Load pixmap from blob data.
        pixmap.loadFromData(data);
        if (!pixmap.isNull()) {
          size = pixmap.size();
          mime = QMimeDatabase().mimeTypeForData(data);
          if (scaled)
            return pixmap.scaled(kBinaryPicturesize, Qt::KeepAspectRatio);
          else
            return pixmap;
        }

        // Write blob data to temporary file and
        // load icon from blob data.
        QTemporaryFile file;
        file.open();
        file.write(data);

        QString path = file.fileName();
        loadIcon(pixmap, path, mime, scaled);

        file.close();
      } else {
        // Load default icon.
        QString path = "";
        loadIcon(pixmap, path, mime, scaled);
      }

      if (scaled)
        return pixmap.scaled(kBinaryIconsize, Qt::KeepAspectRatio);
      else
        return pixmap;
    }

    QString path = patch.repo().workdir().filePath(patch.name());
    uint64_t filesize = QFileInfo(path).size();

    if ((type == git::Diff::OldFile) || (filesize == 0)) {
        // Return empty pixmap, file does not exist.
        return pixmap;
    } else if (filesize < kBinaryPictureFilesize) {
      // Load pixmap from file.
      pixmap.load(path);
      if (!pixmap.isNull()) {
        size = pixmap.size();
        mime = QMimeDatabase().mimeTypeForFile(path);
        if (scaled)
          return pixmap.scaled(kBinaryPicturesize, Qt::KeepAspectRatio);
        else
          return pixmap;
      }

      // Load icon for file.
      loadIcon(pixmap, path, mime, scaled);
    } else {
      // Load default icon.
      path = "";
      loadIcon(pixmap, path, mime, scaled);
    }

    if (scaled)
      return pixmap.scaled(kBinaryIconsize, Qt::KeepAspectRatio);
    else
      return pixmap;
  }

  void loadIcon(QPixmap &pixmap, QString &path, QMimeType &mime, bool scaled)
  {
    QFileIconProvider provider;
    QIcon icon;

    if (QFileInfo(path).exists()) {
      // Load icon for file.
      icon = provider.icon(QFileInfo(path));
      mime = QMimeDatabase().mimeTypeForFile(path);
    } else {
      // Set generic icon.
      icon = provider.icon(QFileIconProvider::File);
      mime = QMimeDatabase().mimeTypeForName("binary");
    }

    if (scaled) {
      // Windows: Icons are provided 16 x 16 and 32 x 32
      // upscale / downscale icon if needed.
      pixmap = icon.pixmap(windowHandle(), QSize(32, 32));
    } else {
      pixmap = icon.pixmap(windowHandle(), QSize(64, 64));
    }
  }

  Header *mHeader;
  Image *mImage;
};

class InfoWidget : public QFrame
{
  Q_OBJECT

public:
  class Header : public QFrame
  {
  public:
    Header(
      bool binary,
      bool lfs,
      QWidget *parent = nullptr)
      : QFrame(parent)
    {
      setObjectName("InfoHeader");
      QLabel *label = new QLabel(this);
      if (binary)
        label->setText(kHunkFmt.arg(InfoWidget::tr("Binary File Information")));
      else if (lfs)
        label->setText(kHunkFmt.arg(InfoWidget::tr("Large File Information")));
      else
        label->setText(kHunkFmt.arg(InfoWidget::tr("Text File Information")));

      mButton = new DisclosureButton(this);
      mButton->setToolTip(
        mButton->isChecked() ? InfoWidget::tr("Collapse File Info") : InfoWidget::tr("Expand File Info"));
      connect(mButton, &DisclosureButton::toggled, [this] {
        mButton->setToolTip(
          mButton->isChecked() ? InfoWidget::tr("Collapse File Info") : InfoWidget::tr("Expand File Info"));
      });

      QHBoxLayout *layout = new QHBoxLayout(this);
      layout->setContentsMargins(4,4,4,4);
      layout->addWidget(label);
      layout->addStretch();
      layout->addWidget(mButton);
    }

    DisclosureButton *button() const { return mButton; }

  protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
      Q_UNUSED(event);

      if (mButton->isEnabled())
        mButton->toggle();
    }

  private:
    DisclosureButton *mButton;
  };

  InfoWidget(
    const git::Diff &diff,
    const git::Patch &patch,
    bool binary,
    bool lfs,
    QWidget *parent = nullptr)
    : QFrame(parent)
  {
    setObjectName("InfoWidget");
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(0);

    int index = diff.indexOf(patch.name());

    mHeader = new Header(binary, lfs, this);
    layout->addWidget(mHeader);

    mEditor = new Editor(this);
    mEditor->setCaretStyle(CARETSTYLE_INVISIBLE);
    mEditor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    connect(mEditor, &TextEditor::updateUi,
            MenuBar::instance(this), &MenuBar::updateCutCopyPaste);

    // Ensure that text margin reacts to settings changes.
    connect(mEditor, &TextEditor::settingsChanged, [this] {
      int width = mEditor->textWidth(STYLE_LINENUMBER, mEditor->marginText(0));
      mEditor->setMarginWidthN(TextEditor::LineNumbers, width);
    });

    // Darken background when find highlight is active.
    connect(mEditor, &TextEditor::highlightActivated,
            this, &InfoWidget::setDisabled);

    // Disable vertical resize.
    mEditor->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    layout->addWidget(mEditor);
    connect(mHeader->button(), &DisclosureButton::toggled,
            mEditor, &TextEditor::setVisible);

    // Filesize and mode.
    git_diff_file oldfile = diff.oldFile(index);
    git_diff_file newfile = diff.newFile(index);
    git_filemode_t oldmode = static_cast<git_filemode_t>(oldfile.mode);
    git_filemode_t newmode = static_cast<git_filemode_t>(newfile.mode);
    uint64_t oldsize = oldfile.size;
    uint64_t newsize = newfile.size;
    uint64_t oldlfssize = 0;
    uint64_t newlfssize = 0;

    // Filesize of modified binary is not set.
    if ((newfile.mode != GIT_FILEMODE_UNREADABLE) && (newsize == 0)) {
      if (patch.isValid() && patch.blob(git::Diff::NewFile).isValid()) {
        newsize = patch.blob(git::Diff::NewFile).content().size();
      }
    }

    // Get lfs pointer size (added or modified file).
    if (lfs) {
      if ((patch.status() == GIT_DELTA_ADDED) ||
          (patch.status() == GIT_DELTA_MODIFIED) ||
          (patch.status() == GIT_DELTA_DELETED)) {
        // oldsize is already set,
        // newsize is calculated using patch data.
        newsize = 0;

        int hunkCount = patch.count();
        for (int idx = 0; idx < hunkCount; ++idx) {
          int patchCount = patch.lineCount(idx);
          for (int lidx = 0; lidx < patchCount; ++lidx) {
            char origin = patch.lineOrigin(idx, lidx);
            if (origin == GIT_DIFF_LINE_CONTEXT_EOFNL ||
                origin == GIT_DIFF_LINE_ADD_EOFNL ||
                origin == GIT_DIFF_LINE_DEL_EOFNL) {
              newsize += 1;
              continue;
            }

            // Old content: find lfs object size.
            if (patch.lineNumber(idx, lidx, git::Diff::OldFile) >= 0) {
              QByteArray line = patch.lineContent(idx, lidx);
              if (line.startsWith("size "))
                oldlfssize = line.remove(0, 5).toULong();
            }

            // New content: find lfs object size.
            if (patch.lineNumber(idx, lidx, git::Diff::NewFile) >= 0) {
              newsize += patch.lineContent(idx, lidx).size();
              QByteArray line = patch.lineContent(idx, lidx);
              if (line.startsWith("size "))
                newlfssize = line.remove(0, 5).toULong();
            }
          }
        }
      }
    }

    QList<Line> lines;
    QByteArray content;

    // Add old filesize.
    if (oldsize) {
      if (oldsize != newsize)
        lines << Line(GIT_DIFF_LINE_DELETION, 1, -1);
      else
        lines << Line(GIT_DIFF_LINE_CONTEXT, 1, -1);
      if (lfs)
        content += InfoWidget::tr("LFS pointer size:");
      else
        content += InfoWidget::tr("size:");
      content += " " + locale().formattedDataSize(oldsize) + "\n";
    }

    // Add old LFS object size.
    if (oldlfssize) {
      if (oldlfssize != newlfssize)
        lines << Line(GIT_DIFF_LINE_DELETION, 2, -1);
      else
        lines << Line(GIT_DIFF_LINE_CONTEXT, 2, -1);
      content += InfoWidget::tr("LFS object size:");
      content += " " + locale().formattedDataSize(oldlfssize) + "\n";
    }

    // Add old filemode.
    if ((oldmode != GIT_FILEMODE_UNREADABLE) && (oldmode != newmode)) {
      if (oldmode != newmode)
        lines << Line(GIT_DIFF_LINE_DELETION, 3, -1);
      else
        lines << Line(GIT_DIFF_LINE_CONTEXT, 3, -1);
      content += InfoWidget::tr("mode:");
      content += " " + QString::number(oldmode, 8);
      content += " (" + fromFilemode(oldmode) + ")\n";
    }

    // Add new filesize.
    if ((newsize) && (newsize != oldsize)) {
        lines << Line(GIT_DIFF_LINE_ADDITION, -1, 1);
      if (lfs)
        content += InfoWidget::tr("LFS pointer size:");
      else
        content += InfoWidget::tr("size:");
      content += " " + locale().formattedDataSize(newsize) + "\n";
    }

    // Add new LFS object size.
    if ((newlfssize) && (newlfssize != oldlfssize)) {
      lines << Line(GIT_DIFF_LINE_ADDITION, -1, 2);
      content += InfoWidget::tr("LFS object size:");
      content += " " + locale().formattedDataSize(newlfssize) + "\n";
    }

    // Add new filemode.
    if (newmode != GIT_FILEMODE_UNREADABLE) {
      if (newmode != oldmode)
        lines << Line(GIT_DIFF_LINE_ADDITION, -1, 3);
      else
        lines << Line(GIT_DIFF_LINE_CONTEXT, -1, 3);
      content += InfoWidget::tr("mode:");
      content += " " + QString::number(newmode, 8);
      content += " (" + fromFilemode(newmode) + ")\n";
    }

    // Find matching lines.
    int count = lines.size();
    for (int lidx = 0; lidx < count; ++lidx) {
      for (int ridx = lidx + 1; ridx < count; ++ridx) {
        QByteArray l = lines[lidx].oldLine();
        QByteArray r = lines[ridx].newLine();
        if ((l.size() > 0) && (r.size() > 0) && (l.toInt() == r.toInt())) {
          lines[lidx].setMatchingLine(ridx);
          lines[ridx].setMatchingLine(lidx);
        }
      }
    }

    // Trim final line end.
    content.chop(1);

    mEditor->setText(content);

    // Calculate margin width.
    int width = 0;
    int conflictWidth = 0;
    foreach (const Line &line, lines) {
      int oldWidth = line.oldLine().length();
      int newWidth = line.newLine().length();
      width = qMax(width, oldWidth + newWidth + 1);
      conflictWidth = qMax(conflictWidth, oldWidth);
    }

    // Add markers and line numbers.
    for (int lidx = 0; lidx < count; ++lidx) {
      const Line &line = lines.at(lidx);
      QByteArray oldLine = line.oldLine();
      QByteArray newLine = line.newLine();
      int spaces = width - (oldLine.length() + newLine.length());
      QByteArray text = oldLine + QByteArray(spaces, ' ') + newLine;
      mEditor->marginSetText(lidx, text);
      mEditor->marginSetStyle(lidx, STYLE_LINENUMBER);

      // Find marker.
      int marker = -1;
      switch (line.origin()) {
        case GIT_DIFF_LINE_CONTEXT:
          marker = TextEditor::Context;
          break;

        case GIT_DIFF_LINE_ADDITION:
          marker = TextEditor::Addition;
          break;

        case GIT_DIFF_LINE_DELETION:
          marker = TextEditor::Deletion;
          break;
      }

      // Add marker.
      if (marker >= 0)
        mEditor->markerAdd(lidx, marker);
    }

    // Diff matching lines.
    for (int lidx = 0; lidx < count; ++lidx) {
      const Line &line = lines.at(lidx);
      int matchingLine = line.matchingLine();
      if (line.origin() == GIT_DIFF_LINE_DELETION && matchingLine >= 0) {
        // Split lines into tokens and diff corresponding tokens.
        QList<Token> oldTokens = tokens(lidx);
        QList<Token> newTokens = tokens(matchingLine);
        QByteArray oldBuffer = tokenBuffer(oldTokens);
        QByteArray newBuffer = tokenBuffer(newTokens);

        git::Patch patch = git::Patch::fromBuffers(oldBuffer, newBuffer);
        for (int pidx = 0; pidx < patch.count(); ++pidx) {
          // Find the boundary between additions and deletions.
          int index;
          int count = patch.lineCount(pidx);
          for (index = 0; index < count; ++index) {
            if (patch.lineOrigin(pidx, index) == GIT_DIFF_LINE_ADDITION)
              break;
          }

          // Map differences onto the deletion line.
          if (index > 0) {
            int first = patch.lineNumber(pidx, 0, git::Diff::OldFile) - 1;
            int last = patch.lineNumber(pidx, index - 1, git::Diff::OldFile);

            int size = oldTokens.size();
            if (first >= 0 && first < size && last >= 0 && last < size) {
              int pos = oldTokens.at(first).pos;
              mEditor->setIndicatorCurrent(TextEditor::WordDeletion);
              mEditor->indicatorFillRange(pos, oldTokens.at(last).pos - pos);
            }
          }

          // Map differences onto the addition line.
          if (index < count) {
            int first = patch.lineNumber(pidx, index, git::Diff::NewFile) - 1;
            int last = patch.lineNumber(pidx, count - 1, git::Diff::NewFile);

            int size = newTokens.size();
            if (first >= 0 && first < size && last >= 0 && last < size) {
              int pos = newTokens.at(first).pos;
              mEditor->setIndicatorCurrent(TextEditor::WordAddition);
              mEditor->indicatorFillRange(pos, newTokens.at(last).pos - pos);
            }
          }
        }
      }
    }

    // Set margin width.
    QByteArray text(patch.isConflicted() ? conflictWidth : width, ' ');
    int margin = mEditor->textWidth(STYLE_DEFAULT, text);
    if (margin > mEditor->marginWidthN(TextEditor::LineNumbers))
      mEditor->setMarginWidthN(TextEditor::LineNumbers, margin);

    // Disallow editing.
    mEditor->setReadOnly(true);

    mEditor->updateGeometry();
  }

  DisclosureButton *button() const { return mHeader->button(); }

private:
  QString fromFilemode(const git_filemode_t mode)
  {
    switch (mode) {
      case GIT_FILEMODE_UNREADABLE:
        return QString(InfoWidget::tr("missing"));
      case GIT_FILEMODE_TREE:
        return QString(InfoWidget::tr("tree"));
      case GIT_FILEMODE_BLOB:
        return QString(InfoWidget::tr("blob"));
      case GIT_FILEMODE_BLOB_EXECUTABLE:
        return QString(InfoWidget::tr("executable"));
      case GIT_FILEMODE_LINK:
        return QString(InfoWidget::tr("link"));
      case GIT_FILEMODE_COMMIT:
        return QString(InfoWidget::tr("submodule"));
      default:
        // New (unknown) enum value added
        return QString(InfoWidget::tr("unknown"));
    }
  }

  struct Token
  {
    int pos;
    QByteArray text;
  };

  int tokenEndPosition(int pos) const
  {
    int length = mEditor->length();
    char ch = mEditor->charAt(pos);
    QByteArray wordChars = mEditor->wordChars();
    if (wordChars.contains(ch)) {
      do {
        ch = mEditor->charAt(++pos);
      } while (pos < length && wordChars.contains(ch));
      return pos;
    }

    QByteArray spaceChars = mEditor->whitespaceChars();
    if (spaceChars.contains(ch)) {
      do {
        ch = mEditor->charAt(++pos);
      } while (pos < length && spaceChars.contains(ch));
      return pos;
    }

    return pos + 1;
  }

  QList<Token> tokens(int line) const
  {
    QList<Token> tokens;

    int end = mEditor->lineEndPosition(line);
    int pos = mEditor->positionFromLine(line);
    while (pos < end) {
      int wordEnd = tokenEndPosition(pos);
      tokens.append({pos, mEditor->textRange(pos, wordEnd)});
      pos = wordEnd;
    }

    // Add sentinel.
    tokens.append({pos, ""});

    return tokens;
  }

  QByteArray tokenBuffer(const QList<Token> &tokens)
  {
    QByteArrayList list;
    foreach (const Token &token, tokens)
      list.append(token.text);
    return list.join('\n');
  }

  Header *mHeader;
  TextEditor *mEditor;
};

class HunkWidget : public QFrame
{
  Q_OBJECT

public:
  class Header : public QFrame
  {
  public:
    Header(
      const git::Diff &diff,
      const git::Patch &patch,
      int index,
      bool lfs,
      bool submodule,
      QWidget *parent = nullptr)
      : QFrame(parent)
    {
      setObjectName("HunkHeader");
      mCheck = new QCheckBox(this);

      QString header = (index >= 0) ? patch.header(index) : QString();
      QString escaped = header.trimmed().toHtmlEscaped();
      QLabel *label = new QLabel(kHunkFmt.arg(escaped), this);

      if (patch.isConflicted()) {
        mSave = new QToolButton(this);
        mSave->setObjectName("ConflictSave");
        mSave->setText(HunkWidget::tr("Save"));

        mUndo = new QToolButton(this);
        mUndo->setObjectName("ConflictUndo");
        mUndo->setText(HunkWidget::tr("Undo"));
        connect(mUndo, &QToolButton::clicked, [this] {
          mSave->setVisible(false);
          mUndo->setVisible(false);
          mOurs->setEnabled(true);
          mTheirs->setEnabled(true);
        });

        mOurs = new QToolButton(this);
        mOurs->setObjectName("ConflictOurs");
        mOurs->setStyleSheet(buttonStyle(Theme::Diff::Ours));
        mOurs->setText(HunkWidget::tr("Use Ours"));
        connect(mOurs, &QToolButton::clicked, [this] {
          mSave->setVisible(true);
          mUndo->setVisible(true);
          mOurs->setEnabled(false);
          mTheirs->setEnabled(false);
        });

        mTheirs = new QToolButton(this);
        mTheirs->setObjectName("ConflictTheirs");
        mTheirs->setStyleSheet(buttonStyle(Theme::Diff::Theirs));
        mTheirs->setText(HunkWidget::tr("Use Theirs"));
        connect(mTheirs, &QToolButton::clicked, [this] {
          mSave->setVisible(true);
          mUndo->setVisible(true);
          mOurs->setEnabled(false);
          mTheirs->setEnabled(false);
        });
      }

      EditButton *edit = new EditButton(patch, index, false, lfs, this);
      edit->setToolTip(HunkWidget::tr("Edit Hunk"));

      // Add discard button.
      DiscardButton *discard = nullptr;
      if (diff.isStatusDiff() && !submodule && !patch.isConflicted()) {
        discard = new DiscardButton(this);
        discard->setToolTip(HunkWidget::tr("Discard Hunk"));
        connect(discard, &DiscardButton::clicked, [this, patch, index] {
          QString name = patch.name();
          int line = patch.lineNumber(index, 0, git::Diff::NewFile);

          QString title = HunkWidget::tr("Discard Hunk?");
          QString text = patch.isUntracked() ?
            HunkWidget::tr("Are you sure you want to remove '%1'?").arg(name) :
            HunkWidget::tr("Are you sure you want to discard the "
               "hunk starting at line %1 in '%2'?").arg(line).arg(name);

          QMessageBox *dialog = new QMessageBox(
            QMessageBox::Warning, title, text, QMessageBox::Cancel, this);
          dialog->setAttribute(Qt::WA_DeleteOnClose);
          dialog->setInformativeText(HunkWidget::tr("This action cannot be undone."));

          QPushButton *discard =
            dialog->addButton(HunkWidget::tr("Discard Hunk"), QMessageBox::AcceptRole);
          connect(discard, &QPushButton::clicked, [this, patch, index] {
            git::Repository repo = patch.repo();
            if (patch.isUntracked()) {
              repo.workdir().remove(patch.name());
              return;
            }

            QString name = patch.name();
            QSaveFile file(repo.workdir().filePath(name));
            if (!file.open(QFile::WriteOnly))
              return;

            QBitArray hunks(patch.count(), true);
            hunks[index] = false;
            QByteArray buffer = patch.apply(hunks, repo.filters(name));
            if (buffer.isEmpty())
              return;

            file.write(buffer);
            if (!file.commit())
              return;

            // FIXME: Work dir changed?
            RepoView::parentView(this)->refresh();
          });

          dialog->open();
        });
      }

      mButton = new DisclosureButton(this);
      mButton->setToolTip(
        mButton->isChecked() ? HunkWidget::tr("Collapse Hunk") : HunkWidget::tr("Expand Hunk"));
      connect(mButton, &DisclosureButton::toggled, [this] {
        mButton->setToolTip(
          mButton->isChecked() ? HunkWidget::tr("Collapse Hunk") : HunkWidget::tr("Expand Hunk"));
      });

      QHBoxLayout *buttons = new QHBoxLayout;
      buttons->setContentsMargins(0,0,0,0);
      buttons->setSpacing(4);
      if (mSave && mUndo && mOurs && mTheirs) {
        mSave->setVisible(false);
        mUndo->setVisible(false);
        buttons->addWidget(mSave);
        buttons->addWidget(mUndo);
        buttons->addWidget(mOurs);
        buttons->addWidget(mTheirs);
        buttons->addSpacing(8);
      }

      buttons->addWidget(edit);
      if (discard)
        buttons->addWidget(discard);
      buttons->addWidget(mButton);

      QHBoxLayout *layout = new QHBoxLayout(this);
      layout->setContentsMargins(4,4,4,4);
      layout->addWidget(mCheck);
      layout->addWidget(label);
      layout->addStretch();
      layout->addLayout(buttons);

      // Collapse on check.
      connect(mCheck, &QCheckBox::clicked, [this](bool staged) {
        mButton->setChecked(!staged);
      });
    }

    QCheckBox *check() const { return mCheck; }

    DisclosureButton *button() const { return mButton; }

    QToolButton *saveButton() const { return mSave; }
    QToolButton *undoButton() const { return mUndo; }
    QToolButton *oursButton() const { return mOurs; }
    QToolButton *theirsButton() const { return mTheirs; }

  protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
      if (mButton->isEnabled())
        mButton->toggle();
    }

  private:
    QCheckBox *mCheck;
    DisclosureButton *mButton;
    QToolButton *mSave = nullptr;
    QToolButton *mUndo = nullptr;
    QToolButton *mOurs = nullptr;
    QToolButton *mTheirs = nullptr;
  };

  HunkWidget(
    DiffView *view,
    const git::Diff &diff,
    const git::Patch &patch,
    int index,
    bool lfs,
    bool submodule,
    QWidget *parent = nullptr)
    : QFrame(parent), mView(view), mPatch(patch), mIndex(index)
  {
    setObjectName("HunkWidget");
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(0);

    mHeader = new Header(diff, patch, index, lfs, submodule, this);
    layout->addWidget(mHeader);

    mEditor = new Editor(this);
    mEditor->setLexer(patch.name());
    mEditor->setCaretStyle(CARETSTYLE_INVISIBLE);
    mEditor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    if (index >= 0)
      mEditor->setLineCount(patch.lineCount(index));

    connect(mEditor, &TextEditor::updateUi,
            MenuBar::instance(this), &MenuBar::updateCutCopyPaste);

    // Ensure that text margin reacts to settings changes.
    connect(mEditor, &TextEditor::settingsChanged, [this] {
      int width = mEditor->textWidth(STYLE_LINENUMBER, mEditor->marginText(0));
      mEditor->setMarginWidthN(TextEditor::LineNumbers, width);
    });

    // Darken background when find highlight is active.
    connect(mEditor, &TextEditor::highlightActivated,
            this, &HunkWidget::setDisabled);

    // Disable vertical resize.
    mEditor->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    layout->addWidget(mEditor);
    connect(mHeader->button(), &DisclosureButton::toggled,
            mEditor, &TextEditor::setVisible);

    // Handle conflict resolution.
    if (QToolButton *save = mHeader->saveButton()) {
      connect(save, &QToolButton::clicked, [this] {
        TextEditor editor;
        git::Repository repo = mPatch.repo();
        QString path = repo.workdir().filePath(mPatch.name());

        {
          // Read file.
          QFile file(path);
          if (file.open(QFile::ReadOnly))
            editor.load(path, repo.decode(file.readAll()));
        }

        if (!editor.length())
          return;

        // Apply resolution.
        for (int i = mPatch.lineCount(mIndex); i >= 0; --i) {
          char origin = mPatch.lineOrigin(mIndex, i);
          auto resolution = mPatch.conflictResolution(mIndex);
          if (origin == GIT_DIFF_LINE_CONTEXT ||
              (origin == 'O' && resolution == git::Patch::Ours) ||
              (origin == 'T' && resolution == git::Patch::Theirs))
            continue;

          int line = mPatch.lineNumber(mIndex, i);
          int pos = editor.positionFromLine(line);
          int length = editor.lineLength(line);
          editor.deleteRange(pos, length);
        }

        // Write file to disk.
        QSaveFile file(path);
        if (!file.open(QFile::WriteOnly))
          return;

        QTextStream out(&file);
        out.setCodec(repo.codec());
        out << editor.text();
        file.commit();

        mPatch.setConflictResolution(mIndex, git::Patch::Unresolved);

        RepoView::parentView(this)->refresh();
      });
    }

    if (QToolButton *undo = mHeader->undoButton()) {
      connect(undo, &QToolButton::clicked, [this] {
        // Invalidate to trigger reload.
        invalidate();
        mPatch.setConflictResolution(mIndex, git::Patch::Unresolved);
      });
    }

    if (QToolButton *ours = mHeader->oursButton()) {
      connect(ours, &QToolButton::clicked, [this] {
        mEditor->markerDeleteAll(TextEditor::Theirs);
        chooseLines(TextEditor::Ours);
        mPatch.setConflictResolution(mIndex, git::Patch::Ours);
      });
    }

    if (QToolButton *theirs = mHeader->theirsButton()) {
      connect(theirs, &QToolButton::clicked, [this] {
        mEditor->markerDeleteAll(TextEditor::Ours);
        chooseLines(TextEditor::Theirs);
        mPatch.setConflictResolution(mIndex, git::Patch::Theirs);
      });
    }

    // Hook up error margin click.
    bool status = diff.isStatusDiff();
    connect(mEditor, &TextEditor::marginClicked, [this, status](int pos) {
      int line = mEditor->lineFromPosition(pos);
      QList<TextEditor::Diagnostic> diags = mEditor->diagnostics(line);
      if (diags.isEmpty())
        return;

      QTableWidget *table = new QTableWidget(diags.size(), 3);
      table->setWindowFlag(Qt::Popup);
      table->setAttribute(Qt::WA_DeleteOnClose);
      table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
      table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
      table->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);

      table->setShowGrid(false);
      table->setSelectionMode(QAbstractItemView::NoSelection);
      table->verticalHeader()->setVisible(false);
      table->horizontalHeader()->setVisible(false);

      QShortcut *esc = new QShortcut(tr("Esc"), table);
      connect(esc, &QShortcut::activated, table, &QTableWidget::close);

      for (int i = 0; i < diags.size(); ++i) {
        const TextEditor::Diagnostic &diag = diags.at(i);

        QStyle::StandardPixmap pixmap;
        switch (diag.kind) {
          case TextEditor::Note:
            pixmap = QStyle::SP_MessageBoxInformation;
            break;

          case TextEditor::Warning:
            pixmap = QStyle::SP_MessageBoxWarning;
            break;

          case TextEditor::Error:
            pixmap = QStyle::SP_MessageBoxCritical;
            break;
        }

        QIcon icon = style()->standardIcon(pixmap);
        QTableWidgetItem *item = new QTableWidgetItem(icon, diag.message);
        item->setToolTip(diag.description);
        table->setItem(i, 0, item);

        // Add fix button. Disable for deletion lines.
        QPushButton *fix = new QPushButton(tr("Fix"));
        bool deletion = (mEditor->markers(line) & (1 << TextEditor::Deletion));
        fix->setEnabled(status && !deletion && !diag.replacement.isNull());
        connect(fix, &QPushButton::clicked, [this, line, diag, table] {
          // Look up the actual line number from the margin.
          QRegularExpression re("\\s+");
          QStringList numbers = mEditor->marginText(line).split(re);
          if (numbers.size() != 2)
            return;

          int newLine = numbers.last().toInt() - 1;
          if (newLine < 0)
            return;

          // Load editor.
          TextEditor editor;
          git::Repository repo = mPatch.repo();
          QString path = repo.workdir().filePath(mPatch.name());

          {
            // Read file.
            QFile file(path);
            if (file.open(QFile::ReadOnly))
              editor.load(path, repo.decode(file.readAll()));
          }

          if (!editor.length())
            return;

          // Replace range.
          int pos = editor.positionFromLine(newLine) + diag.range.pos;
          editor.setSelection(pos + diag.range.len, pos);
          editor.replaceSelection(diag.replacement);

          // Write file to disk.
          QSaveFile file(path);
          if (!file.open(QFile::WriteOnly))
            return;

          QTextStream out(&file);
          out.setCodec(repo.codec());
          out << editor.text();
          file.commit();

          table->hide();
          RepoView::parentView(this)->refresh();
        });

        table->setCellWidget(i, 1, fix);

        // Add edit button.
        QPushButton *edit = new QPushButton(tr("Edit"));
        connect(edit, &QPushButton::clicked, [this, line, diag] {
          // Look up the actual line number from the margin.
          QRegularExpression re("\\s+");
          QStringList numbers = mEditor->marginText(line).split(re);
          if (numbers.size() != 2)
            return;

          int newLine = numbers.last().toInt() - 1;
          if (newLine < 0)
            return;

          // Edit the file and select the range.
          RepoView *view = RepoView::parentView(this);
          EditorWindow *window = view->openEditor(mPatch.name(), newLine);
          TextEditor *editor = window->widget()->editor();
          int pos = editor->positionFromLine(newLine) + diag.range.pos;
          editor->setSelection(pos + diag.range.len, pos);
        });

        table->setCellWidget(i, 2, edit);
      }

      table->resizeColumnsToContents();
      table->resize(table->sizeHint());

      QPoint point = mEditor->pointFromPosition(pos);
      point.setY(point.y() + mEditor->textHeight(line));
      table->move(mEditor->mapToGlobal(point));
      table->show();
    });
  }

  Header *header() const { return mHeader; }

  TextEditor *editor(bool ensureLoaded = true) {
    if (ensureLoaded)
      load();
    return mEditor;
  }

  void invalidate()
  {
    mEditor->setReadOnly(false);
    mEditor->clearAll();
    mLoaded = false;
    update();
  }

protected:
  void paintEvent(QPaintEvent *event) override
  {
    load();
    QFrame::paintEvent(event);
  }

private:
  struct Token
  {
    int pos;
    QByteArray text;
  };

  int tokenEndPosition(int pos) const
  {
    int length = mEditor->length();
    char ch = mEditor->charAt(pos);
    QByteArray wordChars = mEditor->wordChars();
    if (wordChars.contains(ch)) {
      do {
        ch = mEditor->charAt(++pos);
      } while (pos < length && wordChars.contains(ch));
      return pos;
    }

    QByteArray spaceChars = mEditor->whitespaceChars();
    if (spaceChars.contains(ch)) {
      do {
        ch = mEditor->charAt(++pos);
      } while (pos < length && spaceChars.contains(ch));
      return pos;
    }

    return pos + 1;
  }

  QList<Token> tokens(int line) const
  {
    QList<Token> tokens;

    int end = mEditor->lineEndPosition(line);
    int pos = mEditor->positionFromLine(line);
    while (pos < end) {
      int wordEnd = tokenEndPosition(pos);
      tokens.append({pos, mEditor->textRange(pos, wordEnd)});
      pos = wordEnd;
    }

    // Add sentinel.
    tokens.append({pos, ""});

    return tokens;
  }

  QByteArray tokenBuffer(const QList<Token> &tokens)
  {
    QByteArrayList list;
    foreach (const Token &token, tokens)
      list.append(token.text);
    return list.join('\n');
  }

  void load()
  {
    if (mLoaded)
      return;

    mLoaded = true;

    // Load entire file.
    git::Repository repo = mPatch.repo();
    if (mIndex < 0) {
      QString name = mPatch.name();
      QFile dev(repo.workdir().filePath(name));
      if (dev.open(QFile::ReadOnly)) {
        mEditor->load(name, repo.decode(dev.readAll()));

        int count = mEditor->lineCount();
        QByteArray lines = QByteArray::number(count);
        int width = mEditor->textWidth(STYLE_LINENUMBER, lines.constData());
        int marginWidth = (mEditor->length() > 0) ? width + 8 : 0;
        mEditor->setMarginWidthN(TextEditor::LineNumber, marginWidth);
      }

      // Disallow editing.
      mEditor->setReadOnly(true);

      return;
    }

    // Load hunk.
    QList<Line> lines;
    QByteArray content;
    int patchCount = mPatch.lineCount(mIndex);
    for (int lidx = 0; lidx < patchCount; ++lidx) {
      char origin = mPatch.lineOrigin(mIndex, lidx);
      if (origin == GIT_DIFF_LINE_CONTEXT_EOFNL ||
          origin == GIT_DIFF_LINE_ADD_EOFNL ||
          origin == GIT_DIFF_LINE_DEL_EOFNL) {
        Q_ASSERT(!lines.isEmpty());
        lines.last().setNewline(false);
        content += '\n';
        continue;
      }

      int oldLine = mPatch.lineNumber(mIndex, lidx, git::Diff::OldFile);
      int newLine = mPatch.lineNumber(mIndex, lidx, git::Diff::NewFile);
      lines << Line(origin, oldLine, newLine);
      content += mPatch.lineContent(mIndex, lidx);
    }

    // Trim final line end.
    if (content.endsWith('\n'))
      content.chop(1);
    if (content.endsWith('\r'))
      content.chop(1);

    // Add text.
    mEditor->setText(repo.decode(content));

    // Calculate margin width.
    int width = 0;
    int conflictWidth = 0;
    foreach (const Line &line, lines) {
      int oldWidth = line.oldLine().length();
      int newWidth = line.newLine().length();
      width = qMax(width, oldWidth + newWidth + 1);
      conflictWidth = qMax(conflictWidth, oldWidth);
    }

    // Get comments for this file.
    Account::FileComments comments = mView->comments().files.value(mPatch.name());

    // Add markers and line numbers.
    int additions = 0;
    int deletions = 0;
    int count = lines.size();
    for (int lidx = 0; lidx < count; ++lidx) {
      const Line &line = lines.at(lidx);
      QByteArray oldLine = line.oldLine();
      QByteArray newLine = line.newLine();
      int spaces = width - (oldLine.length() + newLine.length());
      QByteArray text = oldLine + QByteArray(spaces, ' ') + newLine;
      mEditor->marginSetText(lidx, text);
      mEditor->marginSetStyle(lidx, STYLE_LINENUMBER);

      // Build annotations.
      QList<Annotation> annotations;
      if (!line.newline()) {
        QString text = tr("No newline at end of file");
        QByteArray styles =
          QByteArray(text.toUtf8().size(), TextEditor::EofNewline);
        annotations.append({text, styles});
      }

      auto it = comments.constFind(lidx);
      if (it != comments.constEnd()) {
        QString whitespace(kIndent, ' ');
        QFont font = mEditor->styleFont(TextEditor::CommentBody);
        int margin = QFontMetrics(font).horizontalAdvance(' ') * kIndent * 2;
        int width = mEditor->textRectangle().width() - margin - 50;

        foreach (const QDateTime &key, it->keys()) {
          QStringList paragraphs;
          Account::Comment comment = it->value(key);
          foreach (const QString &paragraph, comment.body.split('\n')) {
            if (paragraph.isEmpty()) {
              paragraphs.append(QString());
              continue;
            }

            QStringList lines;
            QTextLayout layout(paragraph, font);
            layout.beginLayout();

            forever {
              QTextLine line = layout.createLine();
              if (!line.isValid())
                break;

              line.setLineWidth(width);
              QString text = paragraph.mid(line.textStart(), line.textLength());
              lines.append(whitespace + text.trimmed() + whitespace);
            }

            layout.endLayout();
            paragraphs.append(lines.join('\n'));
          }

          QString author = comment.author;
          QString time = key.toString(Qt::DefaultLocaleLongDate);
          QString body = paragraphs.join('\n');
          QString text = author + ' ' + time + '\n' + body;
          QByteArray styles =
            QByteArray(author.toUtf8().size() + 1, TextEditor::CommentAuthor) +
            QByteArray(time.toUtf8().size() + 1, TextEditor::CommentTimestamp) +
            QByteArray(body.toUtf8().size(), TextEditor::CommentBody);
          annotations.append({text, styles});
        }
      }

      QString atnText;
      QByteArray atnStyles;
      foreach (const Annotation &annotation, annotations) {
        if (!atnText.isEmpty()) {
          atnText.append("\n\n");
          atnStyles.append(QByteArray(2, TextEditor::CommentBody));
        }

        atnText.append(annotation.text);
        atnStyles.append(annotation.styles);
      }

      // Set annotations.
      if (!atnText.isEmpty()) {
        mEditor->annotationSetText(lidx, atnText);
        mEditor->annotationSetStyles(lidx, atnStyles);
        mEditor->annotationSetVisible(ANNOTATION_STANDARD);
      }

      // Find matching lines.
      int marker = -1;
      switch (line.origin()) {
        case GIT_DIFF_LINE_CONTEXT:
          marker = TextEditor::Context;
          additions = 0;
          deletions = 0;
          break;

        case GIT_DIFF_LINE_ADDITION:
          marker = TextEditor::Addition;
          ++additions;
          if (lidx + 1 >= count ||
              mPatch.lineOrigin(mIndex, lidx + 1) != GIT_DIFF_LINE_ADDITION) {
            // The heuristic is that matching blocks have
            // the same number of additions as deletions.
            if (additions == deletions) {
              for (int i = 0; i < additions; ++i) {
                int current = lidx - i;
                int match = current - additions;
                lines[current].setMatchingLine(match);
                lines[match].setMatchingLine(current);
              }
            }

            additions = 0;
            deletions = 0;
          }
          break;

        case GIT_DIFF_LINE_DELETION:
          marker = TextEditor::Deletion;
          ++deletions;
          break;

        case 'O':
          marker = TextEditor::Ours;
          break;

        case 'T':
          marker = TextEditor::Theirs;
          break;
      }

      // Add marker.
      if (marker >= 0)
        mEditor->markerAdd(lidx, marker);
    }

    // Diff matching lines.
    for (int lidx = 0; lidx < count; ++lidx) {
      const Line &line = lines.at(lidx);
      int matchingLine = line.matchingLine();
      if (line.origin() == GIT_DIFF_LINE_DELETION && matchingLine >= 0) {
        // Split lines into tokens and diff corresponding tokens.
        QList<Token> oldTokens = tokens(lidx);
        QList<Token> newTokens = tokens(matchingLine);
        QByteArray oldBuffer = tokenBuffer(oldTokens);
        QByteArray newBuffer = tokenBuffer(newTokens);
        git::Patch patch = git::Patch::fromBuffers(oldBuffer, newBuffer);
        for (int pidx = 0; pidx < patch.count(); ++pidx) {
          // Find the boundary between additions and deletions.
          int index;
          int count = patch.lineCount(pidx);
          for (index = 0; index < count; ++index) {
            if (patch.lineOrigin(pidx, index) == GIT_DIFF_LINE_ADDITION)
              break;
          }

          // Map differences onto the deletion line.
          if (index > 0) {
            int first = patch.lineNumber(pidx, 0, git::Diff::OldFile) - 1;
            int last = patch.lineNumber(pidx, index - 1, git::Diff::OldFile);

            int size = oldTokens.size();
            if (first >= 0 && first < size && last >= 0 && last < size) {
              int pos = oldTokens.at(first).pos;
              mEditor->setIndicatorCurrent(TextEditor::WordDeletion);
              mEditor->indicatorFillRange(pos, oldTokens.at(last).pos - pos);
            }
          }

          // Map differences onto the addition line.
          if (index < count) {
            int first = patch.lineNumber(pidx, index, git::Diff::NewFile) - 1;
            int last = patch.lineNumber(pidx, count - 1, git::Diff::NewFile);

            int size = newTokens.size();
            if (first >= 0 && first < size && last >= 0 && last < size) {
              int pos = newTokens.at(first).pos;
              mEditor->setIndicatorCurrent(TextEditor::WordAddition);
              mEditor->indicatorFillRange(pos, newTokens.at(last).pos - pos);
            }
          }
        }
      }
    }

    // Set margin width.
    QByteArray text(mPatch.isConflicted() ? conflictWidth : width, ' ');
    int margin = mEditor->textWidth(STYLE_DEFAULT, text);
    if (margin > mEditor->marginWidthN(TextEditor::LineNumbers))
      mEditor->setMarginWidthN(TextEditor::LineNumbers, margin);

    // Disallow editing.
    mEditor->setReadOnly(true);

    // Restore resolved conflicts.
    if (mPatch.isConflicted()) {
      switch (mPatch.conflictResolution(mIndex)) {
        case git::Patch::Ours:
          mHeader->oursButton()->click();
          break;

        case git::Patch::Theirs:
          mHeader->theirsButton()->click();
          break;

        default:
          break;
      }
    }

    // Execute hunk plugins.
    foreach (PluginRef plugin, mView->plugins()) {
      if (plugin->isValid() && plugin->isEnabled())
        plugin->hunk(mEditor);
    }

    mEditor->updateGeometry();
  }

  void chooseLines(TextEditor::Marker kind)
  {
    // Edit hunk.
    mEditor->setReadOnly(false);
    int mask = ((1 << TextEditor::Context) | (1 << kind));
    for (int i = mEditor->lineCount() - 1; i >= 0; --i) {
      if (!(mask & mEditor->markers(i))) {
        int pos = mEditor->positionFromLine(i);
        int length = mEditor->lineLength(i);
        mEditor->deleteRange(pos, length);
      }
    }

    mEditor->setReadOnly(true);
  }

  DiffView *mView;
  git::Patch mPatch;
  int mIndex;

  Header *mHeader;
  TextEditor *mEditor;
  bool mLoaded = false;
};

class LineStats : public QWidget
{
public:
  LineStats(const git::Patch::LineStats &stats, QWidget *parent = nullptr)
    : QWidget(parent)
  {
    Q_ASSERT(stats.additions > 0 || stats.deletions > 0);

    qreal x = static_cast<qreal>(stats.additions + stats.deletions) / 5;
    bool scaled = (stats.additions + stats.deletions > 5);
    mPluses = scaled ? stats.additions / x : stats.additions;
    mMinuses = scaled ? stats.deletions / x : stats.deletions;
  }

  QSize minimumSizeHint() const override
  {
    return QSize(10 * (mPluses + mMinuses), 12);
  }

  void paintEvent(QPaintEvent *event) override
  {
    // Drawing the outline of the shapes with a narrow pen approximates
    // a slight drop shadow. Minus has to be drawn slightly smaller than
    // plus or it creates an optical illusion that makes it look to big.

    QPainter painter(this);
    qreal w = width() / (mPluses + mMinuses);
    qreal h = height();
    qreal x = w / 2;
    qreal y = h / 2;

    painter.setPen(QPen(Qt::black, 0.1));
    painter.setBrush(Application::theme()->diff(Theme::Diff::Plus));
    for (int i = 0; i < mPluses; ++i) {
      QPainterPath path;
      path.moveTo(x - 4, y - 1);
      path.lineTo(x - 1, y - 1);
      path.lineTo(x - 1, y - 4);
      path.lineTo(x + 1, y - 4);
      path.lineTo(x + 1, y - 1);
      path.lineTo(x + 4, y - 1);
      path.lineTo(x + 4, y + 1);
      path.lineTo(x + 1, y + 1);
      path.lineTo(x + 1, y + 4);
      path.lineTo(x - 1, y + 4);
      path.lineTo(x - 1, y + 1);
      path.lineTo(x - 4, y + 1);
      path.closeSubpath();
      painter.drawPath(path);
      x += w;
    }

    painter.setBrush(Application::theme()->diff(Theme::Diff::Minus));
    for (int i = 0; i < mMinuses; ++i) {
      QPainterPath path;
      path.moveTo(x - 3, y - 1);
      path.lineTo(x + 4, y - 1);
      path.lineTo(x + 4, y + 1);
      path.lineTo(x - 3, y + 1);
      path.closeSubpath();
      painter.drawPath(path);
      x += w;
    }
  }

private:
  int mPluses = 0;
  int mMinuses = 0;
};

class ElidedLabel : public QLabel
{
public:
  ElidedLabel(const QString &text,
              const QString &shorttext = QString(),
              const Qt::TextElideMode &elidemode = Qt::ElideMiddle,
              QWidget *parent = nullptr)
    : QLabel(parent), mText(text), mShortText(shorttext), mElideMode(elidemode)
  {
    QLabel(text, parent);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  }

  ElidedLabel(const QString &text,
              const Qt::TextElideMode &elidemode = Qt::ElideMiddle,
              QWidget *parent = nullptr)
    : QLabel(parent), mText(text), mElideMode(elidemode)
  {
    QLabel(text, parent);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  }

  void setText(const QString &text,
               const QString &shorttext = QString())
  {
    mText = text;
    mShortText = shorttext;

    QFontMetrics fm = getFontMetric();
    QString plain = QTextDocumentFragment::fromHtml(text).toPlainText();
    mWidth = fm.boundingRect(plain).width() + fm.averageCharWidth();

    QLabel::setText(text);
  }

  void setElideMode(const Qt::TextElideMode &elidemode)
  {
    mElideMode = elidemode;
    repaint();
  }

  QSize sizeHint() const override
  {
    if (mElided)
      return QSize(mWidth, QLabel::sizeHint().height());

    return QLabel::sizeHint();
  }

  QSize minimumSizeHint() const override
  {
    if (mElided && !mShortText.isEmpty())
      return QLabel::minimumSizeHint();

    QFontMetrics fm = getFontMetric();

    return QSize(fm.boundingRect("...").width() + fm.averageCharWidth(), QLabel::minimumSizeHint().height());
  }

protected:
  void paintEvent(QPaintEvent *event) override
  {
    QFontMetrics fm = getFontMetric();
    QString plain = QTextDocumentFragment::fromHtml(mText).toPlainText();
    QString elide = fm.elidedText(plain, mElideMode, width());
    if (plain.length() > elide.length()) {
      // Use short text or elide text.
      if (!mShortText.isEmpty())
        QLabel::setText(mShortText);
      else {
        QString text = mText;
        QLabel::setText(text.replace(plain, elide));
      }
      mElided = true;
    } else {
      // Use text.
      QLabel::setText(mText);
      mElided = false;
      mWidth = QLabel::width();
    }

    QLabel::paintEvent(event);
  }

private:
  QFontMetrics getFontMetric(void) const
  {
    QFont font = QLabel::font();

    // Detect common HTML tags.
    font.setBold(font.bold() || mText.contains("<b>"));
    font.setItalic(font.italic() || mText.contains("<i>"));

    QFontMetrics fm(font);

    return fm;
  }

  QString mText;
  QString mShortText;

  Qt::TextElideMode mElideMode = Qt::ElideMiddle;

  bool mElided = false;
  int mWidth = 10000;
};

class FileLabel : public QWidget
{
  Q_OBJECT

public:
  FileLabel(const QString &oldname,
            const QString &newname,
            bool submodule, QWidget *parent = nullptr)
    : QWidget(parent)
  {
    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(2,2,2,2);

    QFont font = this->font();
    font.setBold(true);
    font.setPointSize(font.pointSize() + 3);

    // Old name of renamed file.
    if (!oldname.isEmpty()) {
      ElidedLabel *oldlabel = new ElidedLabel(oldname, Qt::ElideLeft, this);
      oldlabel->setFont(font);
      layout->addWidget(oldlabel, 1);
      layout->addWidget(new Arrow(this));
    }

    ElidedLabel *newlabel = new ElidedLabel(newname, Qt::ElideRight, this);
    newlabel->setFont(font);

    // New name, hyperlink if this is a submodule.
    if (submodule) {
      mUrlName = newname;
      newlabel->setText("<a href=\"submodule\">" + newname + "</a>", QString());
      newlabel->setToolTip(FileLabel::tr("Open Submodule"));
      connect(newlabel, &QLabel::linkActivated, [this] {
        QUrl url;
        url.setScheme("submodule");
        url.setPath(mUrlName);
        RepoView::parentView(this)->visitLink(url.toString());
      });
    }
    layout->addWidget(newlabel);
    layout->addStretch();
  }

private:
  QString mUrlName;
};

class FileWidget : public QFrame
{
  Q_OBJECT

public:
  class Header : public QFrame
  {
  public:
    Header(
      const git::Diff &diff,
      const git::Patch &patch,
      bool binary,
      bool lfs,
      bool submodule,
      QWidget *parent = nullptr)
      : QFrame(parent), mDiff(diff), mPatch(patch)
    {
      setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

      QString name = patch.name();
      mCheck = new QCheckBox(this);
      mCheck->setVisible(diff.isStatusDiff());

      char status = git::Diff::statusChar(patch.status());
      Badge *badge = new Badge({Badge::Label(QChar(status))}, this);

      LineStats *stats = nullptr;
      git::Patch::LineStats lineStats = patch.lineStats();
      if (lineStats.additions > 0 || lineStats.deletions > 0)
        stats = new LineStats(lineStats, this);

      FileLabel *label;
      if (patch.status() == GIT_DELTA_RENAMED)
        label = new FileLabel(patch.name(git::Diff::OldFile), name, submodule, this);
      else
        label = new FileLabel(QString(), name, submodule, this);

      QHBoxLayout *buttons = new QHBoxLayout;
      buttons->setContentsMargins(0,0,0,0);
      buttons->setSpacing(4);

      QHBoxLayout *layout = new QHBoxLayout(this);
      layout->setContentsMargins(4,4,4,4);
      layout->addWidget(mCheck);
      layout->addSpacing(4);
      layout->addWidget(badge);
      if (stats)
        layout->addWidget(stats);
      layout->addWidget(label, 1);
      layout->addStretch();
      layout->addLayout(buttons);

      if (binary) {

        // Add binary button.
        mToolButton = new QToolButton(this);
        mToolButton->setText(FileWidget::tr("Show Picture/Icon"));
        mToolButton->setCheckable(true);
      } else if (lfs) {

        // Add LFS buttons.
        Badge *lfsBadge = new Badge({Badge::Label(FileWidget::tr("LFS"), Theme::BadgeState::Head)}, this);
        buttons->addWidget(lfsBadge);

        QToolButton *lfsLockButton = new QToolButton(this);
        bool locked = patch.repo().lfsIsLocked(patch.name());
        lfsLockButton->setText(locked ? FileWidget::tr("Unlock") : FileWidget::tr("Lock"));
        buttons->addWidget(lfsLockButton);

        connect(lfsLockButton, &QToolButton::clicked, [this, patch] {
          bool locked = patch.repo().lfsIsLocked(patch.name());
          RepoView::parentView(this)->lfsSetLocked({patch.name()}, !locked);
        });

        git::RepositoryNotifier *notifier = patch.repo().notifier();
        connect(notifier, &git::RepositoryNotifier::lfsLocksChanged, this,
        [patch, lfsLockButton] {
          bool locked = patch.repo().lfsIsLocked(patch.name());
          lfsLockButton->setText(locked ? FileWidget::tr("Unlock") : FileWidget::tr("Lock"));
        });

        mToolButton = new QToolButton(this);
        mToolButton->setText(FileWidget::tr("Show Object"));
        mToolButton->setCheckable(true);
      } else {

        // Add info button.
        mToolButton = new QToolButton(this);
        mToolButton->setText(FileWidget::tr("Show Info"));
        mToolButton->setCheckable(true);
      }
      buttons->addWidget(mToolButton);
      buttons->addSpacing(8);

      // Add edit button.
      mEdit = new EditButton(patch, -1, binary, lfs, this);
      mEdit->setToolTip(FileWidget::tr("Edit File"));
      buttons->addWidget(mEdit);

      // Add discard button.
      if (diff.isStatusDiff() && !submodule && !patch.isConflicted()) {
        DiscardButton *discard = new DiscardButton(this);
        discard->setToolTip(FileWidget::tr("Discard File"));
        buttons->addWidget(discard);

        connect(discard, &QToolButton::clicked, [this] {
          RepoView *view = RepoView::parentView(this);
          QString name = mPatch.name();

          if (mPatch.isUntracked())
            view->clean({name});
          else
            view->promptToDiscard(view->repo().head().target(), {name});
        });
      }

      mDisclosureButton = new DisclosureButton(this);
      mDisclosureButton->setToolTip(
        mDisclosureButton->isChecked() ? FileWidget::tr("Collapse File") : FileWidget::tr("Expand File"));
      connect(mDisclosureButton, &DisclosureButton::toggled, [this] {
        mDisclosureButton->setToolTip(
          mDisclosureButton->isChecked() ? FileWidget::tr("Collapse File") : FileWidget::tr("Expand File"));
      });
      buttons->addWidget(mDisclosureButton);

      if (!diff.isStatusDiff())
        return;

      // Respond to check changes.
      connect(mCheck, &QCheckBox::clicked, [this](bool staged) {
        mCheck->setChecked(!staged); // Allow index to decide.
        mDiff.index().setStaged({mPatch.name()}, staged);
      });

      // Respond to index changes.
      git::Repository repo = RepoView::parentView(this)->repo();
      connect(repo.notifier(), &git::RepositoryNotifier::indexChanged, this,
      [this](const QStringList &paths) {
        if (paths.contains(mPatch.name()))
          updateCheckState();
      });

      // Set initial check state.
      updateCheckState();
    }

    QCheckBox *check() const { return mCheck; }

    DisclosureButton *disclosureButton() const { return mDisclosureButton; }

    QToolButton *toolButton() const { return mToolButton; }

  protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
      if (mDisclosureButton->isEnabled())
        mDisclosureButton->toggle();
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
      RepoView *view = RepoView::parentView(this);
      FileContextMenu menu(view, {mPatch.name()}, mDiff.index());
      menu.exec(event->globalPos());
    }

  private:
    void updateCheckState()
    {
      bool disabled = false;
      Qt::CheckState state = Qt::Unchecked;
      switch (mDiff.index().isStaged(mPatch.name())) {
        case git::Index::Disabled:
          disabled = true;
          break;

        case git::Index::Unstaged:
          break;

        case git::Index::PartiallyStaged:
          state = Qt::PartiallyChecked;
          break;

        case git::Index::Staged:
          state = Qt::Checked;
          break;

        case git::Index::Conflicted:
          disabled = (mPatch.count() > 0);
          break;
      }

      mCheck->setCheckState(state);
      mCheck->setEnabled(!disabled);
    }

    git::Diff mDiff;
    git::Patch mPatch;

    QCheckBox *mCheck;
    QToolButton *mToolButton;
    EditButton *mEdit;
    DisclosureButton *mDisclosureButton;
  };

  FileWidget(
    DiffView *view,
    const git::Diff &diff,
    const git::Patch &patch,
    const git::Patch &staged,
    QWidget *parent = nullptr)
    : QFrame(parent), mView(view), mDiff(diff), mPatch(patch)
  {
    setObjectName("FileWidget");
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(0);

    git::Repository repo = RepoView::parentView(this)->repo();

    QString name = patch.name();
    QString path = repo.workdir().filePath(name);
    bool submodule = repo.lookupSubmodule(name).isValid();

    bool binary = patch.isBinary();
    if (patch.isUntracked()) {
      QFile dev(path);
      if (dev.open(QFile::ReadOnly)) {
        QByteArray content;
        do {
          content = dev.read(1024 * 1024);
          git::Buffer buffer(content.constData(), content.length());
          binary = buffer.isBinary();
        } while (!binary && (content.length() == (1024 * 1024)));
        dev.close();
      }
    }

    bool lfs = patch.isLfsPointer();

    mHeader = new Header(diff, patch, binary, lfs, submodule, parent);
    layout->addWidget(mHeader);

    // File collapse.
    DisclosureButton *disclosureButton = mHeader->disclosureButton();
    connect(disclosureButton, &DisclosureButton::toggled, [this, lfs, binary](bool visible) {
      // LFS file expand/collapse.
      if (lfs && !visible) {
        if (!mHunks.isEmpty())
          mHunks.first()->setVisible(false);
        if (!mImages.isEmpty())
          mImages.first()->setVisible(false);
        if (!mInfos.isEmpty())
          mInfos.first()->setVisible(false);
        return;
      }

      if (lfs && visible) {
        bool checked = mHeader->toolButton()->isChecked();
        if (!mHunks.isEmpty())
          mHunks.first()->setVisible(!checked);
        if (!mImages.isEmpty())
          mImages.first()->setVisible(checked);
        if (!mInfos.isEmpty())
          mInfos.first()->setVisible(true);
        return;
      }

      // Binary file expand/collapse.
      if (binary && !visible) {
        if (!mImages.isEmpty())
          mImages.first()->setVisible(false);
        if (!mInfos.isEmpty())
          mInfos.first()->setVisible(false);
        return;
      }

      if (binary && visible) {
        bool checked = mHeader->toolButton()->isChecked();
        if (mHeader->toolButton()->isEnabled()) {

          // Toggle picture visibility.
          if (!mImages.isEmpty())
            mImages.first()->setVisible(checked);
        } else {

          // Button is disabled: expand/collapse picture.
          if (!mImages.isEmpty())
            mImages.first()->setVisible(true);
        }
        if (!mInfos.isEmpty())
          mInfos.first()->setVisible(true);
        return;
      }

      // Text file expand/collapse.
      if (mHeader->toolButton() && !visible) {
        if (!mInfos.isEmpty())
          mInfos.first()->setVisible(false);
      }

      if (mHeader->toolButton() && visible) {
        bool checked = mHeader->toolButton()->isChecked();
        if (!mInfos.isEmpty())
          mInfos.first()->setVisible(checked);
      }

      foreach (HunkWidget *hunk, mHunks)
        hunk->setVisible(visible);
    });

    if (diff.isStatusDiff()) {
      // Collapse on check.
      connect(mHeader->check(), &QCheckBox::stateChanged, [this](int state) {
        mHeader->disclosureButton()->setChecked(state != Qt::Checked);
        if (state != Qt::PartiallyChecked) {
          foreach (HunkWidget *hunk, mHunks)
            hunk->header()->check()->setChecked(state == Qt::Checked);
        }
      });
    }

    // Read default binary view configuration.
    git::Config config = repo.appConfig();
    bool loadbinary = config.value<bool>("diffview.loadbinary", true);
    bool scalebinary = config.value<bool>("diffview.scalebinary", false);

    QToolButton *toolButton = mHeader->toolButton();

    if (binary) {
      if ((patch.status() != GIT_DELTA_RENAMED) && loadbinary) {

        // Add file picture/icon.
        layout->addWidget(addImage(patch, lfs, scalebinary));
        if (toolButton)
          toolButton->setVisible(false);
      }

      // Add file info.
      layout->addWidget(addInfo(mDiff, mPatch, true, false));

      // Binary button.
      if (toolButton) {
        connect(toolButton, &QToolButton::clicked,
        [this, layout, disclosureButton, toolButton, scalebinary](bool checked) {
          toolButton->setText(checked ? tr("Hide Picture/Icon") : tr("Show Picture/Icon"));

          if (!mImages.isEmpty()) {

            // Picture/icon already loaded.
            mImages.first()->setVisible(checked);
            mImages.first()->button()->setChecked(checked);
          } else if (checked) {

            // Load picture/icon.
            layout->insertWidget(1, addImage(mPatch, false, scalebinary));
          }

          // Load file info.
          if (mInfos.isEmpty())
            layout->addWidget(addInfo(mDiff, mPatch, true, false));

          // File picture/icon or info loaded.
          if (!mImages.isEmpty() || !mInfos.isEmpty()) {
            disclosureButton->setEnabled(true);
          }
          else {
            disclosureButton->setChecked(false);
            disclosureButton->setEnabled(false);
          }
        });
      }

    } else if (patch.isUntracked()) {

      // Add untracked file content.
      if (!QFileInfo(path).isDir())
        layout->addWidget(addHunk(diff, patch, -1, lfs, submodule));

      // File info button.
      if (toolButton) {
        connect(toolButton, &QToolButton::clicked,
        [this, layout, disclosureButton, toolButton, lfs](bool checked) {
          toolButton->setText(checked ? tr("Hide Info") : tr("Show Info"));

          if (!mInfos.isEmpty()) {
            // Info already loaded.
            mInfos.first()->setVisible(checked);
            mInfos.first()->button()->setChecked(checked);
          } else if (checked) {
            // Load file info.
            layout->addWidget(addInfo(mDiff, mPatch, false, lfs));
          }

          // Hunk or info loaded.
          if (!mHunks.isEmpty() || (!mInfos.isEmpty() && checked))
            disclosureButton->setEnabled(true);
          else {
            disclosureButton->setChecked(false);
            disclosureButton->setEnabled(false);
          }
        });
      }

    } else {

      // Generate a diff between the head tree and index.
      QSet<int> stagedHunks;
      if (staged.isValid()) {
        for (int i = 0; i < staged.count(); ++i)
          stagedHunks.insert(staged.lineNumber(i, 0, git::Diff::OldFile));
      }

      // Add diff hunks.
      int hunkCount = patch.count();
      for (int hidx = 0; hidx < hunkCount; ++hidx) {
        HunkWidget *hunk = addHunk(diff, patch, hidx, lfs, submodule);
        int startLine = patch.lineNumber(hidx, 0, git::Diff::OldFile);
        hunk->header()->check()->setChecked(stagedHunks.contains(startLine));
        layout->addWidget(hunk);
      }

      // LFS view
      if (lfs) {

        // Add file info.
        layout->addWidget(addInfo(mDiff, mPatch, false, true));

        // LFS button.
        if (toolButton) {
          connect(toolButton, &QToolButton::clicked,
          [this, layout, disclosureButton, toolButton, scalebinary](bool checked) {
            toolButton->setText(checked ? tr("Show Pointer") : tr("Show Object"));
            mHunks.first()->setVisible(!checked);
            mHunks.first()->header()->button()->setChecked(!checked);

            if (!mImages.isEmpty()) {

              // Picture/icon already loaded.
              mImages.first()->setVisible(checked);
              mImages.first()->button()->setChecked(checked);
            } else if (checked) {

              // Load picture/icon.
              layout->insertWidget(1, addImage(mPatch, true, scalebinary));
            }

            // Pointer, object or info loaded.
            disclosureButton->setEnabled(true);
          });
        }
      } else {

        // Add file info.
        if (mHunks.isEmpty()) {
          layout->addWidget(addInfo(mDiff, mPatch, false, false));
          if (toolButton)
            toolButton->setChecked(true);
        }

        // Submodule and empty hunk: hide file info button.
        if (toolButton && (submodule || mHunks.isEmpty()))
          toolButton->setVisible(false);

        // File info button.
        if (toolButton) {
          connect(toolButton, &QToolButton::clicked,
          [this, layout, disclosureButton, toolButton](bool checked) {
            toolButton->setText(checked ? tr("Hide Info") : tr("Show Info"));

            if (!mInfos.isEmpty()) {

              // Info already loaded.
              mInfos.first()->setVisible(checked);
              mInfos.first()->button()->setChecked(checked);
            } else if (checked) {

              // Load file info.
              layout->addWidget(addInfo(mDiff, mPatch, false, false));
            }

            // Hunk, picture/info or info loaded.
            if (!mHunks.isEmpty() || !mImages.isEmpty() || (!mInfos.isEmpty() && checked))
              disclosureButton->setEnabled(true);
            else {
              disclosureButton->setChecked(false);
              disclosureButton->setEnabled(false);
            }
          });
        }
      }
    }

    // Start hidden when the file is checked.
    bool expand = (mHeader->check()->checkState() == Qt::Unchecked);

    // Auto collapse settings
    if (Settings::instance()->value("collapse/untracked").toBool() == true &&
        patch.isUntracked())
      expand = false;

    if (Settings::instance()->value("collapse/added").toBool() == true &&
        patch.status() == GIT_DELTA_ADDED)
      expand = false;

    if (Settings::instance()->value("collapse/modified").toBool() == true &&
        patch.status() == GIT_DELTA_MODIFIED)
      expand = false;

    if (patch.status() == GIT_DELTA_RENAMED)
      expand = false;

    if (Settings::instance()->value("collapse/deleted").toBool() == true &&
        patch.status() == GIT_DELTA_DELETED)
      expand = false;

    disclosureButton->setChecked(expand);
  }

  bool isEmpty()
  {
    return (mHunks.isEmpty() && mImages.isEmpty() && mInfos.isEmpty());
  }

  Header *header() const { return mHeader; }

  QString name() const { return mPatch.name(); }

  QList<HunkWidget *> hunks() const { return mHunks; }

  QWidget *addInfo(
    const git::Diff diff,
    const git::Patch patch,
    bool binary,
    bool lfs)
  {
    InfoWidget *info = new InfoWidget(diff, patch, binary, lfs, this);

    // Remember info.
    mInfos.append(info);

    return info;
  }

  QWidget *addImage(
    const git::Patch patch,
    bool lfs,
    bool scaled)
  {
    ImageWidget *image = new ImageWidget(patch, lfs, scaled, this);

    // Remember picture/icon.
    mImages.append(image);

    return image;
  }

  HunkWidget *addHunk(
    const git::Diff &diff,
    const git::Patch &patch,
    int index,
    bool lfs,
    bool submodule)
  {
    HunkWidget *hunk =
      new HunkWidget(mView, diff, patch, index, lfs, submodule, this);

    // Respond to check box click.
    QCheckBox *check = hunk->header()->check();
    check->setVisible(diff.isStatusDiff() && !submodule && !patch.isConflicted());
    connect(check, &QCheckBox::clicked, this, &FileWidget::stageHunks);

    // Respond to editor diagnostic signal.
    connect(hunk->editor(false), &TextEditor::diagnosticAdded,
    [this](int line, const TextEditor::Diagnostic &diag) {
      emit diagnosticAdded(diag.kind);
    });

    // Remember hunk.
    mHunks.append(hunk);

    return hunk;
  }

  void stageHunks()
  {
    QBitArray hunks(mHunks.size());
    for (int i = 0; i < mHunks.size(); ++i)
      hunks[i] = mHunks.at(i)->header()->check()->isChecked();

    git::Index index = mDiff.index();
    if (hunks == QBitArray(hunks.size(), true)) {
      index.setStaged({mPatch.name()}, true);
      return;
    }

    if (hunks == QBitArray(hunks.size(), false)) {
      index.setStaged({mPatch.name()}, false);
      return;
    }

    QByteArray buffer = mPatch.apply(hunks);
    if (buffer.isEmpty())
      return;

    // Add the buffer to the index.
    index.add(mPatch.name(), buffer);
  }

signals:
  void diagnosticAdded(TextEditor::DiagnosticKind kind);

private:
  DiffView *mView;

  git::Diff mDiff;
  git::Patch mPatch;

  Header *mHeader;
  QList<InfoWidget *> mInfos;
  QList<ImageWidget *> mImages;
  QList<HunkWidget *> mHunks;
};

} // anon. namespace

DiffView::DiffView(const git::Repository &repo, QWidget *parent)
  : QScrollArea(parent)
{
  setStyleSheet(kStyleSheet);
  setAcceptDrops(true);
  setWidgetResizable(true);
  setFocusPolicy(Qt::NoFocus);
  setContextMenuPolicy(Qt::ActionsContextMenu);

  mPlugins = Plugin::plugins(repo);

  // Update comments.
  if (Repository *remote = RepoView::parentView(this)->remoteRepo()) {
    connect(remote->account(), &Account::commentsReady, this, [this, remote](
      Repository *repo,
      const QString &oid,
      const Account::CommitComments &comments)
    {
      if (repo != remote)
        return;

      RepoView *view = RepoView::parentView(this);
      QList<git::Commit> commits = view->commits();
      if (commits.size() != 1 || oid != commits.first().id().toString())
        return;

      mComments = comments;

      // Invalidate editors.
      foreach (QWidget *widget, mFiles) {
        foreach (HunkWidget *hunk, static_cast<FileWidget *>(widget)->hunks())
          hunk->invalidate();
      }

      // Load commit comments.
      if (!canFetchMore())
        fetchMore();
    });
  }
}

DiffView::~DiffView() {}

QWidget *DiffView::file(int index)
{
  fetchAll(index);
  return mFiles.at(index);
}

void DiffView::setDiff(const git::Diff &diff)
{
  RepoView *view = RepoView::parentView(this);
  git::Repository repo = view->repo();

  // Disconnect signals.
  foreach (QMetaObject::Connection connection, mConnections)
    disconnect(connection);
  mConnections.clear();

  // Clear state.
  mFiles.clear();
  mStagedPatches.clear();
  mComments = Account::CommitComments();

  // Set data.
  mDiff = diff;

  // Create a new widget.
  QWidget *widget = new QWidget(this);
  setWidget(widget);

  // Disable painting the background.
  // This allows drawing content over the border shadow.
  widget->setStyleSheet(".QWidget {background-color: transparent}");

  // Begin layout.
  QVBoxLayout *layout = new QVBoxLayout(widget);
  layout->setSpacing(4);
  layout->setSizeConstraint(QLayout::SetMinAndMaxSize);

  if (!diff.isValid()) {
    if (repo.isHeadUnborn()) {
      QPushButton *button =
        new QPushButton(QIcon(":/file.png"), tr("Add new file"));
      button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
      button->setStyleSheet("color: #484848");
      button->setIconSize(QSize(32, 32));
      button->setFlat(true);

      QFont buttonFont = button->font();
      buttonFont.setPointSize(24);
      button->setFont(buttonFont);

      // Open new editor associated with this view.
      connect(button, &QPushButton::clicked, view, &RepoView::newEditor);

      QLabel *label =
        new QLabel(tr("Or drag files here to copy into the repository"));
      label->setStyleSheet("color: #696969");
      label->setAlignment(Qt::AlignHCenter);

      QFont labelFont = label->font();
      labelFont.setPointSize(18);
      label->setFont(labelFont);

      layout->addStretch();
      layout->addWidget(button, 0, Qt::AlignHCenter);
      layout->addWidget(label, 0, Qt::AlignHCenter);
      layout->addStretch();
    }

    return;
  }

  // Generate a diff between the head tree and index.
  if (diff.isStatusDiff()) {
    if (git::Reference head = repo.head()) {
      if (git::Commit commit = head.target()) {
        git::Diff stagedDiff = repo.diffTreeToIndex(commit.tree());
        for (int i = 0; i < stagedDiff.count(); ++i)
          mStagedPatches[stagedDiff.name(i)] = stagedDiff.patch(i);
      }
    }
  }

  if (canFetchMore())
    fetchMore();

  // Load patches on demand.
  QScrollBar *scrollBar = verticalScrollBar();
  mConnections.append(
    connect(scrollBar, &QScrollBar::valueChanged, [this](int value) {
      if (value > verticalScrollBar()->maximum() / 2 && canFetchMore())
        fetchMore();
    })
  );

  mConnections.append(
    connect(scrollBar, &QScrollBar::rangeChanged, [this](int min, int max) {
      if (max - min < this->widget()->height() / 2 && canFetchMore())
        fetchMore();
    })
  );

  // Request comments for this diff.
  if (Repository *remoteRepo = view->remoteRepo()) {
    QList<git::Commit> commits = view->commits();
    if (commits.size() == 1) {
      QString oid = commits.first().id().toString();
      remoteRepo->account()->requestComments(remoteRepo, oid);
    }
  }
}

bool DiffView::scrollToFile(int index)
{
  // Ensure that the given index is loaded.
  fetchAll(index);

  // Finish layout by processing events. May cause a new diff to
  // be loaded. In that case the scroll widget will be different.
  QWidget *ptr = widget();
  QCoreApplication::processEvents();
  if (widget() != ptr)
    return false;

  // Scroll to the widget.
  verticalScrollBar()->setValue(mFiles.at(index)->y());
  return true;
}

void DiffView::setFilter(const QStringList &paths)
{
  fetchAll();
  QSet<QString> set = QSet<QString>::fromList(paths);
  foreach (QWidget *widget, mFiles) {
    FileWidget *file = static_cast<FileWidget *>(widget);
    file->setVisible(set.isEmpty() || set.contains(file->name()));
  }
}

QList<TextEditor *> DiffView::editors()
{
  fetchAll();
  QList<TextEditor *> editors;
  foreach (QWidget *widget, mFiles) {
    foreach (HunkWidget *hunk, static_cast<FileWidget *>(widget)->hunks())
      editors.append(hunk->editor());
  }

  return editors;
}

void DiffView::ensureVisible(TextEditor *editor, int pos)
{
  HunkWidget *hunk = static_cast<HunkWidget *>(editor->parentWidget());
  hunk->header()->button()->setChecked(true);

  FileWidget *file = static_cast<FileWidget *>(hunk->parentWidget());
  file->header()->disclosureButton()->setChecked(true);

  int fileY = hunk->parentWidget()->y();
  int y = fileY + hunk->y() + editor->y() + editor->pointFromPosition(pos).y();

  QScrollBar *scrollBar = verticalScrollBar();
  int val = scrollBar->value();
  int step = scrollBar->pageStep();
  if (y < val) {
    scrollBar->setValue(y);
  } else if (y >= val + step) {
    scrollBar->setValue(y - step + editor->textHeight(0));
  }
}

void DiffView::dropEvent(QDropEvent *event)
{
  if (event->dropAction() != Qt::CopyAction)
    return;

  event->acceptProposedAction();

  // Copy files into the workdir.
  RepoView *view = RepoView::parentView(this);
  git::Repository repo = view->repo();
  foreach (const QUrl &url, event->mimeData()->urls()) {
    if (url.isLocalFile())
      copy(url.toString(kUrlFormat), repo.workdir());
  }

  // FIXME: Work dir changed?
  view->refresh();
}

void DiffView::dragEnterEvent(QDragEnterEvent *event)
{
  if (event->mimeData()->hasUrls())
    event->acceptProposedAction();
}

bool DiffView::canFetchMore()
{
  return (mDiff.isValid() && mFiles.size() < mDiff.count());
}

void DiffView::fetchMore()
{
  QVBoxLayout *layout = static_cast<QVBoxLayout *>(widget()->layout());

  // Add widgets.
  int init = mFiles.size();
  int patchCount = mDiff.count();
  RepoView *view = RepoView::parentView(this);
  for (int pidx = init; pidx < patchCount && pidx - init < 8; ++pidx) {
    git::Patch patch = mDiff.patch(pidx);
    if (!patch.isValid()) {
      // This diff is stale. Refresh the view.
      QTimer::singleShot(0, view, &RepoView::refresh);
      return;
    }

    git::Patch staged = mStagedPatches.value(patch.name());
    FileWidget *file = new FileWidget(this, mDiff, patch, staged, widget());
    layout->addWidget(file);

    mFiles.append(file);

    if (file->isEmpty()) {
      DisclosureButton *button = file->header()->disclosureButton();
      button->setChecked(false);
      button->setEnabled(false);
    }

    // Respond to diagnostic signal.
    connect(file, &FileWidget::diagnosticAdded,
            this, &DiffView::diagnosticAdded);
  }

  // Finish layout.
  if (mFiles.size() == mDiff.count()) {
    // Add comments widget.
    if (!mComments.comments.isEmpty())
      layout->addWidget(new CommentWidget(mComments.comments, widget()));

    layout->addStretch();
  }
}

void DiffView::fetchAll(int index)
{
  // Load all patches up to and including index.
  while ((index < 0 || mFiles.size() <= index) && canFetchMore())
    fetchMore();
}

#include "DiffView.moc"
