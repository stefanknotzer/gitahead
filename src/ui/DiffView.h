//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef DIFFVIEW_H
#define DIFFVIEW_H

#include "FindWidget.h"
#include "editor/TextEditor.h"
#include "git/Commit.h"
#include "git/Diff.h"
#include "git/Index.h"
#include "host/Account.h"
#include "plugins/Plugin.h"
#include <QMap>
#include <QScrollArea>

class QCheckBox;
class QVBoxLayout;

class DiffView : public QScrollArea, public EditorProvider
{
  Q_OBJECT

public:
  DiffView(const git::Repository &repo, QWidget *parent = nullptr);
  virtual ~DiffView();

  QWidget *file(int index);

  void setDiff(const git::Diff &diff);

  bool scrollToFile(int index);
  void setFilter(const QList<int> &indexes);

  const QList<PluginRef> &plugins() const { return mPlugins; }
  const Account::CommitComments &comments() const { return mComments; }

  QList<TextEditor *> editors() override;
  bool isEditorSelection() override { return mVisibleFiles >= 0; }
  void ensureVisible(TextEditor *editor, int pos) override;

  int borderWidth();
  bool stageRequest(int index, bool staged);

signals:
  void diagnosticAdded(TextEditor::DiagnosticKind kind);

protected:
  void dropEvent(QDropEvent *event) override;
  void dragEnterEvent(QDragEnterEvent *event) override;

private:
  bool canFetchMore();
  void fetchMore(int count = 8);
  void fetchAll(int index = -1);

  git::Diff mDiff;
  QMap<QString,git::Patch *> mStagedPatches;

  QList<QWidget *> mFiles;
  QList<QMetaObject::Connection> mConnections;

  QList<PluginRef> mPlugins;
  Account::CommitComments mComments;

  int mVisibleFiles = -1;
  int mBorderWidth = -1;

  bool mDisclosureHunks = false;
  bool mDisclosureContents = false;
  bool mDisclosureFiles = false;
};

#endif
