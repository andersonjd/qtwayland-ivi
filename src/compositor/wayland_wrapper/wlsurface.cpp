/****************************************************************************
**
** Copyright (C) 2012 Nokia Corporation and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/
**
** This file is part of the Qt Compositor.
**
** $QT_BEGIN_LICENSE:BSD$
** You may use this file under the terms of the BSD license as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of Nokia Corporation and its Subsidiary(-ies) nor
**     the names of its contributors may be used to endorse or promote
**     products derived from this software without specific prior written
**     permission.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "wlsurface.h"

#include "waylandsurface.h"
#include "waylandsurfaceitem.h"

#include "wlcompositor.h"
#include "wlshmbuffer.h"
#include "wlinputdevice.h"
#include "wlextendedsurface.h"
#include "wlsubsurface.h"
#include "wlsurfacebuffer.h"
#include "wlshellsurface.h"

#include <QtCore/QDebug>
#include <QTouchEvent>

#include <wayland-server.h>

#ifdef QT_COMPOSITOR_WAYLAND_GL
#include "hardware_integration/graphicshardwareintegration.h"
#include <QtGui/QPlatformOpenGLContext>
#endif

#ifdef QT_WAYLAND_WINDOWMANAGER_SUPPORT
#include "waylandwindowmanagerintegration.h"
#endif

namespace Wayland {

void destroy_surface(struct wl_resource *resource)
{
    Surface *surface = resolve<Surface>(resource);
    surface->compositor()->surfaceDestroyed(surface);
    delete surface;
}

Surface::Surface(struct wl_client *client, uint32_t id, Compositor *compositor)
    : m_compositor(compositor)
    , m_waylandSurface(new WaylandSurface(this))
    , m_backBuffer(0)
    , m_frontBuffer(0)
    , m_surfaceMapped(false)
    , m_extendedSurface(0)
    , m_subSurface(0)
    , m_shellSurface(0)
{
    wl_list_init(&m_frame_callback_list);
    addClientResource(client, &base()->resource, id, &wl_surface_interface,
            &Surface::surface_interface, destroy_surface);
    for (int i = 0; i < buffer_pool_size; i++) {
        m_bufferPool[i] = new SurfaceBuffer(this);
    }
}

Surface::~Surface()
{
    delete m_waylandSurface;
    delete m_extendedSurface;
    delete m_subSurface;
    delete m_shellSurface;

    for (int i = 0; i < buffer_pool_size; i++) {
        if (!m_bufferPool[i]->pageFlipperHasBuffer())
            delete m_bufferPool[i];
    }
}

WaylandSurface::Type Surface::type() const
{
    SurfaceBuffer *surfaceBuffer = currentSurfaceBuffer();
    if (surfaceBuffer && surfaceBuffer->waylandBufferHandle()) {
        if (surfaceBuffer->isShmBuffer()) {
            return WaylandSurface::Shm;
        } else {
            return WaylandSurface::Texture;
        }
    }
    return WaylandSurface::Invalid;
}

bool Surface::isYInverted() const
{
    bool ret = false;
    static bool negateReturn = qgetenv("QT_COMPOSITOR_NEGATE_INVERTED_Y").toInt();
    GraphicsHardwareIntegration *graphicsHWIntegration = m_compositor->graphicsHWIntegration();

#ifdef QT_COMPOSITOR_WAYLAND_GL
    SurfaceBuffer *surfacebuffer = currentSurfaceBuffer();
    if (!surfacebuffer) {
        ret = false;
    } else if (graphicsHWIntegration && surfacebuffer->waylandBufferHandle() && type() != WaylandSurface::Shm) {
        ret = graphicsHWIntegration->isYInverted(surfacebuffer->waylandBufferHandle());
    } else
#endif
        ret = true;

    return ret != negateReturn;
}

bool Surface::visible() const
{

    SurfaceBuffer *surfacebuffer = currentSurfaceBuffer();
    return surfacebuffer->waylandBufferHandle();
}

QPointF Surface::pos() const
{
    return m_position;
}

void Surface::setPos(const QPointF &pos)
{
    bool emitChange = pos != m_position;
    m_position = pos;
    if (emitChange)
        m_waylandSurface->posChanged();
}

QSize Surface::size() const
{
    return m_size;
}

void Surface::setSize(const QSize &size)
{
    bool emitChange = size != m_size;
    m_size = size;
    if (emitChange)
        m_waylandSurface->sizeChanged();
}

QImage Surface::image() const
{
    SurfaceBuffer *surfacebuffer = currentSurfaceBuffer();
    if (surfacebuffer && !surfacebuffer->isDestroyed() && type() == WaylandSurface::Shm) {
        ShmBuffer *shmBuffer = static_cast<ShmBuffer *>(surfacebuffer->waylandBufferHandle()->user_data);
        return shmBuffer->image();
    }
    return QImage();
}

#ifdef QT_COMPOSITOR_WAYLAND_GL
GLuint Surface::textureId(QOpenGLContext *context) const
{
    const SurfaceBuffer *surfacebuffer = currentSurfaceBuffer();

    if (m_compositor->graphicsHWIntegration() && type() == WaylandSurface::Texture
         && !surfacebuffer->textureCreated()) {
        GraphicsHardwareIntegration *hwIntegration = m_compositor->graphicsHWIntegration();
        const_cast<SurfaceBuffer *>(surfacebuffer)->createTexture(hwIntegration,context);
    }
    return surfacebuffer->texture();
}
#endif // QT_COMPOSITOR_WAYLAND_GL

void Surface::sendFrameCallback()
{
    SurfaceBuffer *surfacebuffer = currentSurfaceBuffer();
    surfacebuffer->setDisplayed();
    if (m_backBuffer) {
        if (m_frontBuffer)
            m_frontBuffer->disown();
        m_frontBuffer = m_backBuffer;
    }

    bool updateNeeded = advanceBufferQueue();

    uint time = Compositor::currentTimeMsecs();
    struct wl_resource *frame_callback;
    wl_list_for_each(frame_callback, &m_frame_callback_list, link) {
        wl_resource_post_event(frame_callback,WL_CALLBACK_DONE,time);
        wl_resource_destroy(frame_callback,Compositor::currentTimeMsecs());
    }
    wl_list_init(&m_frame_callback_list);

    if (updateNeeded)
        doUpdate();
}

void Surface::frameFinished()
{
    m_compositor->frameFinished(this);
}

WaylandSurface * Surface::waylandSurface() const
{
    return m_waylandSurface;
}

QPoint Surface::lastMousePos() const
{
    return m_lastLocalMousePos;
}

void Surface::setExtendedSurface(ExtendedSurface *extendedSurface)
{
    m_extendedSurface = extendedSurface;
}

ExtendedSurface *Surface::extendedSurface() const
{
    return m_extendedSurface;
}

void Surface::setSubSurface(SubSurface *subSurface)
{
    m_subSurface = subSurface;
}

SubSurface *Surface::subSurface() const
{
    return m_subSurface;
}

void Surface::setShellSurface(ShellSurface *shellSurface)
{
    m_shellSurface = shellSurface;
}

ShellSurface *Surface::shellSurface() const
{
    return m_shellSurface;
}

Compositor *Surface::compositor() const
{
    return m_compositor;
}

bool Surface::advanceBufferQueue()
{
    //has current buffer been displayed,
    //do we have another buffer in the queue
    //and does it have a valid damage rect

    if (m_bufferQueue.size()) {
        int width = 0;
        int height = 0;
        if (m_backBuffer) {
            width = m_backBuffer->width();
            height = m_backBuffer->height();
        }

        m_backBuffer = m_bufferQueue.takeFirst();
        while (m_backBuffer && m_backBuffer->isDestroyed()) {
            m_backBuffer->disown();
            m_backBuffer = m_bufferQueue.size() ? m_bufferQueue.takeFirst() : 0;
        }

        if (!m_backBuffer)
            return false; //we have no new backbuffer;

        if (m_backBuffer->waylandBufferHandle()) {
            width = m_backBuffer->width();
            height = m_backBuffer->height();
        }
        setSize(QSize(width,height));


        if (m_backBuffer &&  (!m_subSurface || !m_subSurface->parent()) && !m_surfaceMapped) {
            m_surfaceMapped = true;
            emit m_waylandSurface->mapped();
        } else if (m_backBuffer && !m_backBuffer->waylandBufferHandle() && m_surfaceMapped) {
            m_surfaceMapped = false;
            emit m_waylandSurface->unmapped();
        }

    } else {
        m_backBuffer = 0;
        return false;
    }

    return true;
}

void Surface::doUpdate() {
    if (postBuffer()) {
        WaylandSurfaceItem *surfaceItem = waylandSurface()->surfaceItem();
        if (surfaceItem)
            surfaceItem->setDamagedFlag(true); // avoid flicker when we switch back to composited mode
        sendFrameCallback();
    } else {
        SurfaceBuffer *surfaceBuffer = currentSurfaceBuffer();
        if (surfaceBuffer) {
            if (surfaceBuffer->damageRect().isValid()) {
                m_compositor->markSurfaceAsDirty(this);
                emit m_waylandSurface->damaged(surfaceBuffer->damageRect());
            }
        }
    }
}

SurfaceBuffer *Surface::createSurfaceBuffer(struct wl_buffer *buffer)
{
    SurfaceBuffer *newBuffer = 0;
    for (int i = 0; i < Surface::buffer_pool_size; i++) {
        if (!m_bufferPool[i]->isRegisteredWithBuffer()) {
            newBuffer = m_bufferPool[i];
            newBuffer->initialize(buffer);
            break;
        }
    }

    Q_ASSERT(newBuffer);
    return newBuffer;
}

bool Surface::postBuffer() {
#ifdef QT_COMPOSITOR_WAYLAND_GL
    if (m_waylandSurface->handle() == m_compositor->directRenderSurface()) {
        SurfaceBuffer *surfaceBuffer = m_backBuffer? m_backBuffer : m_frontBuffer;
        if (surfaceBuffer && m_compositor->pageFlipper()) {
            if (m_compositor->pageFlipper()->displayBuffer(surfaceBuffer)) {
                surfaceBuffer->setPageFlipperHasBuffer(true);
                return true;
            } else {
                qDebug() << "could not post buffer";
            }
        }
    }
#endif
    return false;
}

void Surface::attach(struct wl_buffer *buffer)
{
    SurfaceBuffer *last = m_bufferQueue.size()?m_bufferQueue.last():0;
    if (last) {
        if (last->waylandBufferHandle() == buffer)
            return;
        if (!last->damageRect().isValid()) {
            last->disown();
            m_bufferQueue.takeLast();
        }
    }

    m_bufferQueue <<  createSurfaceBuffer(buffer);
}

void Surface::damage(const QRect &rect)
{
    if (m_bufferQueue.size()) {
        SurfaceBuffer *surfaceBuffer = m_bufferQueue.last();
        if (surfaceBuffer)
            surfaceBuffer->setDamage(rect);
        else
            qWarning() << "Surface::damage() null buffer";
        if (!m_backBuffer)
            advanceBufferQueue();
    } else {
        // we've receicved a second damage for the same buffer
        currentSurfaceBuffer()->setDamage(rect);
    }
    doUpdate();
}

const struct wl_surface_interface Surface::surface_interface = {
        Surface::surface_destroy,
        Surface::surface_attach,
        Surface::surface_damage,
        Surface::surface_frame
};

void Surface::surface_destroy(struct wl_client *, struct wl_resource *surface_resource)
{
    wl_resource_destroy(surface_resource,Compositor::currentTimeMsecs());
}

void Surface::surface_attach(struct wl_client *client, struct wl_resource *surface,
                    struct wl_resource *buffer, int x, int y)
{
    Q_UNUSED(client);
    Q_UNUSED(x);
    Q_UNUSED(y);
    resolve<Surface>(surface)->attach(buffer ? reinterpret_cast<wl_buffer *>(buffer->data) : 0);
}

void Surface::surface_damage(struct wl_client *client, struct wl_resource *surface,
                    int32_t x, int32_t y, int32_t width, int32_t height)
{
    Q_UNUSED(client);
    resolve<Surface>(surface)->damage(QRect(x, y, width, height));
}

void Surface::surface_frame(struct wl_client *client,
                   struct wl_resource *resource,
                   uint32_t callback)
{
    Surface *surface = resolve<Surface>(resource);
    struct wl_resource *frame_callback = wl_client_add_object(client,&wl_callback_interface,0,callback,surface);
    wl_list_insert(&surface->m_frame_callback_list,&frame_callback->link);
}

} // namespace Wayland

