//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "Patch.h"
#include "Blob.h"
#include "Id.h"
#include "Repository.h"
#include "git/Buffer.h"
#include "git/Submodule.h"
#include "git2/filter.h"

namespace git {

Patch::Patch() {}

Patch::Patch(git_patch *patch)
  : d(patch, git_patch_free)
{
  if (!isValid())
    return;

  mRepo = new Repository(git_patch_owner(patch));
  if ((mRepo == nullptr) || !mRepo->isValid())
    return;

  mStatus = git_patch_get_delta(d.data())->status;
  mIsBinary = git_patch_get_delta(d.data())->flags & GIT_DIFF_FLAG_BINARY;

  QFile file(mRepo->workdir().filePath(name(Diff::OldFile)));
  if (file.open(QFile::ReadOnly)) {
    QList<QByteArray> lines;
    bool conflicted = isConflicted();
    QByteArray line = file.readLine(1024 * 1024);

    while (!line.isEmpty()) {
      git::Buffer buffer(line.constData(), line.length());

      // Detect binary file.
      if (buffer.isBinary()) {
        lines.clear();
        mIsBinary = true;
        break;
      }

      // Read conflict hunks.
      if (conflicted)
        lines.append(line);

      line = file.readLine();
    }

    file.close();

    // Conflict hunks evaluation.
    if (!lines.isEmpty()) {
      int lineCount = lines.size();
      int context = git_patch_context_lines(patch);
      for (int i = 0; i < lineCount; ++i) {
        if (!lines.at(i).startsWith("<<<<<<<"))
          continue;

        int mid = -1;
        for (int j = i + 1; j < lineCount; ++j) {
          if (!lines.at(j).startsWith("======="))
            continue;

          mid = j;
          break;
        }

        if (mid < 0)
          break;

        for (int j = mid + 1; j < lineCount; ++j) {
          if (!lines.at(j).startsWith(">>>>>>>"))
            continue;

          // Add conflict.
          int line = qMax(0, i - context);
          int count = qMin(lineCount, j + context + 1) - line;
          mConflicts.append({line, i, mid, j, lines.mid(line, count)});

          i = j + 1;
          break;
        }
      }
    }
  }

  // Check if file is a lfs pointer.
  mIsLfsPointer = false;
  if ((count() > 0) && (lineCount(0) > 0)) {
    QByteArray line = lineContent(0, 0).trimmed();
    if (line == "version https://git-lfs.github.com/spec/v1")
      mIsLfsPointer = true;
  }

  // Check if file is a submodule.
  mIsSubmodule = false;
  if (!mIsBinary && !mIsLfsPointer)
    mIsSubmodule = mRepo->lookupSubmodule(name()).isValid();
}

QString Patch::name(Diff::File file) const
{
  if (!isValid()) // Patch data is invalid
    return QString();

  const git_diff_delta *delta = git_patch_get_delta(d.data());
  return (file == Diff::NewFile) ? delta->new_file.path : delta->old_file.path;
}

bool Patch::isResolved() const
{
  if (mStatus == GIT_DELTA_CONFLICTED) {
    if (mIsBinary)
      return conflictResolution(-1) != git::Patch::Unresolved;

    for (int i = 0; i < mConflicts.size(); i++)
      if (conflictResolution(i) == git::Patch::Unresolved)
        return false;
  }
  return true;
}

Blob Patch::blob(Diff::File file) const
{
  if (!isValid()) // Patch data is invalid
    return Blob();

  git_repository *repo = git_patch_owner(d.data());
  if (!repo)
    return Blob();

  const git_diff_delta *dd = git_patch_get_delta(d.data());
  const git_oid &id = (file == Diff::NewFile) ? dd->new_file.id : dd->old_file.id;

  git_object *obj = nullptr;
  git_object_lookup(&obj, repo, &id, GIT_OBJECT_BLOB);
  return Blob(reinterpret_cast<git_blob *>(obj));
}

Patch::LineStats Patch::lineStats() const
{
  size_t context;
  size_t additions = 0;
  size_t deletions = 0;

  if (isValid())
    git_patch_line_stats(&context, &additions, &deletions, d.data());

  LineStats stats;
  stats.additions = additions;
  stats.deletions = deletions;
  return stats;
}

int Patch::count() const
{
  if (!isValid()) // Patch data is invalid
    return 0;

  if (isConflicted())
    return mConflicts.size();

  return git_patch_num_hunks(d.data());
}

QByteArray Patch::header(int index) const
{
  if (isConflicted())
    return QByteArray();

  const git_diff_hunk *hunk = nullptr;
  int result = git_patch_get_hunk(&hunk, nullptr, d.data(), index);
  return !result ? hunk->header : QByteArray();
}

int Patch::lineCount(int index) const
{
  if (!isValid()) // Patch data is invalid
    return 0;

  if (isConflicted())
    return mConflicts.at(index).lines.size();

  return git_patch_num_lines_in_hunk(d.data(), index);
}

char Patch::lineOrigin(int index, int ln) const
{
  if (isConflicted()) {
    const ConflictHunk &conflict = mConflicts.at(index);
    int line = conflict.line + ln;
    if (line < conflict.min || line > conflict.max)
      return GIT_DIFF_LINE_CONTEXT;

    if (line == conflict.min)
      return 'L'; // <<<<<<<

    if (line == conflict.mid)
      return 'E'; // =======

    if (line == conflict.max)
      return 'G'; // >>>>>>>

    return (line < conflict.mid) ? 'O' : 'T';
  }

  const git_diff_line *line = nullptr;
  int result = git_patch_get_line_in_hunk(&line, d.data(), index, ln);
  return !result ? line->origin : GIT_DIFF_LINE_CONTEXT;
}

int Patch::lineNumber(int index, int ln, Diff::File file) const
{
  if (index < 0)
    return index;

  if (isConflicted())
    return mConflicts.at(index).line + ln;

  const git_diff_line *line = nullptr;
  if (git_patch_get_line_in_hunk(&line, d.data(), index, ln))
    return -1;

  return (file == Diff::NewFile) ? line->new_lineno : line->old_lineno;
}

QByteArray Patch::lineContent(int index, int ln) const
{
  if (isConflicted())
    return mConflicts.at(index).lines.at(ln);

  const git_diff_line *line = nullptr;
  int result = git_patch_get_line_in_hunk(&line, d.data(), index, ln);
  return !result ? QByteArray(line->content, line->content_len) : QByteArray();
}

Patch::ConflictResolution Patch::conflictResolution(int index) const
{
  int resolution = mRepo->conflictResolution(name(), index);
  if (resolution < 0)
    return Unresolved;

  return static_cast<ConflictResolution>(resolution);
}

void Patch::setConflictResolution(int index, ConflictResolution resolution) const
{
  mRepo->setConflictResolution(name(), index, resolution);
}

QByteArray Patch::apply(
  const QBitArray &hunks,
  const FilterList &filters) const
{
  // Populate preimage.
  QList<QList<QByteArray>> image;
  QByteArray source = blob(Diff::OldFile).content();

  int index = 0;
  int newline = source.indexOf('\n');
  while (newline >= 0) {
    image.append({source.mid(index, newline - index + 1)});
    index = newline + 1;
    newline = source.indexOf('\n', index);
  }

  image.append({source.mid(index)});

  // Apply hunks.
  for (int i = 0; i < hunks.size(); ++i) {
    if (!hunks.at(i))
      continue;

    size_t lines = 0;
    const git_diff_hunk *hunk = nullptr;
    if (git_patch_get_hunk(&hunk, &lines, d.data(), i))
      continue;

    // FIXME: Incorrectly prepends when there are zero lines
    // of context and there's an addition after the first line.
    int index = hunk->old_start ? hunk->old_start - 1 : 0;
    bool prepend = (index == 0);
    for (size_t j = 0; j < lines; ++j) {
      const git_diff_line *line = nullptr;
      if (git_patch_get_line_in_hunk(&line, d.data(), i, j))
        continue;

      if (line->old_lineno > 0)
        index = line->old_lineno - 1;

      switch (line->origin) {
        case GIT_DIFF_LINE_CONTEXT:
          prepend = false;
          break;

        case GIT_DIFF_LINE_ADDITION: {
          QByteArray text(line->content, line->content_len);
          image[index].insert(prepend ? 0 : image.at(index).size(), text);
          break;
        }

        case GIT_DIFF_LINE_DELETION:
          image[index].clear();
          prepend = false;
          break;

        default:
          break;
      }
    }
  }

  // Generate result.
  QByteArray result;
  foreach (const QList<QByteArray> &lines, image) {
    foreach (const QByteArray &line, lines)
      result.append(line);
  }

  if (!filters.isValid())
    return result;

  // Apply filters.
  git_buf out = GIT_BUF_INIT_CONST(nullptr, 0);
  git_buf raw = GIT_BUF_INIT_CONST(result.constData(), result.length());
  git_filter_list_apply_to_data(&out, filters, &raw);
  git_buf_dispose(&raw);

  QByteArray filtered(out.ptr, out.size);
  git_buf_dispose(&out);
  return filtered;
}

Patch Patch::fromBuffers(
  const QByteArray &oldBuffer,
  const QByteArray &newBuffer,
  const QString &oldPath,
  const QString &newPath)
{
  // FIXME: Pass context lines as a parameter?
  git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
  opts.context_lines = 0;

  git_patch *patch = nullptr;
  git_patch_from_buffers(
    &patch, oldBuffer.constData(), oldBuffer.length(), oldPath.toUtf8(),
    newBuffer.constData(), newBuffer.length(), newPath.toUtf8(), &opts);
  return Patch(patch);
}

} // namespace git
