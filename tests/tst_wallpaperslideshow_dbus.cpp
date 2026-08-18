// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "fakeservice.h"
#include "wallpaperslideshowadaptor.h"

#include <QTest>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>
#include <QCoreApplication>

// D-Bus contract test for the org.deepin.dde.WallpaperSlideshow interface.
//
// The interface is defined by the project introspection XML at
//   src/plugin-qt/wallpaperslideshow/org.deepin.dde.WallpaperSlideshow.xml
// and the production adaptor is generated from it via qt_add_dbus_adaptor.
// This test generates the *same* adaptor around a FakeWallpaperSlideshowService
// (see fakeservice.h), registers the object on the session bus, and verifies:
//   1. Introspection publishes the interface name and its methods/property.
//   2. SetWallpaperSlideShow / GetWallpaperSlideShow round-trip per monitor.
//   3. The WallpaperSlideShow property is read/write over D-Bus.
//
// To avoid colliding with a possibly-running production service, the test
// registers a unique per-process service name; the *interface* name under test
// remains the real org.deepin.dde.WallpaperSlideshow. The object path matches
// the production WALLPAPER_SLIDESHOW_PATH.
//
// Dependency: requires a reachable session bus (DBUS_SESSION_BUS_ADDRESS).
// The executor is expected to run this on an isolated session bus; if none is
// available the test skips rather than fails.

static const char *const kInterface = "org.deepin.dde.WallpaperSlideshow";
static const char *const kPath = "/org/deepin/dde/WallpaperSlideshow";

class TestWallpaperSlideshowDBus : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void introspectionPublishesInterface();
    void setGetRoundTripsPerMonitor();
    void monitorsAreIndependent();
    void propertyIsReadWrite();

private:
    QDBusConnection bus() const { return QDBusConnection::sessionBus(); }
    QString m_service;
};

void TestWallpaperSlideshowDBus::initTestCase()
{
    if (!bus().isConnected())
        QSKIP("no session bus available; run on an isolated session bus");

    m_service = QStringLiteral("org.deepin.dde.WallpaperSlideshow.Test.p%1")
                    .arg(QCoreApplication::applicationPid());
    QVERIFY2(bus().registerService(m_service),
             "failed to acquire service name on the session bus; "
             "run on an isolated session bus");

    auto *service = new FakeWallpaperSlideshowService(this);
    new WallpaperSlideshowAdaptor(service); // adaptor is a child of `service`
    QVERIFY2(bus().registerObject(QLatin1String(kPath), service,
                                  QDBusConnection::ExportAdaptors),
             "failed to register object on the session bus");
}

void TestWallpaperSlideshowDBus::introspectionPublishesInterface()
{
    QDBusMessage intro = QDBusMessage::createMethodCall(
        m_service, QLatin1String(kPath),
        QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"));
    const QDBusReply<QString> reply = bus().call(intro);
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    const QString xml = reply.value();

    QVERIFY2(xml.contains(QLatin1String(kInterface)),
             "introspection does not expose org.deepin.dde.WallpaperSlideshow");
    QVERIFY2(xml.contains(QStringLiteral("GetWallpaperSlideShow")),
             "introspection missing method GetWallpaperSlideShow");
    QVERIFY2(xml.contains(QStringLiteral("SetWallpaperSlideShow")),
             "introspection missing method SetWallpaperSlideShow");
    QVERIFY2(xml.contains(QStringLiteral("WallpaperSlideShow")),
             "introspection missing property WallpaperSlideShow");
}

void TestWallpaperSlideshowDBus::setGetRoundTripsPerMonitor()
{
    QDBusInterface iface(m_service, QLatin1String(kPath), QLatin1String(kInterface), bus());
    QVERIFY(iface.isValid());

    iface.call(QStringLiteral("SetWallpaperSlideShow"),
               QStringLiteral("eDP-1"), QStringLiteral("2000"));
    const QDBusReply<QString> r = iface.call(QStringLiteral("GetWallpaperSlideShow"),
                                             QStringLiteral("eDP-1"));
    QVERIFY2(r.isValid(), qPrintable(r.error().message()));
    QCOMPARE(r.value(), QStringLiteral("2000"));
}

void TestWallpaperSlideshowDBus::monitorsAreIndependent()
{
    QDBusInterface iface(m_service, QLatin1String(kPath), QLatin1String(kInterface), bus());
    QVERIFY(iface.isValid());

    iface.call(QStringLiteral("SetWallpaperSlideShow"),
               QStringLiteral("HDMI-1"), QStringLiteral("600"));
    iface.call(QStringLiteral("SetWallpaperSlideShow"),
               QStringLiteral("DP-1"), QStringLiteral("1200"));

    QCOMPARE(QDBusReply<QString>(iface.call(QStringLiteral("GetWallpaperSlideShow"),
                                            QStringLiteral("HDMI-1"))).value(),
             QStringLiteral("600"));
    QCOMPARE(QDBusReply<QString>(iface.call(QStringLiteral("GetWallpaperSlideShow"),
                                            QStringLiteral("DP-1"))).value(),
             QStringLiteral("1200"));
}

void TestWallpaperSlideshowDBus::propertyIsReadWrite()
{
    // Properties.Set
    QDBusMessage setMsg = QDBusMessage::createMethodCall(
        m_service, QLatin1String(kPath),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Set"));
    setMsg << QLatin1String(kInterface) << QStringLiteral("WallpaperSlideShow")
           << QVariant::fromValue(QDBusVariant(QStringLiteral("240")));
    const QDBusReply<void> setReply = bus().call(setMsg);
    QVERIFY2(setReply.isValid(), qPrintable(setReply.error().message()));

    // Properties.Get
    QDBusMessage getMsg = QDBusMessage::createMethodCall(
        m_service, QLatin1String(kPath),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    getMsg << QLatin1String(kInterface) << QStringLiteral("WallpaperSlideShow");
    const QDBusReply<QDBusVariant> getReply = bus().call(getMsg);
    QVERIFY2(getReply.isValid(), qPrintable(getReply.error().message()));
    QCOMPARE(getReply.value().variant().toString(), QStringLiteral("240"));
}

QTEST_MAIN(TestWallpaperSlideshowDBus)

#include "tst_wallpaperslideshow_dbus.moc"
