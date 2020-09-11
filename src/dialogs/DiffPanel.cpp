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
#include <QFormLayout>
#include <QLabel>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTextCodec>

DiffPanel::DiffPanel(const git::Repository &repo, QWidget *parent)
  : QWidget(parent), mConfig(repo ? repo.config() : git::Config::global()),
                     mAppConfig(repo ? repo.appConfig() : git::Config::appGlobal())
{
  bool global = repo ? false : true;

  // Diff context.
  mContext = new QSpinBox(this);
  QLabel *contextLabel = new QLabel(tr("lines"), this);
  QHBoxLayout *contextLayout = new QHBoxLayout;
  contextLayout->addWidget(mContext);
  contextLayout->addWidget(contextLabel);
  contextLayout->addStretch();

  // Encoding.
  mEncoding = new QComboBox(this);
  mEncoding->addItem(tr("System Locale"), -1);
  mEncoding->insertSeparator(mEncoding->count());
  foreach (int mib, QTextCodec::availableMibs())
    mEncoding->addItem(QTextCodec::codecForMib(mib)->name(), mib);

  QFormLayout *layout = new QFormLayout(this);
  layout->addRow(tr("Context lines:"), contextLayout);
  layout->addRow(tr("Character Encoding:"), mEncoding);

  // Diff view for binary files.
  mLoadBinary = new QCheckBox(tr("Load Picture/Icon for Binary Files"), this);
  mScaleBinary = new QCheckBox(tr("Miniaturized Picture/Icon View (Scaled)"), this);

  layout->addRow(tr("Binary View:"), mLoadBinary);
  layout->addRow(tr("Picture/Icon Size:"), mScaleBinary);

  // Remaining settings are strictly global.
  if (global) {
    QFrame *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    layout->addRow(QString(), line);

    // ignore whitespace
    // The ignore whitespace option is global because it's
    // not a config setting. It's a flag (-w) to git diff.
    mIgnoreWs = new QCheckBox(tr("Ignore Whitespace (-w)"), this);

    // Auto collapse diff view.
    mCollapseUntracked = new QCheckBox(tr("Untracked files"), this);
    mCollapseAdded = new QCheckBox(tr("Added files"), this);
    mCollapseModified = new QCheckBox(tr("Modified files"), this);
    mCollapseDeleted = new QCheckBox(tr("Deleted files"), this);

    layout->addRow(tr("Whitespace:"), mIgnoreWs);
    layout->addRow(tr("Auto Collapse:"), mCollapseUntracked);
    layout->addRow(QString(), mCollapseAdded);
    layout->addRow(QString(), mCollapseModified);
    layout->addRow(QString(), mCollapseDeleted);
  }

  refresh();

  // Connect signals after initializing fields.
  auto contextSignal = QOverload<int>::of(&QSpinBox::valueChanged);
  connect(mContext, contextSignal, [this](int value) {
    mConfig.setValue("diff.context", value);
    foreach (MainWindow *window, MainWindow::windows()) {
      for (int i = 0; i < window->count(); ++i)
        window->view(i)->refresh();
    }
  });

  auto encodingSignal = QOverload<int>::of(&QComboBox::currentIndexChanged);
  connect(mEncoding, encodingSignal, [this](int index) {
    if (mEncoding->itemData(index).toInt() < 0) {
      mConfig.remove("gui.encoding");
    } else {
      mConfig.setValue("gui.encoding", mEncoding->itemText(index));
    }

    foreach (MainWindow *window, MainWindow::windows()) {
      for (int i = 0; i < window->count(); ++i)
        window->view(i)->refresh();
    }
  });

  connect(mLoadBinary, &QCheckBox::toggled, [this](bool checked) {
    mAppConfig.setValue("diffview.loadbinary", checked);

    foreach (MainWindow *window, MainWindow::windows()) {
      for (int i = 0; i < window->count(); ++i)
        window->view(i)->refresh();
    }
  });

  connect(mScaleBinary, &QCheckBox::toggled, [this](bool checked) {
    mAppConfig.setValue("diffview.scalebinary", checked);

    foreach (MainWindow *window, MainWindow::windows()) {
      for (int i = 0; i < window->count(); ++i)
        window->view(i)->refresh();
    }
  });

  if (global) {
    connect(mIgnoreWs, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setWhitespaceIgnored(checked);
    });

    connect(mCollapseUntracked, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("collapse/untracked", checked);
    });

    connect(mCollapseAdded, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("collapse/added", checked);
    });

    connect(mCollapseModified, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("collapse/modified", checked);
    });

    connect(mCollapseDeleted, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("collapse/deleted", checked);
    });
  }
}

void DiffPanel::refresh(void)
{
  mContext->setValue(mConfig.value<int>("diff.context", 3));

  QString name = mConfig.value<QString>("gui.encoding");
  if (!name.isEmpty())
    mEncoding->setCurrentIndex(mEncoding->findText(name));

  mLoadBinary->setChecked(mAppConfig.value<bool>("diffview.loadbinary", true));
  mScaleBinary->setChecked(mAppConfig.value<bool>("diffview.scalebinary", false));

  if (mIgnoreWs == nullptr)
    return;

  mIgnoreWs->setChecked(Settings::instance()->isWhitespaceIgnored());

  mCollapseUntracked->setChecked(Settings::instance()->value("collapse/untracked").toBool());
  mCollapseAdded->setChecked(Settings::instance()->value("collapse/added").toBool());
  mCollapseModified->setChecked(Settings::instance()->value("collapse/modified").toBool());
  mCollapseDeleted->setChecked(Settings::instance()->value("collapse/deleted").toBool());
}
