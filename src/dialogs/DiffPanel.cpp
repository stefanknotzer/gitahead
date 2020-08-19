//
//          Copyright (c) 2017, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "DiffPanel.h"
#include "ConfigDialog.h"
#include "conf/Settings.h"
#include "git/Config.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSpinBox>
#include <QTextCodec>

DiffPanel::DiffPanel(const git::Repository &repo, QWidget *parent)
  : QWidget(parent), mConfig(repo ? repo.config() : git::Config::global())
{
  // diff context
  QSpinBox *context = new QSpinBox(this);
  QLabel *contextLabel = new QLabel(tr("lines"), this);
  QHBoxLayout *contextLayout = new QHBoxLayout;
  contextLayout->addWidget(context);
  contextLayout->addWidget(contextLabel);
  contextLayout->addStretch();
  context->setValue(mConfig.value<int>("diff.context", 3));

  auto contextSignal = QOverload<int>::of(&QSpinBox::valueChanged);
  connect(context, contextSignal, [this](int value) {
    mConfig.setValue("diff.context", value);
    foreach (MainWindow *window, MainWindow::windows()) {
      for (int i = 0; i < window->count(); ++i)
        window->view(i)->refresh();
    }
  });

  // encoding
  QComboBox *encoding = new QComboBox(this);
  encoding->addItem(tr("System Locale"), -1);
  encoding->insertSeparator(encoding->count());
  foreach (int mib, QTextCodec::availableMibs())
    encoding->addItem(QTextCodec::codecForMib(mib)->name(), mib);

  QString name = mConfig.value<QString>("gui.encoding");
  if (!name.isEmpty())
    encoding->setCurrentIndex(encoding->findText(name));

  auto encodingSignal = QOverload<int>::of(&QComboBox::currentIndexChanged);
  connect(encoding, encodingSignal, [this, encoding](int index) {
    if (encoding->itemData(index).toInt() < 0) {
      mConfig.remove("gui.encoding");
    } else {
      mConfig.setValue("gui.encoding", encoding->itemText(index));
    }

    foreach (MainWindow *window, MainWindow::windows()) {
      for (int i = 0; i < window->count(); ++i)
        window->view(i)->refresh();
    }
  });

  QFormLayout *layout = new QFormLayout(this);
  layout->addRow(tr("Context lines:"), contextLayout);
  layout->addRow(tr("Character Encoding:"), encoding);

  // Diff view.
  QCheckBox *binaryScaled = new QCheckBox(tr("Scaled"), this);
  binaryScaled->setChecked(mConfig.value<bool>("diffview.scaled", false));

  connect(binaryScaled, &QCheckBox::toggled, [this](bool checked) {
    mConfig.setValue("diffview.scaled", checked);

    foreach (MainWindow *window, MainWindow::windows()) {
      for (int i = 0; i < window->count(); ++i)
        window->view(i)->refresh();
    }
  });

  QComboBox *binaryView = new QComboBox(this);
  binaryView->addItems(QStringList({tr("Image/Icon"), tr("File Information"), tr("Image/Icon and Information"), tr("Nothing")}));
  if (repo) {
    binaryView->insertSeparator(binaryView->count());
    binaryView->addItem(tr("Reset to Global Default"));
  }
  binaryView->setCurrentIndex(mConfig.value<int>("diffview.binary", 0));

  connect(binaryView, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, binaryView, binaryScaled](int index) {
    if (index > 3) {
      mConfig.remove("diffview.binary");
      mConfig.remove("diffview.scaled");
//sk/TODO      binaryView->setCurrentIndex(mConfig.value<int>("diffview.binary", 0));
//sk/TODO      binaryScaled->setChecked(mConfig.value<bool>("diffview.scaled", false));
    } else {
      mConfig.setValue("diffview.binary", index);
    }

    foreach (MainWindow *window, MainWindow::windows()) {
      for (int i = 0; i < window->count(); ++i)
        window->view(i)->refresh();
    }
  });

  QComboBox *lfsView = new QComboBox(this);
  lfsView->addItems(QStringList({tr("Pointer"), tr("Object"), tr("File Information"), tr("Nothing")}));
  if (repo) {
    lfsView->insertSeparator(lfsView->count());
    lfsView->addItem(tr("Reset to Global Default"));
  }
  lfsView->setCurrentIndex(mConfig.value<int>("diffview.lfs", 0));

  connect(lfsView, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, lfsView](int index) {
    if (index > 3) {
      mConfig.remove("diffview.lfs");
//sk/TODO      lfsView->setCurrentIndex(mConfig.value<int>("diffview.lfs", 0));
    } else {
      mConfig.setValue("diffview.lfs", index);
    }

    foreach (MainWindow *window, MainWindow::windows()) {
      for (int i = 0; i < window->count(); ++i)
        window->view(i)->refresh();
    }
  });

  layout->addRow(tr("Binary View:"), binaryView);
  layout->addRow(tr("Image/Icon size:"), binaryScaled);
  layout->addRow(tr("LFS View:"), lfsView);

  // Remaining settings are strictly global.
  if (qobject_cast<ConfigDialog *>(parent))
    return;

  // ignore whitespace
  // The ignore whitespace option is global because it's
  // not a config setting. It's a flag (-w) to git diff.
  QCheckBox *ignoreWs = new QCheckBox(tr("Ignore Whitespace (-w)"), this);
  ignoreWs->setChecked(Settings::instance()->isWhitespaceIgnored());
  connect(ignoreWs, &QCheckBox::toggled, [](bool checked) {
    Settings::instance()->setWhitespaceIgnored(checked);
  });

  // auto collapse
  Settings *settings = Settings::instance();

  QCheckBox *collapseUntracked = new QCheckBox(tr("Untracked files"), this);
  collapseUntracked->setChecked(settings->value("collapse/untracked").toBool());
  connect(collapseUntracked, &QCheckBox::toggled, [settings](bool checked) {
    settings->setValue("collapse/untracked", checked);
  });

  QCheckBox *collapseAdded = new QCheckBox(tr("Added files"), this);
  collapseAdded->setChecked(settings->value("collapse/added").toBool());
  connect(collapseAdded, &QCheckBox::toggled, [settings](bool checked) {
    settings->setValue("collapse/added", checked);
  });

  QCheckBox *collapseModified = new QCheckBox(tr("Modified files"), this);
  collapseModified->setChecked(settings->value("collapse/modified").toBool());
  connect(collapseModified, &QCheckBox::toggled, [settings](bool checked) {
    settings->setValue("collapse/modified", checked);
  });

  QCheckBox *collapseDeleted = new QCheckBox(tr("Deleted files"), this);
  collapseDeleted->setChecked(settings->value("collapse/deleted").toBool());
  connect(collapseDeleted, &QCheckBox::toggled, [settings](bool checked) {
    settings->setValue("collapse/deleted", checked);
  });

  layout->addRow(tr("Whitespace:"), ignoreWs);
  layout->addRow(tr("Auto Collapse:"), collapseUntracked);
  layout->addRow(QString(), collapseAdded);
  layout->addRow(QString(), collapseModified);
  layout->addRow(QString(), collapseDeleted);
}
