//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef TREEWIDGET_H
#define TREEWIDGET_H

#include "DetailView.h"

class BlameEditor;
class ColumnView;

namespace git {
class Diff;
class Repository;
}

class TreeWidget : public ContentWidget
{
  Q_OBJECT
  
public:
  TreeWidget(const git::Repository &repo, QWidget *parent = nullptr);

  QString selectedFile() const override;

  void setDiff(
    const git::Diff &diff,
    const QString &file = QString(),
    const QString &pathspec = QString()) override;

  void cancelBackgroundTasks() override;

  void find() override;
  void findNext() override;
  void findPrevious() override;
  bool writeFile(const QString &file, bool staged) override;

protected:
  void contextMenuEvent(QContextMenuEvent *event) override;

private:
  void edit(const QModelIndex &index);
  void loadEditorContent(const QModelIndex &index);

  void selectFile(const QString &name);

  ColumnView *mView;
  BlameEditor *mEditor;
};

#endif
