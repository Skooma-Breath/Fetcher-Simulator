#include "existinginstallationpage.hpp"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>

#include <components/misc/scalableicon.hpp>

#include "mainwizard.hpp"

namespace
{
    bool versionIsOK(QWidget* parent, const QString& directoryName)
    {
        const QFileInfoList infoList = QDir(directoryName).entryInfoList({ "Morrowind.bsa" });
        if (infoList.size() != 1)
            return false;

        const qint64 actualFileSize = infoList.at(0).size();

        // Size of Morrowind.bsa in Steam and GOG editions.
        constexpr qint64 expectedFileSize = 310459500;
        if (actualFileSize == expectedFileSize)
            return true;

        QMessageBox msgBox(parent);
        msgBox.setWindowTitle(QObject::tr("Most recent Morrowind not detected"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        msgBox.setDefaultButton(QMessageBox::No);
        msgBox.setText(
            QObject::tr("<br><b>There may be a more recent version of Morrowind available.</b><br><br>"
                        "Do you wish to continue anyway?<br>"));
        return msgBox.exec() == QMessageBox::Yes;
    }
}
Wizard::ExistingInstallationPage::ExistingInstallationPage(QWidget* parent)
    : QWizardPage(parent)
{
    mWizard = qobject_cast<MainWizard*>(parent);

    setupUi(this);

    browseButton->setIcon(Misc::ScalableIcon::load(":folder"));
    connect(browseButton, &QPushButton::clicked, this, &ExistingInstallationPage::browseButtonClicked);

    // Add a placeholder item to the list of installations
    QListWidgetItem* emptyItem = new QListWidgetItem(tr("No existing installations detected"), installationsList);
    emptyItem->setFlags(Qt::NoItemFlags);

    connect(installationsList, &QListWidget::currentTextChanged, this, &ExistingInstallationPage::textChanged);
    connect(installationsList, &QListWidget::itemSelectionChanged, this, &ExistingInstallationPage::completeChanged);
}

void Wizard::ExistingInstallationPage::initializePage()
{
    // Add the available installation paths
    const QStringList paths(mWizard->mInstallations.keys());

    // Hide the default item if there are installations to choose from
    installationsList->item(0)->setHidden(!paths.isEmpty());

    for (const QString& path : paths)
    {
        if (installationsList->findItems(path, Qt::MatchExactly).isEmpty())
        {
            installationsList->addItem(path);
        }
    }
}

bool Wizard::ExistingInstallationPage::validatePage()
{
    QString path(field(QStringLiteral("installation.path")).toString());
    if (path.isEmpty() && installationsList->currentItem() != nullptr)
    {
        path = installationsList->currentItem()->text();
        mWizard->setField(QStringLiteral("installation.path"), path);
    }

    if (path.isEmpty() || !mWizard->mInstallations.contains(path))
        return false;

    if (!QDir(path).exists() || !mWizard->findFiles(QStringLiteral("Morrowind"), path))
    {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Invalid Morrowind installation"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.setText(
            tr("<br><b>The selected Data Files directory is not usable.</b><br><br>"
               "Press \"Browse...\" and select Morrowind.esm inside the Data Files directory for "
               "the Morrowind installation on this computer.<br>"));
        msgBox.exec();
        return false;
    }

    QString& iniPath = mWizard->mInstallations[path].iniPath;
    if (iniPath.isEmpty())
    {
        QDir dir(path);
        dir.cdUp();
        const QString candidate = dir.filePath(QStringLiteral("Morrowind.ini"));
        if (QFile::exists(candidate))
            iniPath = candidate;
    }

    if (QFile::exists(iniPath))
        return true;

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Error detecting Morrowind configuration"));
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setStandardButtons(QMessageBox::Cancel);
    msgBox.setText(
        tr("<br><b>Could not find Morrowind.ini</b><br><br>"
           "The Wizard can continue using the selected Data Files directory, but settings "
           "import from Morrowind.ini will be skipped.<br><br>"
           "Press \"Browse...\" to specify the location manually, or continue without importing.<br>"));

    QAbstractButton* browseIniButton = msgBox.addButton(tr("B&rowse..."), QMessageBox::ActionRole);
    QAbstractButton* continueButton
        = msgBox.addButton(tr("&Continue without importing"), QMessageBox::AcceptRole);
    msgBox.exec();

    if (msgBox.clickedButton() == continueButton)
    {
        mWizard->setField(QStringLiteral("installation.import-settings"), false);
        mWizard->setField(QStringLiteral("installation.import-addons"), false);
        mWizard->setField(QStringLiteral("installation.import-fonts"), false);
        return true;
    }
    if (msgBox.clickedButton() != browseIniButton)
        return false;

    QDir dir(path);
    dir.cdUp();
    const QString iniFile = QFileDialog::getOpenFileName(this, tr("Select Morrowind.ini"),
        dir.exists() ? dir.absolutePath() : QDir::currentPath(),
        tr("Morrowind.ini (Morrowind.ini);;INI files (*.ini *.INI);;All files (*)"), nullptr,
        QFileDialog::DontResolveSymlinks);
    if (iniFile.isEmpty())
        return false;

    const QFileInfo info(iniFile);
    if (info.fileName().compare(QStringLiteral("Morrowind.ini"), Qt::CaseInsensitive) != 0)
    {
        QMessageBox invalid(this);
        invalid.setWindowTitle(tr("Invalid configuration file"));
        invalid.setIcon(QMessageBox::Warning);
        invalid.setStandardButtons(QMessageBox::Ok);
        invalid.setText(tr("Please select Morrowind.ini."));
        invalid.exec();
        return false;
    }

    iniPath = info.absoluteFilePath();
    return true;
}

void Wizard::ExistingInstallationPage::browseButtonClicked()
{
    const QString selectedFile = QFileDialog::getOpenFileName(this, tr("Select Morrowind.esm (located in Data Files)"),
        QDir::currentPath(), tr("Morrowind master file (Morrowind.esm)"), nullptr, QFileDialog::DontResolveSymlinks);

    if (selectedFile.isEmpty())
        return;

    const QString path(QDir::toNativeSeparators(QFileInfo(selectedFile).absolutePath()));
    if (!MainWizard::findFiles(QStringLiteral("Morrowind"), path))
    {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Error detecting Morrowind files"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.setText(
            tr("<b>Morrowind.bsa</b> is missing!<br>"
               "Make sure your Morrowind installation is complete."));
        msgBox.exec();
        return;
    }

    if (!versionIsOK(this, path))
        return;

    const QList<QListWidgetItem*> items = installationsList->findItems(path, Qt::MatchExactly);

    if (items.isEmpty())
    {
        // Path is not yet in the list, add it
        mWizard->addInstallation(path);

        // Hide the default item
        installationsList->item(0)->setHidden(true);

        QListWidgetItem* item = new QListWidgetItem(path, installationsList);
        installationsList->setCurrentItem(item); // Select it too
    }
    else
    {
        installationsList->setCurrentItem(items.first());
    }

    // Update the button
    emit completeChanged();
}

void Wizard::ExistingInstallationPage::textChanged(const QString& text)
{
    // Set the installation path manually, as registerField doesn't work
    // Because it doesn't accept two widgets operating on a single field
    if (!text.isEmpty())
        mWizard->setField(QStringLiteral("installation.path"), text);
}

bool Wizard::ExistingInstallationPage::isComplete() const
{
    return installationsList->selectionModel()->hasSelection();
}

int Wizard::ExistingInstallationPage::nextId() const
{
    return MainWizard::Page_LanguageSelection;
}
