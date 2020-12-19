//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "FileContextMenu.h"
#include "RepoView.h"
#include "conf/Settings.h"
#include "dialogs/SettingsDialog.h"
#include "git/Index.h"
#include "git/Patch.h"
#include "git/Tree.h"
#include "tools/EditTool.h"
#include "tools/ShowTool.h"
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QMessageBox>
#include <QPushButton>

namespace {

void warnRevisionNotFound(
  QWidget *parent,
  const QString &fragment,
  const QString &file)
{
  QString title = FileContextMenu::tr("Revision Not Found");
  QString text = FileContextMenu::tr(
    "The selected file doesn't have a %1 revision.").arg(fragment);
  QMessageBox msg(QMessageBox::Warning, title, text, QMessageBox::Ok, parent);
  msg.setInformativeText(file);
  msg.exec();
}

} // anon. namespace

FileContextMenu::FileContextMenu(
  RepoView *view,
  const QStringList &files,
  const git::Index &index,
  QWidget *parent)
  : QMenu(parent)
{
  // Show diff and merge tools for the currently selected diff.
  git::Diff diff = view->diff();
  git::Repository repo = view->repo();

  // Create external tools.
  QList<ExternalTool *> showTools;
  QList<ExternalTool *> editTools;
  QList<ExternalTool *> diffTools;
  QList<ExternalTool *> mergeTools;
  foreach (const QString &file, files) {
    // Convert to absolute path.
    QString path = repo.workdir().filePath(file);

    // Add show tool.
    showTools.append(new ShowTool(path, this));

    // Add edit tool.
    editTools.append(new EditTool(path, this));

    // Add diff or merge tool.
    if (ExternalTool *tool = ExternalTool::create(file, diff, repo, this)) {
      switch (tool->kind()) {
        case ExternalTool::Diff:
          diffTools.append(tool);
          break;

        case ExternalTool::Merge:
          mergeTools.append(tool);
          break;

        default:
          Q_ASSERT(false);
          break;
      }

      connect(tool, &ExternalTool::error, [this](ExternalTool::Error error) {
        if (error != ExternalTool::BashNotFound)
          return;

        QString title = tr("Bash Not Found");
        QString text = tr("Bash was not found on your PATH.");
        QMessageBox msg(QMessageBox::Warning, title, text, QMessageBox::Ok, this);
        msg.setInformativeText(tr("Bash is required to execute external tools."));
        msg.exec();
      });
    }
  }

  // Add external tool actions.
  addExternalToolsAction(showTools);
  addExternalToolsAction(editTools);
  addExternalToolsAction(diffTools);
  addExternalToolsAction(mergeTools);

  if (!isEmpty())
    addSeparator();

  QList<git::Commit> commits = view->commits();
  if (commits.isEmpty()) {
    if (index.isValid()) {
      QString statusText;
      git_delta_t status = GIT_DELTA_UNMODIFIED;

      // Single file selected.
      if (files.size() == 1) {
        int idx = diff.indexOf(files.first());
        if (idx >= 0) {
          status = diff.status(idx);
          switch (diff.status(idx)) {
            case GIT_DELTA_ADDED:     statusText = tr("Added");     break;
            case GIT_DELTA_DELETED:   statusText = tr("Deleted");   break;
            case GIT_DELTA_MODIFIED:  statusText = tr("Modified");  break;
            case GIT_DELTA_UNTRACKED: statusText = tr("Untracked"); break;
            default:                                                break;
          }
        }
      }

      // Stage/Unstage
      QAction *stage = addAction(tr("Stage"), [view, files] {
        view->stageFiles(files, true);
      });

      QAction *stageAll = addAction(tr("Stage All %1 Files").arg(statusText),
      [view, diff, status] {
        QStringList files;
        for (int i = 0; i < diff.count(); i++) {
          if (diff.status(i) == status)
            files.append(diff.name(i));
        }
        if (!files.isEmpty())
          view->stageFiles(files, true);
      });

      QAction *unstage = addAction(tr("Unstage"), [view, files] {
        view->stageFiles(files, false);
      });

      QAction *unstageAll = addAction(tr("Unstage All %1 Files").arg(statusText),
      [view, diff, status] {
        QStringList files;
        for (int i = 0; i < diff.count(); i++) {
          if (diff.status(i) == status)
            files.append(diff.name(i));
        }
        if (!files.isEmpty())
          view->stageFiles(files, false);
      });

      int staged = 0;
      int unstaged = 0;
      bool resolved = true;
      foreach (const QString &file, files) {
        switch (index.isStaged(file)) {
          case git::Index::Disabled:
            break;

          case git::Index::Unstaged:
            ++unstaged;
            break;

          case git::Index::PartiallyStaged: {
            ++staged;
            ++unstaged;

            int idx = diff.indexOf(file);
            for (int i = 0; i < diff.patch(idx)->count(); i++)
              if (diff.patch(idx)->conflictResolution(i) == git::Patch::Unresolved)
                resolved = false;
            break;
          }

          case git::Index::Staged:
            ++staged;
            break;

          case git::Index::Conflicted: {
            ++unstaged;

            int idx = diff.indexOf(file);
            for (int i = 0; i < diff.patch(idx)->count(); i++)
              if (diff.patch(idx)->conflictResolution(i) == git::Patch::Unresolved)
                resolved = false;
            break;
          }
        }
      }

      stage->setEnabled((unstaged > 0) && resolved);
      unstage->setEnabled((staged > 0) && resolved);
      stageAll->setVisible((unstaged > 0) && !statusText.isEmpty());
      unstageAll->setVisible((staged > 0) && !statusText.isEmpty());

      addSeparator();
    }

    // Remove and discard
    QStringList modified;
    QStringList untracked;
    if (diff.isValid()) {
      foreach (const QString &file, files) {
        int index = diff.indexOf(file);
        if (index < 0)
          continue;

        if (diff.status(index) == GIT_DELTA_UNTRACKED)
          untracked.append(file);
        else if (diff.status(index) != GIT_DELTA_UNMODIFIED)
          modified.append(file);
      }
    }

    QAction *remove = addAction(tr("Remove Untracked Files"), [view, untracked] {
      view->promptToRemove(untracked);
    });
    remove->setEnabled(!untracked.isEmpty());

    QAction *discard = addAction(tr("Discard Changes"), [view, modified] {
      view->promptToDiscard(view->repo().head().target(), modified);
    });
    discard->setEnabled(!modified.isEmpty());

    // Ignore
    QAction *ignore = addAction(tr("Ignore"), [view, files] {
      foreach (const QString &file, files)
        view->ignore(file);
    });

    if (!diff.isValid()) {
      ignore->setEnabled(false);
    } else {
      foreach (const QString &file, files) {
        int index = diff.indexOf(file);
        if (index < 0)
          continue;

        if (diff.status(index) != GIT_DELTA_UNTRACKED) {
          ignore->setEnabled(false);
          break;
        }
      }
    }

  } else {
    // Checkout
    QAction *checkout = addAction(tr("Checkout"), [this, view, files] {
      view->checkout(view->commits().first(), files);
      view->setViewMode(RepoView::Diff);
    });

    checkout->setEnabled(!view->repo().isBare());

    git::Commit commit = commits.first();
    foreach (const QString &file, files) {
      if (commit.tree().id(file) == repo.workdirId(file)) {
        checkout->setEnabled(false);
        break;
      }
    }
  }

  // LFS
  if (repo.lfsIsInitialized()) {
    addSeparator();

    bool locked = false;
    foreach (const QString &file, files) {
      if (repo.lfsIsLocked(file)) {
        locked = true;
        break;
      }
    }

    addAction(locked ? tr("Unlock") : tr("Lock"), [view, files, locked] {
      view->lfsSetLocked(files, !locked);
    });
  }

  // Add single selection actions.
  if (files.size() == 1) {
    addSeparator();

    // Copy File Name
    QDir dir = repo.workdir();
    QString file = files.first();
    QString rel = QDir::toNativeSeparators(file);
    QString abs = QDir::toNativeSeparators(dir.filePath(file));
    QString name = QFileInfo(file).fileName();
    QMenu *copy = addMenu(tr("Copy File Name"));
    if (!name.isEmpty() && name != file) {
      copy->addAction(name, [name] {
        QApplication::clipboard()->setText(name);
      });
    }
    copy->addAction(rel, [rel] {
      QApplication::clipboard()->setText(rel);
    });
    copy->addAction(abs, [abs] {
      QApplication::clipboard()->setText(abs);
    });

    addSeparator();

    // History
    addAction(tr("Filter History"), [view, file] {
      view->setPathspec(file);
    });

    // Navigate
    QMenu *navigate = addMenu(tr("Navigate to"));
    QAction *nextAct = navigate->addAction(tr("Next Revision"));
    connect(nextAct, &QAction::triggered, [view, file] {
      if (git::Commit next = view->nextRevision(file)) {
        view->selectCommit(next, file);
      } else {
        warnRevisionNotFound(view, tr("next"), file);
      }
    });

    QAction *prevAct = navigate->addAction(tr("Previous Revision"));
    connect(prevAct, &QAction::triggered, [view, file] {
      if (git::Commit prev = view->previousRevision(file)) {
        view->selectCommit(prev, file);
      } else {
        warnRevisionNotFound(view, tr("previous"), file);
      }
    });

    if (index.isValid() && index.isStaged(file)) {
      addSeparator();

      // Executable
      git_filemode_t mode = index.mode(file);
      bool exe = (mode == GIT_FILEMODE_BLOB_EXECUTABLE);
      QString exeName = exe ? tr("Unset Executable") : tr("Set Executable");
      QAction *exeAct = addAction(exeName, [index, file, exe] {
        git::Index(index).setMode(
          file, exe ? GIT_FILEMODE_BLOB : GIT_FILEMODE_BLOB_EXECUTABLE);
      });

      exeAct->setEnabled(exe || mode == GIT_FILEMODE_BLOB);
    }
  }
}

void FileContextMenu::addExternalToolsAction(
  const QList<ExternalTool *> &tools)
{
  if (tools.isEmpty())
    return;

  // Add action.
  QAction *action = addAction(tools.first()->name(), [this, tools] {
    foreach (ExternalTool *tool, tools) {
      if (tool->start())
        return;

      QString kind;
      switch (tool->kind()) {
        case ExternalTool::Show:
          return;

        case ExternalTool::Edit:
          kind = tr("edit");
          break;

        case ExternalTool::Diff:
          kind = tr("diff");
          break;

        case ExternalTool::Merge:
          kind = tr("merge");
          break;
      }

      QString title = tr("External Tool Not Found");
      QString text = tr("Failed to execute external %1 tool.");
      QMessageBox::warning(this, title, text.arg(kind), QMessageBox::Ok);
      SettingsDialog::openSharedInstance(SettingsDialog::Tools);
    }
  });

  // Disable if any tools are invalid.
  foreach (ExternalTool *tool, tools) {
    if (!tool->isValid()) {
      action->setEnabled(false);
      break;
    }
  }
}
