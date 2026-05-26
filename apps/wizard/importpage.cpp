#include "importpage.hpp"

#include <QFile>

#include "mainwizard.hpp"

Wizard::ImportPage::ImportPage(QWidget* parent)
    : QWizardPage(parent)
{
    mWizard = qobject_cast<MainWizard*>(parent);

    setupUi(this);

    mDefaultInfoText = infoLabel->text();

    registerField(QLatin1String("installation.import-settings"), importCheckBox);
    registerField(QLatin1String("installation.import-addons"), addonsCheckBox);
    registerField(QLatin1String("installation.import-fonts"), fontsCheckBox);
}

void Wizard::ImportPage::initializePage()
{
    bool hasIni = field(QLatin1String("installation.retailDisc")).toBool();
    if (!hasIni)
    {
        const QString path = field(QLatin1String("installation.path")).toString();
        if (mWizard->mInstallations.contains(path))
            hasIni = QFile::exists(mWizard->mInstallations[path].iniPath);
    }

    importCheckBox->setEnabled(hasIni);
    addonsCheckBox->setEnabled(hasIni);
    fontsCheckBox->setEnabled(hasIni);

    if (hasIni)
    {
        infoLabel->setText(mDefaultInfoText);
        return;
    }

    importCheckBox->setChecked(false);
    addonsCheckBox->setChecked(false);
    fontsCheckBox->setChecked(false);
    infoLabel->setText(
        tr("<html><head/><body><p><b>Morrowind.ini was not found.</b></p>"
           "<p>The selected Data Files directory will still be written to OpenMW's configuration. "
           "Settings, plugin selection, and bitmap font import will be skipped.</p></body></html>"));
}

int Wizard::ImportPage::nextId() const
{
    return MainWizard::Page_Conclusion;
}
