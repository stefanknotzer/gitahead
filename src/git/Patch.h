//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef PATCH_H
#define PATCH_H

#include "Diff.h"
#include "FilterList.h"
#include "git2/patch.h"
#include <QBitArray>
#include <QSharedPointer>

namespace git {

class Blob;
class Id;
class Repository;

class Patch
{
public:
  enum ConflictResolution
  {
    Unresolved,
    Ours,
    Theirs
  };

  struct LineStats
  {
    int additions;
    int deletions;
  };

  Patch();

  bool isValid() const { return !d.isNull(); }

  Repository *repo() const { return mRepo; }

  QString name(Diff::File file = Diff::NewFile) const;
  git_delta_t status() const { return mStatus; }
  bool isUntracked() const { return (mStatus == GIT_DELTA_UNTRACKED); }
  bool isConflicted() const { return (mStatus == GIT_DELTA_CONFLICTED); }
  bool isResolved() const;
  bool isBinary() const { return mIsBinary; }
  bool isLfsPointer() const { return mIsLfsPointer; }
  bool isSubmodule() const { return mIsSubmodule; }

  Blob blob(Diff::File file) const;

  LineStats lineStats() const;

  int count() const;
  QByteArray header(int index) const;

  int lineCount(int index) const;
  char lineOrigin(int index, int line) const;
  int lineNumber(int index, int line, Diff::File file = Diff::NewFile) const;
  QByteArray lineContent(int index, int line) const;

  ConflictResolution conflictResolution(int index) const;
  void setConflictResolution(int index, ConflictResolution resolution) const;

  // Apply the given hunk indexes to the old buffer.
  QByteArray apply(
    const QBitArray &hunks,
    const FilterList &filters = FilterList()) const;

  static Patch fromBuffers(
    const QByteArray &oldBuffer,
    const QByteArray &newBuffer,
    const QString &oldPath = QString(),
    const QString &newPath = QString());

private:
  struct ConflictHunk
  {
    int line; // start line
    int min; // <<<<<<< line
    int mid; // ======= line
    int max; // >>>>>>> line
    QList<QByteArray> lines;
  };

  Patch(git_patch *patch);

  QSharedPointer<git_patch> d;
  Repository *mRepo;
  QList<ConflictHunk> mConflicts;

  git_delta_t mStatus = GIT_DELTA_UNMODIFIED;

  bool mIsBinary = false;
  bool mIsLfsPointer = false;
  bool mIsSubmodule = false;

  friend class Diff;
};

} // namespace git

#endif
