//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "DiffWidget.h"
#include "DiffView.h"
#include "FileList.h"
#include "FindWidget.h"
#include "RepoView.h"
#include "conf/Settings.h"
#include "git/Commit.h"
#include "git/Diff.h"
#include "git/Index.h"
#include <QApplication>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QVBoxLayout>

namespace {

const QItemSelectionModel::SelectionFlags kSelectionFlags =
  QItemSelectionModel::Current;

} // anon. namespace

DiffWidget::DiffWidget(const git::Repository &repo, QWidget *parent)
  : ContentWidget(parent)
{
  mFiles = new FileList(repo, this);
  mFiles->hide(); // Start hidden.

  mDiffView = new DiffView(repo, this);

  mFind = new FindWidget(mDiffView, this);
  mFind->hide(); // Start hidden.

  QWidget *view = new QWidget(this);
  QVBoxLayout *viewLayout = new QVBoxLayout(view);
  viewLayout->setContentsMargins(0,0,0,0);
  viewLayout->setSpacing(0);
  viewLayout->addWidget(mFind);
  viewLayout->addWidget(mDiffView);

  mSplitter = new QSplitter(Qt::Vertical, this);
  mSplitter->setChildrenCollapsible(false);
  mSplitter->setHandleWidth(0);
  mSplitter->addWidget(mFiles);
  mSplitter->addWidget(view);
  mSplitter->setStretchFactor(1, 1);
  connect(mSplitter, &QSplitter::splitterMoved, [this] {
    FileList::setFileRows(mFiles->height() / mFiles->sizeHintForRow(0));
  });

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0,0,0,0);
  layout->addWidget(mSplitter);

  // Sort request.
  connect(mFiles, &FileList::sortRequested, [this] {
    setDiff(mDiff);
  });

  // Conflict resolution status update.
  connect(repo.notifier(), &git::RepositoryNotifier::statusChanged, [this] {
    mFiles->update();
  });
}

QString DiffWidget::selectedFile() const
{
  QModelIndexList indexes = mFiles->selectionModel()->selectedIndexes();
  return indexes.isEmpty() ? QString() :
         indexes.first().data(Qt::DisplayRole).toString();
}

void DiffWidget::setDiff(
  const git::Diff &diff,
  const QString &file,
  const QString &pathspec)
{
  // Disconnect signals.
  foreach (QMetaObject::Connection connection, mConnections)
    disconnect(connection);
  mConnections.clear();

  mDiff = diff;

  // Cancel find.
  mFind->hide();

  // Populate views.
  if (mDiff.isValid()) {
    if (!mDiff.isConflicted()) {
      Settings *settings = Settings::instance();
      QMap<int, QByteArray> sortMap;
      QByteArray inArray;
      inArray = settings->value("sort/map").toByteArray();
      QDataStream inStream(&inArray, QIODevice::ReadOnly);
      inStream >> sortMap;

      if (sortMap.count()) {
        QList<git::Diff::SortRole> roleList;
        QList<Qt::SortOrder> orderList;
        for (int i = 0; i < sortMap.count(); i++) {
          QByteArray ba = sortMap.value(i, QByteArray(2, -1));
          roleList.append(static_cast<git::Diff::SortRole>(ba.at(0)));
          orderList.append(static_cast<Qt::SortOrder>(ba.at(1)));
        }

        mDiff.sort(roleList, orderList);
      } else {
        mDiff.sort(git::Diff::NameRole, Qt::AscendingOrder);
      }
    } else {
      mDiff.sort(git::Diff::StatusRole, Qt::DescendingOrder);
    }
  }

  // Setup FileList, splitter and DiffView.
  mFiles->setDiff(diff, pathspec);
  mSplitter->setSizes({mFiles->sizeHint().height(), -1});
  mDiffView->setDiff(diff);

  // Reset find.
  mFind->reset();

  // Scroll to file.
  selectFile(file);

  // Filter on selection change.
  QItemSelectionModel *selection = mFiles->selectionModel();
  mConnections.append(
    connect(selection, &QItemSelectionModel::selectionChanged, [this] {
      // Reset find.
      mFind->reset();

      QList<int>indexes;
      QModelIndexList selectedIndexes = mFiles->selectionModel()->selectedIndexes();
      foreach (const QModelIndex &selectedIndex, selectedIndexes)
        indexes.append(selectedIndex.row());

      // Apply DiffView filter.
      QApplication::setOverrideCursor(QCursor(Qt::BusyCursor));
      mDiffView->setFilter(indexes);
      QApplication::restoreOverrideCursor();

      // Adjust DiffView scroll to match the FileList scroll.
      if (indexes.isEmpty()) {
        QScrollBar *scroll = mFiles->verticalScrollBar();
        if (scroll != nullptr)
          mDiffView->scrollToFile(scroll->value());
      }
    })
  );

  // Syncronize DiffView scroll and FileList scroll.
  mConnections.append(
    connect(mDiffView->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &DiffWidget::setCurrentFile)
  );
}

void DiffWidget::find()
{
  mFind->showAndSetFocus();
}

void DiffWidget::findNext()
{
  mFind->find();
}

void DiffWidget::findPrevious()
{
  mFind->find(FindWidget::Backward);
}

bool DiffWidget::writeFile(const QString &file, bool staged)
{
  bool stage = true;

  if (mDiff.isValid()) {
    // Write merge resolution to disk.
    if (!mDiffView->writeResolution(mDiff.indexOf(file), staged))
      stage = false;
  }
  return stage;
}

void DiffWidget::selectFile(const QString &file)
{
  if (file.isEmpty())
    return;

  QAbstractItemModel *model = mFiles->model();
  QModelIndex start = model->index(0, 0);
  QModelIndexList indexes = model->match(start, Qt::DisplayRole, file);

  // The file might not be found if it was renamed.
  // FIXME: Look up the old name from the blame?
  if (!indexes.isEmpty()) {
    QModelIndex index = indexes.first();
    if (mDiffView->scrollToFile(index.row()))
      mFiles->selectionModel()->select(index, kSelectionFlags);
  }
}

void DiffWidget::setCurrentFile(int value)
{
  // Scroll if selection is empty.
  if (!mFiles->selectionModel()->selection().isEmpty())
    return;

  QAbstractItemModel *model = mFiles->model();
  int rows = model->rowCount();
  for (int i = 0; i < rows; ++i) {
    if (!mDiffView->file(i)->isVisible())
      continue;

    // Get the position of the next widget
    // respecting the border between the widgets.
    int pos = 0;
    if (i < rows - 1)
      pos = mDiffView->file(i + 1)->y() - mDiffView->borderWidth() - 1;
    else
      pos = mDiffView->widget()->height();

    // Stop at the first index where the scroll
    // value is less than the next widget.
    if (value < pos) {
      QModelIndex index = model->index(i, 0);
      mFiles->selectionModel()->select(index, kSelectionFlags);
      mFiles->scrollToBottom();
      mFiles->scrollTo(index);
      break;
    }
  }
}
