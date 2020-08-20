//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "Config.h"
#include "conf/Settings.h"

namespace git {

namespace {

const QString kConfigFile = "config";

} // anon. namespace

Config::Entry::Entry(git_config_entry *entry, bool owned)
  : d(entry, owned ? git_config_entry_free : [](git_config_entry *) {})
{}

QString Config::Entry::name() const
{
  return d->name;
}

template <>
bool Config::Entry::value<bool>() const
{
  int value = 0;
  git_config_parse_bool(&value, d.data()->value);
  return value;
}

template <>
int Config::Entry::value<int>() const
{
  int value = 0;
  git_config_parse_int32(&value, d.data()->value);
  return value;
}

template <>
QString Config::Entry::value<QString>() const
{
  return d.data()->value ? d.data()->value : QString();
}

Config::Iterator::Iterator(git_config_iterator *iterator)
  : d(iterator, git_config_iterator_free)
{}

Config::Entry Config::Iterator::next() const
{
  git_config_entry *entry = nullptr;
  if (git_config_next(&entry, d.data()))
    return Config::Entry();

  return Entry(entry);
}

Config::Config(git_config *config)
  : d(config, git_config_free)
{}

bool Config::addFile(
  const QString &path,
  git_config_level_t level,
  const Repository &repo)
{
  git_repository *ptr =
    repo.isValid() ? static_cast<git_repository *>(repo) : nullptr;
  return !git_config_add_file_ondisk(d.data(), path.toUtf8(), level, ptr, false);
}

template <>
bool Config::value<bool>(const QString &key, const bool &defaultValue) const
{
  int value = defaultValue;
  git_config_get_bool(&value, d.data(), key.toUtf8());
  return value;
}

template <>
void Config::setValue<bool>(const QString &key, const bool &value)
{
  git_config_set_bool(d.data(), key.toUtf8(), value);
}

template <>
int Config::value<int>(const QString &key, const int &defaultValue) const
{
  int value = defaultValue;
  git_config_get_int32(&value, d.data(), key.toUtf8());
  return value;
}

template <>
void Config::setValue<int>(const QString &key, const int &value)
{
  git_config_set_int32(d.data(), key.toUtf8(), value);
}

template <>
QString Config::value<QString>(
  const QString &key,
  const QString &defaultValue) const
{
  git_buf buf = GIT_BUF_INIT_CONST(nullptr, 0);
  git_config_get_string_buf(&buf, d.data(), key.toUtf8());
  QString value = QString::fromUtf8(buf.ptr, buf.size);
  git_buf_dispose(&buf);
  return !value.isEmpty() ? value : defaultValue;
}

template <>
void Config::setValue<QString>(const QString &key, const QString &value)
{
  git_config_set_string(d.data(), key.toUtf8(), value.toUtf8());
}

bool Config::remove(const QString &key)
{
  return !git_config_delete_entry(d.data(), key.toUtf8());
}

Config::Iterator Config::glob(const QString &pattern) const
{
  git_config_iterator *iterator = nullptr;
  git_config_iterator_glob_new(&iterator, d.data(), pattern.toUtf8());
  return Iterator(iterator);
}

Config Config::global()
{
  git_config *config = nullptr;
  git_config_open_default(&config);
  return Config(config);
}

QString Config::globalPath()
{
  // Ensure that the global file exists.
  Config config = global();
  if (!config.isValid())
    return QString();

  config.setValue("global.force", true);
  config.remove("global.force");

  git_buf buf = GIT_BUF_INIT_CONST(nullptr, 0);
  git_config_find_global(&buf);
  QString path = QString::fromUtf8(buf.ptr, buf.size);
  git_buf_dispose(&buf);

  return path;
}

Config Config::appGlobal()
{
  git_config *config = nullptr;
  if (git_config_new(&config))
    return Config();

  QDir dir = Settings::userDir();

  // Create missing path.
  if (!dir.exists())
    dir.mkpath(dir.path());

  QByteArray path = dir.filePath(kConfigFile).toUtf8();
  if (git_config_add_file_ondisk(
        config, path, GIT_CONFIG_LEVEL_GLOBAL, nullptr, 0)) {
    git_config_free(config);
    return Config();
  }

  return Config(config);
}

Config Config::open(const QString &path)
{
  git_config *config = nullptr;
  git_config_open_ondisk(&config, path.toUtf8());
  return Config(config);
}

} // namespace git
