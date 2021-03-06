/****************************************************************************
**
** Copyright (C) 2012 Nokia Corporation and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/
**
** This file is part of the plugins of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this
** file. Please review the following information to ensure the GNU Lesser
** General Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU General
** Public License version 3.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of this
** file. Please review the following information to ensure the GNU General
** Public License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** Other Usage
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef QWAYLANDXCOMPOSITEEGLWINDOW_H
#define QWAYLANDXCOMPOSITEEGLWINDOW_H

#include "qwaylandwindow.h"
#include "qwaylandbuffer.h"

#include "qwaylandxcompositeeglintegration.h"
#include "qwaylandxcompositeeglcontext.h"

class QWaylandXCompositeEGLWindow : public QWaylandWindow
{
public:
    QWaylandXCompositeEGLWindow(QWindow *window, QWaylandXCompositeEGLIntegration *glxIntegration);
    WindowType windowType() const;

    void setGeometry(const QRect &rect);
    void requestActivateWindow();

    EGLSurface eglSurface() const;

private:
    void createEglSurface();

    QWaylandXCompositeEGLIntegration *m_glxIntegration;
    QWaylandXCompositeEGLContext *m_context;
    QWaylandBuffer *m_buffer;

    Window m_xWindow;
    EGLConfig m_config;
    EGLSurface m_surface;

    bool m_waitingForSync;
    static const struct wl_callback_listener m_callback_listener;
    static void done(void *data,
             struct wl_callback *wl_callback,
             uint32_t time);
};

#endif // QWAYLANDXCOMPOSITEEGLWINDOW_H
