// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#ifndef FAKESERVICE_H
#define FAKESERVICE_H

#include <QObject>
#include <QHash>
#include <QString>

// Minimal in-process implementation of the org.deepin.dde.WallpaperSlideshow
// D-Bus interface (see
// src/plugin-qt/wallpaperslideshow/org.deepin.dde.WallpaperSlideshow.xml).
//
// It is used by the D-Bus contract test: a WallpaperSlideshowAdaptor (generated
// from the project introspection XML via qt_add_dbus_adaptor) wraps this object
// and publishes it on an isolated session bus, so the test can validate that
// the introspection XML is implementable and that every method/property
// round-trips over a real D-Bus connection — without pulling in the heavy
// SlideshowManager / DConfig / AppearanceDBusProxy dependencies of the
// production WallpaperSlideshow class.
class FakeWallpaperSlideshowService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString WallpaperSlideShow READ wallpaperSlideShow WRITE setWallpaperSlideShow)

public:
    explicit FakeWallpaperSlideshowService(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    QString wallpaperSlideShow() const { return m_property; }
    void setWallpaperSlideShow(const QString &value) { m_property = value; }

public slots:
    void SetWallpaperSlideShow(const QString &monitorName, const QString &slideShow)
    {
        m_monitors[monitorName] = slideShow;
        m_property = slideShow;
    }

    QString GetWallpaperSlideShow(const QString &monitorName)
    {
        return m_monitors.value(monitorName);
    }

private:
    QString m_property;
    QHash<QString, QString> m_monitors;
};

#endif // FAKESERVICE_H
