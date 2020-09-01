//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "SettingsDialog.h"
#include "AboutDialog.h"
#include "DiffPanel.h"
#include "ExternalToolsDialog.h"
#include "PluginsPanel.h"
#include "app/Application.h"
#include "app/CustomTheme.h"
#include "conf/Settings.h"
#include "cred/CredentialHelper.h"
#include "git/Config.h"
#include "log/LogEntry.h"
#include "tools/ExternalTool.h"
#include "ui/BlameEditor.h"
#include "ui/EditorWindow.h"
#include "ui/MainWindow.h"
#include "ui/MenuBar.h"
#include "ui/RepoView.h"
#include "update/Updater.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFile>
#include <QFontComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QToolBar>

#ifdef Q_OS_UNIX
#include "cli/Installer.h"
#endif

namespace {

void populateExternalTools(QComboBox *comboBox, const QString &type)
{
  comboBox->clear();

  QList<ExternalTool::Info> tools =
    ExternalTool::readGlobalTools(type) +
    ExternalTool::readBuiltInTools(type);

  QStringList names;
  foreach (const ExternalTool::Info &tool, tools) {
    if (tool.found)
      names.append(tool.name);
  }

  std::sort(names.begin(), names.end());
  foreach (const QString &tool, names)
    comboBox->addItem(tool);
}

class StackedWidget : public QStackedWidget
{
public:
  StackedWidget(QWidget *parent = nullptr)
    : QStackedWidget(parent)
  {}

  QSize sizeHint() const override
  {
    return currentWidget()->sizeHint();
  }

  QSize minimumSizeHint() const override
  {
    return currentWidget()->minimumSizeHint();
  }
};

class GeneralPanel : public QWidget
{
  Q_OBJECT

public:
  GeneralPanel(QWidget *parent = nullptr)
    : QWidget(parent)
  {
    mName = new QLineEdit(this);
    mEmail = new QLineEdit(this);

    mFetch = new QCheckBox(tr("Fetch every"), this);
    mFetchMinutes = new QSpinBox(this);
    connect(mFetch, &QCheckBox::toggled,
            mFetchMinutes, &QSpinBox::setEnabled);

    QHBoxLayout *fetchLayout = new QHBoxLayout;
    fetchLayout->addWidget(mFetch);
    fetchLayout->addWidget(mFetchMinutes);
    fetchLayout->addWidget(new QLabel(tr("minutes"), this));
    fetchLayout->addStretch();

    mPushCommit = new QCheckBox(tr("Push after each commit"), this);
    mPullUpdate = new QCheckBox(tr("Update submodules after pull"), this);
    mAutoPrune = new QCheckBox(tr("Prune when fetching"), this);
    mNoTranslation = new QCheckBox(tr("No translation"), this);

    mStoreCredentials = new QCheckBox(
      tr("Store credentials in secure storage"), this);

    mUsageReporting = new QCheckBox(
      tr("Allow collection of usage data"), this);
    QLabel *privacy = new QLabel(tr("<a href='view'>View privacy policy</a>"));
    connect(privacy, &QLabel::linkActivated, [] {
      AboutDialog::openSharedInstance(AboutDialog::Privacy);
    });

    QFormLayout *form = new QFormLayout;
    form->addRow(tr("User name:"), mName);
    form->addRow(tr("User email:"), mEmail);
    form->addRow(tr("Automatic actions:"), fetchLayout);
    form->addRow(QString(), mPushCommit);
    form->addRow(QString(), mPullUpdate);
    form->addRow(QString(), mAutoPrune);
    form->addRow(tr("Language:"), mNoTranslation);
    form->addRow(tr("Credentials:"), mStoreCredentials);
    form->addRow(tr("Usage reporting:"), mUsageReporting);
    form->addRow(QString(), privacy);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16,12,16,12);
    layout->addLayout(form);

    init();

    // Connect signals after initializing fields.
    connect(mName, &QLineEdit::textChanged, [](const QString &text) {
      git::Config::global().setValue("user.name", text);
    });

    connect(mEmail, &QLineEdit::textChanged, [](const QString &text) {
      git::Config::global().setValue("user.email", text);
    });

    connect(mFetch, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("global/autofetch/enable", checked);
      foreach (MainWindow *window, MainWindow::windows()) {
        for (int i = 0; i < window->count(); ++i)
          window->view(i)->startFetchTimer();
      }
    });

    auto signal = QOverload<int>::of(&QSpinBox::valueChanged);
    connect(mFetchMinutes, signal, [](int value) {
      Settings::instance()->setValue("global/autofetch/minutes", value);
    });

    connect(mPushCommit, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("global/autopush/enable", checked);
    });

    connect(mPullUpdate, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("global/autoupdate/enable", checked);
    });

    connect(mAutoPrune, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("global/autoprune/enable", checked);
    });

    connect(mNoTranslation, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("translation/disable", checked);
    });

    connect(mStoreCredentials, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("credential/store", checked);
      delete CredentialHelper::instance();
    });

    connect(mUsageReporting, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("tracking/enabled", checked);
    });
  }

  void init()
  {
    git::Config config = git::Config::global();
    mName->setText(config.value<QString>("user.name"));
    mEmail->setText(config.value<QString>("user.email"));

    Settings *settings = Settings::instance();
    settings->beginGroup("global");
    settings->beginGroup("autofetch");
    mFetch->setChecked(settings->value("enable").toBool());
    mFetchMinutes->setValue(settings->value("minutes").toInt());
    settings->endGroup();

    mPushCommit->setChecked(settings->value("autopush/enable").toBool());
    mPullUpdate->setChecked(settings->value("autoupdate/enable").toBool());
    mAutoPrune->setChecked(settings->value("autoprune/enable").toBool());
    settings->endGroup();

    mNoTranslation->setChecked(settings->value("translation/disable").toBool());
    mStoreCredentials->setChecked(settings->value("credential/store").toBool());
    mUsageReporting->setChecked(settings->value("tracking/enabled").toBool());
  }

private:
  QLineEdit *mName;
  QLineEdit *mEmail;

  QCheckBox *mFetch;
  QSpinBox *mFetchMinutes;
  QCheckBox *mPushCommit;
  QCheckBox *mPullUpdate;
  QCheckBox *mAutoPrune;
  QCheckBox *mNoTranslation;
  QCheckBox *mStoreCredentials;
  QCheckBox *mUsageReporting;
};

class ToolsPanel : public QWidget
{
  Q_OBJECT

public:
  ToolsPanel(QWidget *parent = nullptr)
    : QWidget(parent), mConfig(git::Config::global())
  {
    // External editor.
    mEditTool = new QLineEdit(this);
    connect(mEditTool, &QLineEdit::textChanged, [this](const QString &text) {
      if (text.isEmpty()) {
        mConfig.remove("gui.editor");
      } else {
        mConfig.setValue("gui.editor", text);
      }
    });

    // External diff/merge.
    mDiffTool = externalTools("diff");
    mMergeTool = externalTools("merge");

    // Backup files.
    mBackup = new QCheckBox(tr("Keep backup of merge files (.orig)"), this);
    connect(mBackup, &QCheckBox::toggled, [this](bool checked) {
      mConfig.setValue("mergetool.keepBackup", checked);
    });

    init();

    QFormLayout *layout = new QFormLayout(this);
    layout->addRow(tr("External editor:"), mEditTool);
    layout->addRow(tr("External diff:"), mDiffTool);
    layout->addRow(tr("External merge:"), mMergeTool);
    layout->addRow(tr("Backup files:"), mBackup);
  }

  void init(void)
  {
    QComboBox *comboBox;
    QString key;
    QString name;

    mEditTool->setText(mConfig.value<QString>("gui.editor"));

    comboBox = static_cast<QComboBox *>(mDiffTool->itemAt(0)->widget());
    key = QString("%1.tool").arg("diff");
    name = mConfig.value<QString>(key);
    comboBox->setCurrentIndex(comboBox->findText(name));

    comboBox = static_cast<QComboBox *>(mMergeTool->itemAt(0)->widget());
    key = QString("%1.tool").arg("merge");
    name = mConfig.value<QString>(key);
    comboBox->setCurrentIndex(comboBox->findText(name));

    mBackup->setChecked(mConfig.value<bool>("mergetool.keepBackup"));
  }

private:
  QHBoxLayout *externalTools(const QString &type)
  {
    // Fill combo box with git config entries.
    QComboBox *comboBox = new QComboBox(this);
    populateExternalTools(comboBox, type);

    // Read tool from git config.
    QString key = QString("%1.tool").arg(type);

    // React to combo box selections.
    auto signal = QOverload<int>::of(&QComboBox::currentIndexChanged);
    connect(comboBox, signal, [this, key, comboBox](int index) {
      mConfig.setValue(key, comboBox->currentText());
    });

    QPushButton *configure = new QPushButton(tr("Configure"), this);
    connect(configure, &QPushButton::clicked, [this, comboBox, type] {
      ExternalToolsDialog *dialog = new ExternalToolsDialog(type, this);

      // Update combo box when external tools dialog closes.
      connect(dialog, &QDialog::finished, [dialog, comboBox, type] {
        QString name = comboBox->currentText();
        populateExternalTools(comboBox, type);
        comboBox->setCurrentIndex(comboBox->findText(name));
      });

      dialog->open();
    });

    QHBoxLayout *layout = new QHBoxLayout;
    layout->addWidget(comboBox, 1);
    layout->addWidget(configure);

    return layout;
  }

  git::Config mConfig;

  QLineEdit *mEditTool;
  QHBoxLayout *mDiffTool;
  QHBoxLayout *mMergeTool;
  QCheckBox *mBackup;
};

class WindowPanel : public QWidget
{
  Q_OBJECT

public:
  WindowPanel(QWidget *parent = nullptr)
    : QWidget(parent)
  {
    mComboBox = new QComboBox(this);

    // Default theme.
    mComboBox->addItem("Default");

    // Predefined themes.
    QDir dir = Settings::themesDir();
    dir.setNameFilters({"*.lua"});
    QDirIterator *it = new QDirIterator(dir);
    while (it->hasNext()) {
      it->next();
      QString name = it->fileInfo().baseName();
      if (name != "Default")
        mComboBox->addItem(name);
    }

    // User themes.
    bool exists = false;
    QDir appLocalDir = CustomTheme::userDir(false, &exists);
    if (exists) {
      appLocalDir.setNameFilters({"*.lua"});
      QDirIterator *it = new QDirIterator(appLocalDir);

      if (it->hasNext())
        mComboBox->insertSeparator(mComboBox->count());

      while (it->hasNext()) {
        it->next();
        mComboBox->addItem(it->fileInfo().baseName(), it->filePath());
      }
    }

    mComboBox->insertSeparator(mComboBox->count());

    Settings *settings = Settings::instance();
    settings->beginGroup("window");
    int index = mComboBox->findText(settings->value("theme").toString());
    settings->endGroup(); // window

    // Add theme.
    mComboBox->addItem(tr("Add New Theme"));
    mComboBox->addItem(tr("Edit Current Theme"), index);
    mComboBox->setCurrentIndex(index >= 0 ? index : 0);

    // Edit enabled for user themes.
    QStandardItemModel *model =
      static_cast<QStandardItemModel *>(mComboBox->model());
    if(!mComboBox->itemData(mComboBox->currentIndex()).isValid())
      model->item(mComboBox->count() - 1)->setEnabled(false);

    auto signal = QOverload<int>::of(&QComboBox::currentIndexChanged);
    connect(mComboBox, signal, [this, parent] {

      //Add new theme.
      if (mComboBox->currentIndex() == mComboBox->count() - 2) {
        QDialog dialog;

        QDialogButtonBox *buttons =
          new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        QPushButton *create =
          buttons->addButton(tr("Create Theme"), QDialogButtonBox::AcceptRole);
        create->setEnabled(false);

        QLineEdit *nameField = new QLineEdit(&dialog);
        connect(nameField, &QLineEdit::textChanged, [this, create, nameField] {
          create->setEnabled(!nameField->text().isEmpty());
        });

        QFormLayout *layout = new QFormLayout(&dialog);
        layout->addRow(tr("Theme Name"), nameField);
        layout->addRow(buttons);

        if (dialog.exec()) {
          QDir dir = CustomTheme::userDir(true);
          QString path = dir.filePath(QString("%1.lua").arg(nameField->text()));
          QFile::copy(Settings::themesDir().filePath("Dark.lua"), path);
          EditorWindow::open(path);
        }

        window()->close();
        return;
      }

      // Edit current theme.
      QStandardItemModel *model =
        static_cast<QStandardItemModel *>(mComboBox->model());
      bool enabled = mComboBox->itemData(mComboBox->currentIndex()).isValid();
      model->item(mComboBox->count() - 1)->setEnabled(enabled);

      if (mComboBox->currentIndex() == mComboBox->count() - 1) {
        int index = mComboBox->currentData().toInt();
        QString path = mComboBox->itemData(index).toString();
        EditorWindow::open(path);
        parent->close();
        return;
      }

      // Save theme.
      Settings::instance()->setValue("window/theme", mComboBox->currentText());

      QMessageBox mb(QMessageBox::Information, tr("Restart?"),
        tr("The application must be restarted for "
           "the theme change to take effect."));
      mb.setInformativeText(tr("Do you want to restart now?"));
      QPushButton *restart =
        mb.addButton(tr("Restart"), QMessageBox::AcceptRole);
      mb.addButton(tr("Later"), QMessageBox::RejectRole);
      mb.setDefaultButton(restart);
      mb.exec();

      if (mb.clickedButton() == restart) {
        QWidget *dialog = window();
        QTimer::singleShot(0, [dialog] {
          // Close the dialog.
          dialog->close();

          // Restart the app.
          QStringList args = qApp->arguments();
          QProcess::startDetached(args.takeFirst(), args);
          qApp->quit();
        });
      }
    });

    mFullPath = new QCheckBox(tr("Show full repository path"));
    connect(mFullPath, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("window/path/full", checked);
    });

    mHideLog = new QCheckBox(tr("Hide automatically"));
    connect(mHideLog, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("window/log/hide", checked);
    });

    mSubTabs = new QCheckBox(tr("Open submodules in tabs"));
    connect(mSubTabs, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("window/tabs/submodule", checked);
    });

    mRepoTabs = new QCheckBox(tr("Open all repositories in tabs"));
    connect(mRepoTabs, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("window/tabs/repository", checked);
    });

    mMerge = new QCheckBox(this);
    connect(mMerge, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setPrompt(Settings::PromptMerge, checked);
    });

    mRevert = new QCheckBox(this);
    connect(mRevert, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setPrompt(Settings::PromptRevert, checked);
    });

    mCherryPick = new QCheckBox(this);
    connect(mCherryPick, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setPrompt(Settings::PromptCherryPick, checked);
    });

    mStash = new QCheckBox(this);
    connect(mStash, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setPrompt(Settings::PromptStash, checked);
    });

    mLargeFiles = new QCheckBox(this);
    connect(mLargeFiles, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setPrompt(Settings::PromptLargeFiles, checked);
    });

    mDirectories = new QCheckBox(this);
    connect(mDirectories, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setPrompt(Settings::PromptDirectories, checked);
    });

    init();

    QFormLayout *layout = new QFormLayout(this);
    layout->addRow(tr("Theme:"), mComboBox);
    layout->addRow(tr("Title:"), mFullPath);
    layout->addRow(tr("Log:"), mHideLog);
    layout->addRow(tr("Tabs:"), mSubTabs);
    layout->addRow(QString(), mRepoTabs);
    layout->addRow(tr("Prompt:"), mMerge);
    layout->addRow(QString(), mRevert);
    layout->addRow(QString(), mCherryPick);
    layout->addRow(QString(), mStash);
    layout->addRow(QString(), mDirectories);
    layout->addRow(QString(), mLargeFiles);
  }

  void init(void)
  {
    Settings *settings = Settings::instance();
    settings->beginGroup("window");

    int index = mComboBox->findText(settings->value("theme").toString());
    mComboBox->setCurrentIndex(index >= 0 ? index : 0);

    mFullPath->setChecked(settings->value("path/full").toBool());
    mHideLog->setChecked(settings->value("log/hide").toBool());
    settings->beginGroup("tabs");
    mSubTabs->setChecked(settings->value("submodule").toBool());
    mRepoTabs->setChecked(settings->value("repository").toBool());
    settings->endGroup(); // tabs
    settings->endGroup(); // window

    QString mergeText = settings->promptDescription(Settings::PromptMerge);
    mMerge->setText(mergeText);
    mMerge->setChecked(settings->prompt(Settings::PromptMerge));

    QString revertText = settings->promptDescription(Settings::PromptRevert);
    mRevert->setText(revertText);
    mRevert->setChecked(settings->prompt(Settings::PromptRevert));

    QString cpText = settings->promptDescription(Settings::PromptCherryPick);
    mCherryPick->setText(cpText);
    mCherryPick->setChecked(settings->prompt(Settings::PromptCherryPick));

    QString stashText = settings->promptDescription(Settings::PromptStash);
    mStash->setText(stashText);
    mStash->setChecked(settings->prompt(Settings::PromptStash));

    QString largeFilesText = settings->promptDescription(Settings::PromptLargeFiles);
    mLargeFiles->setText(largeFilesText);
    mLargeFiles->setChecked(settings->prompt(Settings::PromptLargeFiles));

    QString directoriesText = settings->promptDescription(Settings::PromptDirectories);
    mDirectories->setText(directoriesText);
    mDirectories->setChecked(settings->prompt(Settings::PromptDirectories));
  }

private:
  QComboBox *mComboBox;
  QCheckBox *mFullPath;
  QCheckBox *mHideLog;
  QCheckBox *mSubTabs;
  QCheckBox *mRepoTabs;
  QCheckBox *mMerge;
  QCheckBox *mRevert;
  QCheckBox *mCherryPick;
  QCheckBox *mStash;
  QCheckBox *mLargeFiles;
  QCheckBox *mDirectories;
};

class EditorPanel : public QWidget
{
  Q_OBJECT

public:
  EditorPanel(QWidget *parent = nullptr)
    : QWidget(parent)
  {
    auto spin = QOverload<int>::of(&QSpinBox::valueChanged);
    auto combo = QOverload<int>::of(&QComboBox::currentIndexChanged);

    mFont = new QFontComboBox(this);
    mFont->setEditable(false);
    mFont->setFontFilters(QFontComboBox::MonospacedFonts);
    connect(mFont, &QFontComboBox::currentTextChanged, [](const QString &text) {
      Settings::instance()->setValue("editor/font/family", text);
    });

    mFontSize = new QSpinBox(this);
    mFontSize->setRange(2, 32);
    connect(mFontSize, spin, [](int i) {
      Settings::instance()->setValue("editor/font/size", i);
    });

    mIndent = new QComboBox(this);
    mIndent->addItem(tr("Tabs"));
    mIndent->addItem(tr("Spaces"));
    connect(mIndent, combo, [](int i) {
      Settings::instance()->setValue("editor/indent/tabs", i == 0);
    });

    mIndentWidth = new QSpinBox(this);
    mIndentWidth->setRange(1, 32);
    connect(mIndentWidth, spin, [](int i) {
      Settings::instance()->setValue("editor/indent/width", i);
    });

    mTabWidth = new QSpinBox(this);
    mTabWidth->setRange(1, 32);
    connect(mTabWidth, spin, [](int i) {
      Settings::instance()->setValue("editor/indent/tabwidth", i);
    });

    mBlameHeatMap = new QCheckBox(tr("Show heat map"), this);
    connect(mBlameHeatMap, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("editor/blame/heatmap", checked);
    });

    init();

    QFormLayout *layout = new QFormLayout(this);
    layout->addRow(tr("Font:"), mFont);
    layout->addRow(tr("Font size:"), mFontSize);
    layout->addRow(tr("Indent using:"), mIndent);
    layout->addRow(tr("Indent width:"), mIndentWidth);
    layout->addRow(tr("Tab width:"), mTabWidth);
    layout->addRow(tr("Blame margin:"), mBlameHeatMap);
  }

  void init(void)
  {
    Settings *settings = Settings::instance();
    settings->beginGroup("editor");

    settings->beginGroup("font");
    mFont->setCurrentText(settings->value("family").toString());
    mFontSize->setValue(settings->value("size").toInt());
    settings->endGroup(); // font

    settings->beginGroup("indent");
    mIndent->setCurrentIndex(settings->value("tabs").toBool() ? 0 : 1);
    mIndentWidth->setValue(settings->value("width").toInt());
    mTabWidth->setValue(settings->value("tabwidth").toInt());
    settings->endGroup(); // indent

    mBlameHeatMap->setChecked(settings->value("blame/heatmap").toBool());
    settings->endGroup(); // editor
  }

private:
  QFontComboBox *mFont;
  QSpinBox *mFontSize;
  QComboBox *mIndent;
  QSpinBox *mIndentWidth;
  QSpinBox *mTabWidth;
  QCheckBox *mBlameHeatMap;
};

class UpdatePanel : public QWidget
{
  Q_OBJECT

public:
  UpdatePanel(QWidget *parent = nullptr)
    : QWidget(parent)
  {
    QString checkText = tr("Check for updates automatically");
    mCheck = new QCheckBox(checkText, this);
    connect(mCheck, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("update/check", checked);
    });

    QString downloadText = tr("Automatically download and install updates");
    mDownload = new QCheckBox(downloadText, this);
    connect(mDownload, &QCheckBox::toggled, [](bool checked) {
      Settings::instance()->setValue("update/download", checked);
    });

    QPushButton *button = new QPushButton(tr("Check Now"), this);
    connect(button, &QPushButton::clicked,
            Updater::instance(), &Updater::update);

    init();

    QFormLayout *layout = new QFormLayout(this);
    layout->addRow(tr("Software Update:"), mCheck);
    layout->addRow(QString(), mDownload);
    layout->addRow(QString(), button);
  }

  void init(void)
  {
    Settings *settings = Settings::instance();
    settings->beginGroup("update");

    mCheck->setChecked(settings->value("check").toBool());
    mDownload->setChecked(settings->value("download").toBool());

    settings->endGroup(); // update
  }

private:
  QCheckBox *mCheck;
  QCheckBox *mDownload;
};

class MiscPanel : public QWidget
{
  Q_OBJECT

public:
  MiscPanel(QWidget *parent = nullptr)
    : QWidget(parent)
  {


    mSshConfigPathBox = new QLineEdit(this);
    connect(mSshConfigPathBox, &QLineEdit::textChanged, [](const QString &text) {
      Settings::instance()->setValue("ssh/configFilePath", text);
    });

    mSshKeyPathBox = new QLineEdit(this);
    connect(mSshKeyPathBox, &QLineEdit::textChanged, [](const QString &text) {
      Settings::instance()->setValue("ssh/keyFilePath", text);
    });

    init();

    QFormLayout *layout = new QFormLayout(this);
    layout->addRow(tr("Path to SSH config file:"), mSshConfigPathBox);
    layout->addRow(tr("Path to default / fallback SSH key file:"), mSshKeyPathBox);
  }

  void init(void)
  {
    Settings *settings = Settings::instance();
    settings->beginGroup("ssh");
    mSshConfigPathBox->setText(settings->value("configFilePath").toString());
    mSshKeyPathBox->setText(settings->value("keyFilePath").toString());
    settings->endGroup(); // ssh
  }

private:
  QLineEdit *mSshConfigPathBox;
  QLineEdit *mSshKeyPathBox;
};

#ifdef Q_OS_UNIX
class TerminalPanel : public QWidget
{
  Q_OBJECT

public:
  TerminalPanel(QWidget *parent = nullptr)
    : QWidget(parent)
  {
    Settings *settings = Settings::instance();
    settings->beginGroup("terminal");

    mNameBox = new QLineEdit(settings->value("name").toString(), this);
    connect(mNameBox, &QLineEdit::textChanged, [this](const QString &text) {
      Settings::instance()->setValue("terminal/name", text);
      updateInstallButton();
    });

    mPathBox = new QLineEdit(settings->value("path").toString(), this);
    connect(mPathBox, &QLineEdit::textChanged, [this](const QString &text) {
      Settings::instance()->setValue("terminal/path", text);
      updateInstallButton();
    });

    settings->endGroup();

    mInstallButton = new QPushButton(tr("Install"), this);
    connect(mInstallButton, &QPushButton::clicked, [this] {
      Installer installer(mNameBox->text(), mPathBox->text());
      if (installer.isInstalled()) {
        installer.uninstall();
      } else if (!installer.exists()) {
        installer.install();
      }

      updateInstallButton();
    });

    QFormLayout *layout = new QFormLayout(this);
    layout->addRow(tr("Name:"), mNameBox);
    layout->addRow(tr("Location:"), mPathBox);
    layout->addRow(QString(), mInstallButton);

    updateInstallButton();
  }

private:
  void updateInstallButton()
  {
    Installer installer(mNameBox->text(), mPathBox->text());
    bool installed = installer.isInstalled();
    mInstallButton->setText(!installed ? tr("Install") : tr("Uninstall"));
    mInstallButton->setEnabled(installed || !installer.exists());
  }

  QLineEdit *mNameBox;
  QLineEdit *mPathBox;
  QPushButton *mInstallButton;
};
#endif

} // anon. namespace

SettingsDialog::SettingsDialog(Index index, QWidget *parent)
  : QMainWindow(parent, Qt::Dialog)
{
  setMinimumWidth(500);
  setAttribute(Qt::WA_DeleteOnClose);
  setUnifiedTitleAndToolBarOnMac(true);
  setContextMenuPolicy(Qt::NoContextMenu);

  // Close on escape.
  QShortcut *esc = new QShortcut(tr("Esc"), this);
  connect(esc, &QShortcut::activated, this, &SettingsDialog::close);

  // Create tool bar.
  QToolBar *toolbar = new QToolBar(this);
  toolbar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  toolbar->setMovable(false);
  addToolBar(toolbar);

  // Create central stack widget.
  StackedWidget *stack = new StackedWidget(this);
  connect(stack, &StackedWidget::currentChanged,
          this, &SettingsDialog::adjustSize);

  QString text =
    tr("Global git and GitAhead settings can be overridden for each repository in "
       "the corresponding repository configuration page.");
  QLabel *description = new QLabel(text, this);
  description->setStyleSheet("QLabel { padding: 0px 20px 0px 20px }");
  description->setWordWrap(true);

#ifndef Q_OS_WIN
  QFont small = font();
  small.setPointSize(small.pointSize() - 2);
  description->setFont(small);
#endif

  QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::close);
  connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::close);

  // Add git config edit button.
  QPushButton *edit =
    buttons->addButton(tr("Edit git Config File..."), QDialogButtonBox::ResetRole);

  // Add app config edit button.
  QPushButton *appEdit =
    buttons->addButton(tr("Edit GitAhead Config File"), QDialogButtonBox::ResetRole);

  // Add app settings edit button.
  QPushButton *appSettings =
    buttons->addButton(tr("Edit GitAhead Settings"), QDialogButtonBox::ResetRole);

  QWidget *widget = new QWidget(this);
  QVBoxLayout *layout = new QVBoxLayout(widget);
  layout->addWidget(stack);
  layout->addWidget(description);

#ifdef Q_OS_WIN
  layout->addSpacing(16);
#endif

  layout->addWidget(buttons);

  setCentralWidget(widget);

  // Track actions in a group.
  QActionGroup *actions = new QActionGroup(this);
  connect(actions, &QActionGroup::triggered,
  [this, stack, description, edit, appEdit, appSettings](QAction *action) {
    int index = action->data().toInt();
    bool gitconfig = (index < Window);
    bool gitaheadconfig = (index == General || index == Diff || index == Plugins);
    bool gitaheadsetting = (index  == General || index == Diff || index > Tools);

    description->setVisible(gitconfig || gitaheadconfig);
    edit->setVisible(gitconfig);
    appEdit->setVisible(gitaheadconfig);
    appSettings->setVisible(gitaheadsetting);
    stack->setCurrentIndex(index);
    setWindowTitle(action->text());
  });

  // Add global git settings panel.
  QAction *general = toolbar->addAction(QIcon(":/general.png"), tr("General"));
  general->setData(General);
  general->setActionGroup(actions);
  general->setCheckable(true);

  GeneralPanel *generalPanel = new GeneralPanel(this);
  stack->addWidget(generalPanel);

  // Add diff panel.
  QAction *diff = toolbar->addAction(QIcon(":/diff.png"), tr("Diff"));
  diff->setData(Diff);
  diff->setActionGroup(actions);
  diff->setCheckable(true);

  DiffPanel *diffPanel = new DiffPanel(git::Repository(), this);
  stack->addWidget(diffPanel);

  // Add tools panel.
  QAction *tools = toolbar->addAction(QIcon(":/tools.png"), tr("Tools"));
  tools->setData(Tools);
  tools->setActionGroup(actions);
  tools->setCheckable(true);

  ToolsPanel *toolsPanel = new ToolsPanel(this);
  stack->addWidget(toolsPanel);

  toolbar->addSeparator();

  // Add window panel.
  QAction *window = toolbar->addAction(QIcon(":/window.png"), tr("Window"));
  window->setData(Window);
  window->setActionGroup(actions);
  window->setCheckable(true);

  WindowPanel *windowPanel = new WindowPanel(this);
  stack->addWidget(windowPanel);

  // Add editor panel.
  QAction *editor = toolbar->addAction(QIcon(":/editor.png"), tr("Editor"));
  editor->setData(Editor);
  editor->setActionGroup(actions);
  editor->setCheckable(true);

  EditorPanel *editorPanel = new EditorPanel(this);
  stack->addWidget(editorPanel);

  // Add update panel.
  QAction *update = toolbar->addAction(QIcon(":/update.png"), tr("Update"));
  update->setData(Update);
  update->setActionGroup(actions);
  update->setCheckable(true);

  UpdatePanel *updatePanel = new UpdatePanel(this);
  stack->addWidget(updatePanel);

  // Add plugins panel.
  QAction *plugins = toolbar->addAction(QIcon(":/plugins.png"), tr("Plugins"));
  plugins->setData(Plugins);
  plugins->setActionGroup(actions);
  plugins->setCheckable(true);

  PluginsPanel *pluginsPanel = new PluginsPanel(git::Repository(), this);
  stack->addWidget(pluginsPanel);

  // Add misc panel.
  QAction *misc = toolbar->addAction(QIcon(":/misc.png"), tr("Misc"));
  misc->setData(Misc);
  misc->setActionGroup(actions);
  misc->setCheckable(true);

  MiscPanel *miscPanel = new MiscPanel(this);
  stack->addWidget(miscPanel);

#ifdef Q_OS_UNIX
  // Add terminal panel.
  QAction *terminal = toolbar->addAction(QIcon(":/terminal.png"), tr("Terminal"));
  terminal->setData(Terminal);
  terminal->setActionGroup(actions);
  terminal->setCheckable(true);

  stack->addWidget(new TerminalPanel(this));
#endif

  // Hook up app config edit button.
  connect(appEdit, &QPushButton::clicked, stack, [generalPanel, diffPanel, pluginsPanel] {
    // Update on save.
    EditorWindow *window = EditorWindow::open(Settings::userDir().path() + "/config");
    connect(window->widget(), &BlameEditor::saved, [generalPanel, diffPanel, pluginsPanel] {
      // GitAhead config changed.
      generalPanel->init();
      diffPanel->init();
      pluginsPanel->init();
    });
  });

  // Hook up app settings edit button.
  connect(appSettings, &QPushButton::clicked, stack, [generalPanel, diffPanel, windowPanel, editorPanel, updatePanel, miscPanel] {
    // Update on save.
    QSettings settings;
    EditorWindow *window = EditorWindow::open(settings.fileName());
    connect(window->widget(), &BlameEditor::saved, [generalPanel, diffPanel, windowPanel, editorPanel, updatePanel, miscPanel] {
      // Gitahead settings changed.
      generalPanel->init();
      diffPanel->init();
      windowPanel->init();
      editorPanel->init();
      updatePanel->init();
      miscPanel->init();
    });
  });

  // Hook up git config edit button.
  connect(edit, &QPushButton::clicked, stack, [generalPanel, diffPanel, toolsPanel] {
    // Update on save.
    EditorWindow *window = EditorWindow::open(git::Config::globalPath());
    connect(window->widget(), &BlameEditor::saved, [generalPanel, diffPanel, toolsPanel] {
      // GitAhead Config changed.
      generalPanel->init();
      diffPanel->init();
      toolsPanel->init();
    });
  });

  // Select the requested index.
  actions->actions().at(index)->trigger();
}

void SettingsDialog::openSharedInstance(Index index)
{
  Application::track("SettingsDialog");

  static QPointer<SettingsDialog> dialog;
  if (dialog) {
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    return;
  }

  dialog = new SettingsDialog(index);
  dialog->show();
}

#include "SettingsDialog.moc"
