/* $Id$ */
/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * VirtualBox Qt extensions: QIDialog class implementation
 */

/*
 * Copyright (C) 2008-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* GUI includes: */
#include "QIDialog.h"
#include "VBoxGlobal.h"

QIDialog::QIDialog(QWidget *pParent /* = 0 */, Qt::WindowFlags flags /* = 0 */)
    : QDialog(pParent, flags)
    , m_fPolished(false)
{
    /* No need to count that window as important for application,
     * it will NOT be taken into account when other top-level windows will be closed: */
    setAttribute(Qt::WA_QuitOnClose, false);
}

QIDialog::~QIDialog()
{
}

void QIDialog::setVisible(bool fVisible)
{
    /* Call to base-class: */
    QDialog::setVisible(fVisible);

    /* Exit from the event-loop if
     * 1. there is any and
     * 2. we are changing our state from visible to invisible: */
    if (m_pEventLoop && !fVisible)
        m_pEventLoop->exit();
}

int QIDialog::exec(bool fShow /* = true */, bool fApplicationModal /*= false*/)
{
    /* Reset the result-code: */
    setResult(QDialog::Rejected);

    /* Should we delete ourself on close in theory? */
    bool fOldDeleteOnClose = testAttribute(Qt::WA_DeleteOnClose);
    /* For the exec() time, set this attribute to 'false': */
    setAttribute(Qt::WA_DeleteOnClose, false);

    /* Which is the current window-modality? */
    Qt::WindowModality oldModality = windowModality();
    /* For the exec() time, set this attribute to 'window-modal' or 'application-modal': */
    setWindowModality(!fApplicationModal ? Qt::WindowModal : Qt::ApplicationModal);

    /* Show ourself if requested: */
    if (fShow)
        show();

    /* Create a local event-loop: */
    {
        QEventLoop eventLoop;
        m_pEventLoop = &eventLoop;

        /* Guard ourself for the case
         * we destroyed ourself in our event-loop: */
        QPointer<QIDialog> guard = this;

        /* Start the blocking event-loop: */
        eventLoop.exec();

        /* Are we still valid? */
        if (guard.isNull())
            return QDialog::Rejected;

        m_pEventLoop = 0;
    }

    /* Save the result-code early (we can delete ourself on close): */
    int iResultCode = result();

    /* Return old modality: */
    setWindowModality(oldModality);

    /* Reset attribute to previous value: */
    setAttribute(Qt::WA_DeleteOnClose, fOldDeleteOnClose);
    /* Delete ourself if we should do that on close: */
    if (fOldDeleteOnClose)
        delete this;

    /* Return the result-code: */
    return iResultCode;
}

void QIDialog::showEvent(QShowEvent *pEvent)
{
    /* Make sure we should polish dialog: */
    if (m_fPolished)
        return;

    /* Call to polish-event: */
    polishEvent(pEvent);

    /* Mark dialog as polished: */
    m_fPolished = true;
}

void QIDialog::polishEvent(QShowEvent*)
{
    /* Make sure layout is polished: */
    adjustSize();
#ifdef Q_WS_MAC
    /* And dialog have fixed size: */
    setFixedSize(size());
#endif /* Q_WS_MAC */

    /* Explicit centering according to our parent: */
    VBoxGlobal::centerWidget(this, parentWidget(), false);
}

