/* $Id$ */
/** @file
 *
 * VBox frontends: Qt4 GUI ("VirtualBox"):
 * UIWizardNewVMPageBasic2 class implementation
 */

/*
 * Copyright (C) 2006-2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* Qt includes: */
#include <QIntValidator>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QSpacerItem>
#include <QLabel>

/* GUI includes: */
#include "UIWizardNewVMPageBasic2.h"
#include "UIWizardNewVM.h"
#include "VBoxGlobal.h"
#include "VBoxGuestRAMSlider.h"
#include "QILineEdit.h"
#include "QIRichTextLabel.h"

UIWizardNewVMPage2::UIWizardNewVMPage2()
{
}

void UIWizardNewVMPage2::onRamSliderValueChanged(int iValue)
{
    /* Update 'ram' field editor connected to slider: */
    m_pRamEditor->blockSignals(true);
    m_pRamEditor->setText(QString::number(iValue));
    m_pRamEditor->blockSignals(false);
}

void UIWizardNewVMPage2::onRamEditorTextChanged(const QString &strText)
{
    /* Update 'ram' field slider connected to editor: */
    m_pRamSlider->blockSignals(true);
    m_pRamSlider->setValue(strText.toInt());
    m_pRamSlider->blockSignals(false);
}

UIWizardNewVMPageBasic2::UIWizardNewVMPageBasic2()
{
    /* Create widget: */
    QVBoxLayout *pMainLayout = new QVBoxLayout(this);
    {
        m_pLabel = new QIRichTextLabel(this);
        QGridLayout *pMemoryLayout = new QGridLayout;
        {
            m_pRamSlider = new VBoxGuestRAMSlider(this);
            {
                m_pRamSlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                m_pRamSlider->setOrientation(Qt::Horizontal);
            }
            m_pRamEditor = new QILineEdit(this);
            {
                m_pRamEditor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                m_pRamEditor->setFixedWidthByText("88888");
                m_pRamEditor->setAlignment(Qt::AlignRight);
                m_pRamEditor->setValidator(new QIntValidator(m_pRamSlider->minRAM(), m_pRamSlider->maxRAM(), this));
            }
            m_pRamUnits = new QLabel(this);
            {
                m_pRamUnits->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
            }
            m_pRamMin = new QLabel(this);
            {
                m_pRamMin->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
            }
            m_pRamMax = new QLabel(this);
            {
                m_pRamMax->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
            }
            pMemoryLayout->addWidget(m_pRamSlider, 0, 0, 1, 3);
            pMemoryLayout->addWidget(m_pRamEditor, 0, 3);
            pMemoryLayout->addWidget(m_pRamUnits, 0, 4);
            pMemoryLayout->addWidget(m_pRamMin, 1, 0);
            pMemoryLayout->setColumnStretch(1, 1);
            pMemoryLayout->addWidget(m_pRamMax, 1, 2);
        }
        pMainLayout->addWidget(m_pLabel);
        pMainLayout->addLayout(pMemoryLayout);
        pMainLayout->addStretch();
    }

    /* Setup connections: */
    connect(m_pRamSlider, SIGNAL(valueChanged(int)), this, SLOT(sltRamSliderValueChanged(int)));
    connect(m_pRamEditor, SIGNAL(textChanged(const QString&)), this, SLOT(sltRamEditorTextChanged(const QString&)));

    /* Register fields: */
    registerField("ram", m_pRamSlider, "value", SIGNAL(valueChanged(int)));
}

void UIWizardNewVMPageBasic2::sltRamSliderValueChanged(int iValue)
{
    /* Call to base-class: */
    onRamSliderValueChanged(iValue);

    /* Broadcast complete-change: */
    emit completeChanged();
}

void UIWizardNewVMPageBasic2::sltRamEditorTextChanged(const QString &strText)
{
    /* Call to base-class: */
    onRamEditorTextChanged(strText);

    /* Broadcast complete-change: */
    emit completeChanged();
}

void UIWizardNewVMPageBasic2::retranslateUi()
{
    /* Translate page: */
    setTitle(UIWizardNewVM::tr("Memory size"));

    /* Translate widgets: */
    QString strRecommendedRAM = field("type").value<CGuestOSType>().isNull() ?
                                QString() : QString::number(field("type").value<CGuestOSType>().GetRecommendedRAM());
    m_pLabel->setText(UIWizardNewVM::tr("<p>Select the amount of memory (RAM) in megabytes "
                                        "to be allocated to the virtual machine.</p>"
                                        "<p>The recommended memory size is <b>%1</b> MB.</p>")
                                        .arg(strRecommendedRAM));
    m_pRamUnits->setText(VBoxGlobal::tr("MB", "size suffix MBytes=1024 KBytes"));
    m_pRamMin->setText(QString("%1 %2").arg(m_pRamSlider->minRAM()).arg(VBoxGlobal::tr("MB", "size suffix MBytes=1024 KBytes")));
    m_pRamMax->setText(QString("%1 %2").arg(m_pRamSlider->maxRAM()).arg(VBoxGlobal::tr("MB", "size suffix MBytes=1024 KBytes")));
}

void UIWizardNewVMPageBasic2::initializePage()
{
    /* Translate page: */
    retranslateUi();

    /* Get recommended 'ram' field value: */
    CGuestOSType type = field("type").value<CGuestOSType>();
    m_pRamSlider->setValue(type.GetRecommendedRAM());
    m_pRamEditor->setText(QString::number(type.GetRecommendedRAM()));

    /* 'Ram' field should have focus initially: */
    m_pRamSlider->setFocus();
}

bool UIWizardNewVMPageBasic2::isComplete() const
{
    /* Make sure 'ram' field feats the bounds: */
    return m_pRamSlider->value() >= qMax(1, (int)m_pRamSlider->minRAM()) &&
           m_pRamSlider->value() <= (int)m_pRamSlider->maxRAM();
}

