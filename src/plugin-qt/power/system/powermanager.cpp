// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "powermanager.h"
#include "batterymanager.h"
#include "batterydevice.h"
#include "systemdbusproxy.h"
#include "../powerconstants.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QVariantMap>
#include <QMetaProperty>
#include <QProcess>
#include <QTimer>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QJsonObject>
#include <QJsonParseError>
#include <DConfig>
#include <QLoggingCategory>
#include <polkitqt1-authority.h>
#include <polkitqt1-subject.h>

#include <grp.h>
#include <unistd.h>

using namespace PowerDBus;
using namespace PowerFS;
using namespace PowerDConfig;

static constexpr auto kPowerScope = "org.deepin.dde.Power1:/org/deepin/dde/Power1";
static constexpr auto kAllowCallerStatePath = "/run/dde-services/power_allow_callers.json";
static constexpr auto kLegacyAllowCallerStatePath = "/run/dde-daemon/security_loader_allow_callers.json";
Q_LOGGING_CATEGORY(logPowerSystem, "dde.power.system")
static constexpr auto kLegacyDBusError = "org.deepin.dde.DBus.Error.Unnamed";
static constexpr auto kPowerAction = "org.deepin.dde.power.doAction";

static bool readProcLidClosed(bool &closed)
{
    QFile file{QLatin1String(kLidStatePath)};
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray state = file.readAll();
    if (state.contains("closed")) {
        closed = true;
        return true;
    }
    if (state.contains("open")) {
        closed = false;
        return true;
    }
    return false;
}

static bool isValidPowerMode(const QString &mode)
{
    return mode == QLatin1String("balance")
        || mode == QLatin1String("powersave")
        || mode == QLatin1String("performance")
        || mode == QLatin1String("lowBattery");
}

static bool deepinDaemonGroup(gid_t &gid)
{
    const group *entry = getgrnam("deepin-daemon");
    if (!entry)
        return false;
    gid = entry->gr_gid;
    return true;
}

static bool processHasGroup(uint pid, gid_t gid)
{
    QFile file(QStringLiteral("/proc/%1/status").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return false;

    while (!file.atEnd()) {
        const QString line = QString::fromLatin1(file.readLine());
        if (!line.startsWith(QLatin1String("Gid:")) && !line.startsWith(QLatin1String("Groups:")))
            continue;
        const QStringList values = line.section(QLatin1Char(':'), 1).split(QChar::Space, Qt::SkipEmptyParts);
        for (const QString &value : values) {
            if (value.toUInt() == gid)
                return true;
        }
    }
    return false;
}

static bool readParentPid(uint pid, uint &parent)
{
    QFile file(QStringLiteral("/proc/%1/status").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return false;

    while (!file.atEnd()) {
        const QString line = QString::fromLatin1(file.readLine());
        if (!line.startsWith(QLatin1String("PPid:")))
            continue;
        bool ok = false;
        parent = line.section(QLatin1Char(':'), 1).trimmed().toUInt(&ok);
        return ok;
    }
    return false;
}

static bool isProcessDescendant(uint pid, uint ancestor)
{
    if (pid == 0 || ancestor == 0 || pid == ancestor)
        return false;

    for (int depth = 0; depth < 4096 && pid != 0; ++depth) {
        uint parent = 0;
        if (!readParentPid(pid, parent))
            return false;
        if (parent == ancestor)
            return true;
        pid = parent;
    }
    return false;
}

static QString systemBusId(QDBusConnection *conn)
{
    if (!conn)
        return {};
    QDBusInterface bus(QStringLiteral("org.freedesktop.DBus"),
                       QStringLiteral("/org/freedesktop/DBus"),
                       QStringLiteral("org.freedesktop.DBus"),
                       *conn);
    const QDBusReply<QString> reply = bus.call(QStringLiteral("GetId"));
    return reply.isValid() ? reply.value() : QString();
}

bool SystemPowerManager::addAllowedCaller(const QString &uniqueName)
{
    if (uniqueName.isEmpty() || !uniqueName.startsWith(QLatin1Char(':')))
        return false;

    auto *bus = m_conn ? m_conn->interface() : nullptr;
    if (!bus)
        return false;

    const QDBusReply<uint> uid = bus->serviceUid(uniqueName);
    const QDBusReply<uint> pid = bus->servicePid(uniqueName);
    if (!uid.isValid() || !pid.isValid())
        return false;

    if (calledFromDBus()) {
        const QString registrar = message().service();
        const QDBusReply<uint> registrarUid = bus->serviceUid(registrar);
        const QDBusReply<uint> registrarPid = bus->servicePid(registrar);
        if (!registrarUid.isValid() || !registrarPid.isValid())
            return false;
        if (registrarUid.value() == 0) {
            if (uid.value() != 0)
                return false;
        } else {
            gid_t daemonGroup = 0;
            if (!deepinDaemonGroup(daemonGroup) || !processHasGroup(registrarPid.value(), daemonGroup))
                return false;
            if (registrarUid.value() != uid.value() || !isProcessDescendant(pid.value(), registrarPid.value()))
                return false;
        }
    }

    m_allowedCallers.insert(uniqueName, {uid.value(), pid.value()});
    saveAllowedCallers();
    return true;
}

void SystemPowerManager::loadAllowedCallers()
{
    QFile file{QLatin1String(kAllowCallerStatePath)};
    const bool migrateLegacyState = !file.open(QIODevice::ReadOnly);
    if (migrateLegacyState) {
        file.setFileName(QLatin1String(kLegacyAllowCallerStatePath));
        if (!file.open(QIODevice::ReadOnly))
            return;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return;

    const QJsonObject root = document.object();
    const QString busId = systemBusId(m_conn);
    if (busId.isEmpty() || root.value(QLatin1String("busId")).toString() != busId)
        return;

    auto *bus = m_conn ? m_conn->interface() : nullptr;
    if (!bus)
        return;

    const QJsonObject callers = root.value(QLatin1String("callers")).toObject()
        .value(QLatin1String(kPowerScope)).toObject();
    for (auto it = callers.begin(); it != callers.end(); ++it) {
        const QString uniqueName = it.key();
        const QJsonObject info = it.value().toObject();
        const QDBusReply<uint> uid = bus->serviceUid(uniqueName);
        const QDBusReply<uint> pid = bus->servicePid(uniqueName);
        if (!uid.isValid() || !pid.isValid())
            continue;
        if (uid.value() != static_cast<uint>(info.value(QLatin1String("uid")).toInteger())
            || pid.value() != static_cast<uint>(info.value(QLatin1String("pid")).toInteger())) {
            continue;
        }
        m_allowedCallers.insert(uniqueName, {uid.value(), pid.value()});
    }
    if (migrateLegacyState && !m_allowedCallers.isEmpty())
        saveAllowedCallers();
}

void SystemPowerManager::saveAllowedCallers()
{
    const QString busId = systemBusId(m_conn);
    if (busId.isEmpty()) {
        qWarning(logPowerSystem) << "Failed to get system bus ID";
        return;
    }

    QJsonObject powerCallers;
    for (auto it = m_allowedCallers.cbegin(); it != m_allowedCallers.cend(); ++it) {
        powerCallers.insert(it.key(), QJsonObject{
            {QLatin1String("uid"), static_cast<qint64>(it->uid)},
            {QLatin1String("pid"), static_cast<qint64>(it->pid)},
        });
    }
    const QJsonObject root{
        {QLatin1String("busId"), busId},
        {QLatin1String("callers"), QJsonObject{
            {QLatin1String(kPowerScope), powerCallers},
        }},
    };

    const QString stateDir = QFileInfo(QLatin1String(kAllowCallerStatePath)).absolutePath();
    if (!QDir().mkpath(stateDir)
        || !QFile::setPermissions(stateDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        qWarning(logPowerSystem) << "Failed to create security-loader state directory";
        return;
    }
    QSaveFile file{QLatin1String(kAllowCallerStatePath)};
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning(logPowerSystem) << "Failed to open security-loader state file";
        return;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!file.commit())
        qWarning(logPowerSystem) << "Failed to save security-loader callers";
}

bool SystemPowerManager::isAllowedCaller(const QString &uniqueName, bool &lookupFailed) const
{
    lookupFailed = false;
    const auto it = m_allowedCallers.constFind(uniqueName);
    if (it == m_allowedCallers.cend())
        return false;

    auto *bus = m_conn ? m_conn->interface() : nullptr;
    if (!bus) {
        lookupFailed = true;
        return false;
    }

    const QDBusReply<uint> uid = bus->serviceUid(uniqueName);
    const QDBusReply<uint> pid = bus->servicePid(uniqueName);
    if (!uid.isValid() || !pid.isValid()) {
        lookupFailed = true;
        return false;
    }
    return uid.value() == it->uid && pid.value() == it->pid;
}

bool SystemPowerManager::authorizePowerAction()
{
    if (!calledFromDBus())
        return true;

    const QString sender = message().service();
    bool lookupFailed = false;
    if (isAllowedCaller(sender, lookupFailed))
        return true;
    if (lookupFailed) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Failed to verify caller credentials"));
        return false;
    }
    auto *authority = PolkitQt1::Authority::instance();
    authority->clearError();
    const auto result = authority->checkAuthorizationSync(
        QLatin1String(kPowerAction),
        PolkitQt1::SystemBusNameSubject(sender),
        PolkitQt1::Authority::AllowUserInteraction);
    if (authority->hasError()) {
        const QString error = authority->errorDetails();
        authority->clearError();
        qWarning(logPowerSystem) << "Polkit authorization failed:" << error;
        sendErrorReply(QDBusError::AccessDenied, error.isEmpty() ? QStringLiteral("Authentication failed") : error);
        return false;
    }
    if (result != PolkitQt1::Authority::Yes) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Authentication failed"));
        return false;
    }
    return true;
}

SystemPowerManager::SystemPowerManager(QDBusConnection *conn, const QString &svc,
                                       QObject *p)
    : QObject(p)
    , m_conn(conn)
{
    Q_UNUSED(svc);
}

SystemPowerManager::~SystemPowerManager()
{
    if (m_configReader) {
        QMetaObject::invokeMethod(m_configReader, &QObject::deleteLater,
                                  Qt::BlockingQueuedConnection);
        m_configReader = nullptr;
    }
    for (auto *battery : std::as_const(m_batteries))
        m_conn->unregisterObject(battery->objectPath().path());
    m_conn->unregisterObject(kPath);
}

bool SystemPowerManager::initialize()
{
    bool ok = m_conn->registerObject(kPath, this,
        QDBusConnection::ExportAllSlots |
        QDBusConnection::ExportAllSignals |
        QDBusConnection::ExportAllProperties);
    if (!ok) {
        qWarning(logPowerSystem) << "registerObject failed";
        return false;
    }
    loadAllowedCallers();

    m_powerControlProcess = new QProcess(this);
    connect(m_powerControlProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || exitCode != 0)
            qWarning(logPowerSystem) << "deepin-power-control failed:" << exitCode;
        runNextPowerControl();
    });
    connect(m_powerControlProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            qWarning(logPowerSystem) << "Failed to start deepin-power-control";
            QMetaObject::invokeMethod(this, &SystemPowerManager::runNextPowerControl,
                                      Qt::QueuedConnection);
        }
    });
    initLidSwitch();
    initPowerSavingDConfig();


    m_batteryManager = new BatteryManager(this, this);
    m_onBattery = m_batteryManager->onBattery();
    connect(m_batteryManager, &BatteryManager::onBatteryChanged, this, [this](bool onBatt) {
        if (m_onBattery == onBatt)
            return;
        m_onBattery = onBatt;
        Q_EMIT onBatteryChanged();
        recalcBatteryLow();
        updatePowerMode(false);
    });


    const QMetaObject *mo = metaObject();
    const int slotIndex = mo->indexOfSlot("notifyPropertyChanged()");
    if (slotIndex >= 0) {
        const QMetaMethod slot = mo->method(slotIndex);
        for (int i = mo->propertyOffset(); i < mo->propertyCount(); ++i) {
            const QMetaProperty property = mo->property(i);
            if (property.hasNotifySignal())
                connect(this, property.notifySignal(), this, slot);
        }
    }

    m_initDone = true;
    recalcBatteryLow();
    initializePowerMode();
    return true;
}
void SystemPowerManager::initializePowerMode()
{
    if (m_mode == QLatin1String("performance")) {
        updatePowerMode(true);
        return;
    }

    QDBusInterface displayManager(kDisplayManagerService, kDisplayManagerPath,
                                  kDisplayManagerIface, QDBusConnection::systemBus());
    const auto sessions = qdbus_cast<QList<QDBusObjectPath>>(
        displayManager.property("Sessions"));
    if (!sessions.isEmpty()) {
        updatePowerMode(true);
        return;
    }

    applyMode(QStringLiteral("performance"));
    if (!QDBusConnection::systemBus().connect(
            kDisplayManagerService, kDisplayManagerPath, kDisplayManagerIface,
            QStringLiteral("SessionAdded"), this,
            SLOT(onDisplaySessionAdded(QDBusObjectPath)))) {
        qWarning(logPowerSystem) << "Failed to watch display-manager sessions";
        updatePowerMode(true);
    }
}

void SystemPowerManager::onDisplaySessionAdded(const QDBusObjectPath &)
{
    QDBusConnection::systemBus().disconnect(
        kDisplayManagerService, kDisplayManagerPath, kDisplayManagerIface,
        QStringLiteral("SessionAdded"), this,
        SLOT(onDisplaySessionAdded(QDBusObjectPath)));
    updatePowerMode(true);
}

void SystemPowerManager::registerBattery(BatteryDevice *battery)
{
    if (!battery || m_batteries.contains(battery))
        return;
    if (!m_conn->registerObject(battery->objectPath().path(), battery,
                                QDBusConnection::ExportAllSlots |
                                QDBusConnection::ExportAllProperties)) {
        qWarning(logPowerSystem) << "Failed to register battery" << battery->objectPath().path();
        return;
    }
    m_batteries.append(battery);
    Q_EMIT BatteryAdded(battery->objectPath());
}

void SystemPowerManager::unregisterBattery(BatteryDevice *battery)
{
    if (!battery || !m_batteries.removeOne(battery))
        return;
    m_conn->unregisterObject(battery->objectPath().path());
    Q_EMIT BatteryRemoved(battery->objectPath());
}

void SystemPowerManager::initLidSwitch()
{
    SystemDBusProxy proxy;
    const QString chassis = proxy.chassis();
    if (chassis != QLatin1String("laptop") && chassis != QLatin1String("convertible"))
        return;

    QDBusInterface upower(kUPowerService, kUPowerPath, kUPowerService,
                          QDBusConnection::systemBus());
    const QVariant lidIsPresent = upower.property("LidIsPresent");
    const QVariant lidIsClosed = upower.property("LidIsClosed");
    const bool watchUPower = lidIsPresent.isValid() && lidIsClosed.isValid();
    if (watchUPower) {
        m_hasLidSwitch = lidIsPresent.toBool();
        m_lidClosed = lidIsClosed.toBool();
    } else {
        if (!readProcLidClosed(m_lidClosed))
            return;
        m_hasLidSwitch = true;
    }

    if (!m_hasLidSwitch)
        return;

    if (watchUPower) {
        QDBusConnection::systemBus().connect(
            kUPowerService, kUPowerPath,
            "org.freedesktop.DBus.Properties", "PropertiesChanged",
            this, SLOT(onUPowerPropertiesChanged(QString,QVariantMap,QStringList)));
        return;
    }

    // ponytail: poll the legacy proc fallback only when UPower is unavailable.
    auto *timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, [this] {
        bool closed = false;
        if (!readProcLidClosed(closed) || m_lidClosed == closed)
            return;
        m_lidClosed = closed;
        Q_EMIT lidClosedChanged();
        handleLidSwitchEvent(closed);
    });
    timer->start();
}

void SystemPowerManager::onUPowerPropertiesChanged(const QString &interface,
                                                    const QVariantMap &changed,
                                                    const QStringList &)
{
    if (interface != QLatin1String(kUPowerService))
        return;

    if (changed.contains("LidIsClosed")) {
        bool closed = changed.value("LidIsClosed").toBool();
        if (m_lidClosed != closed) {
            m_lidClosed = closed;
            Q_EMIT lidClosedChanged();
        }
        handleLidSwitchEvent(closed);
    }
}

void SystemPowerManager::handleLidSwitchEvent(bool closed)
{
    qDebug(logPowerSystem) << "handleLidSwitchEvent: closed=" << closed;
    if (closed) {
        Q_EMIT LidClosed();
    } else {
        Q_EMIT LidOpened();
    }
}

void SystemPowerManager::initPowerSavingDConfig()
{
    m_config = Dtk::Core::DConfig::create(kAppId, kPowerName, "", this);
    if (!m_config) return;

    migrateLegacyConfig();

    m_configReader = Dtk::Core::DConfig::create(kAppId, kPowerName);
    if (m_configReader)
        m_configReader->moveToThread(Dtk::Core::DConfig::globalThread());

    m_powerMappingConfig = m_config->value(kPowerMappingConfig).toString();

    auto apply = [this](const QString &key, const QVariant &value) {
        if (key == QLatin1String(kPowerSavingModeEnabled))
            setPowerSavingModeEnabled(value.toBool());
        else if (key == QLatin1String(kPowerSavingModeAuto))
            setPowerSavingModeAuto(value.toBool());
        else if (key == QLatin1String(kPowerSavingModeAutoWhenBatteryLow))
            setPowerSavingModeAutoWhenBatteryLow(value.toBool());
        else if (key == QLatin1String(kPowerSavingModeBrightnessDropPercent))
            setPowerSavingModeBrightnessDropPercent(value.toUInt());
        else if (key == QLatin1String(kPowerSavingModeAutoBatteryPercent))
            setPowerSavingModeAutoBatteryPercent(value.toUInt());
        else if (key == QLatin1String(kShortIdleEnable))
            m_shortIdleEnabled = value.toBool();
        else if (key == QLatin1String(kShortIdleState))
            setShortIdleState(value.toBool());
        else if (key == QLatin1String(kIdleStatePath))
            m_idleStatePath = value.toString();
        else if (key == QLatin1String(kIdleScreenStatePath))
            m_idleScreenStatePath = value.toString();
        else if (key == QLatin1String(kMode)) {
            QString mode = value.toString();
            if (!isValidPowerMode(mode)) {
                mode = QStringLiteral("balance");
                m_config->setValue(QLatin1String(kMode), mode);
            }
            if (m_loadingConfig) {
                m_mode = mode;
                if (mode != QLatin1String("powersave"))
                    m_lastMode = mode;
            } else {
                if (m_mode == mode)
                    return;
                m_suppressModeUpdate = true;
                if (m_mode == QLatin1String("powersave")
                    || mode == QLatin1String("powersave")) {
                    setPowerSavingModeAuto(false);
                    setPowerSavingModeAutoWhenBatteryLow(false);
                }
                m_suppressModeUpdate = false;
                setMode(mode);
            }
        } else if (key == QLatin1String(kPowerMappingConfig)) {
            m_powerMappingConfig = value.toString();
        }
    };

    auto load = [this, apply](const char *key) {
        const QString configKey = QLatin1String(key);
        const QVariant value = m_config->value(configKey);
        qDebug(logPowerSystem) << "DConfig load:" << configKey << value;
        apply(configKey, value);
    };

    m_loadingConfig = true;
    m_applyingConfig = true;
    load(kPowerSavingModeEnabled);
    load(kMode);
    load(kPowerSavingModeAuto);
    load(kPowerSavingModeAutoWhenBatteryLow);
    load(kPowerSavingModeBrightnessDropPercent);
    load(kPowerSavingModeAutoBatteryPercent);
    load(kShortIdleState);
    load(kShortIdleEnable);
    load(kIdleStatePath);
    load(kIdleScreenStatePath);
    load(kPowerMappingConfig);
    m_applyingConfig = false;
    m_loadingConfig = false;

    connect(m_config, &Dtk::Core::DConfig::valueChanged, this,
            [this, apply](const QString &key) {
        if (m_writingConfigKeys.contains(key))
            return;

        auto *reader = m_configReader;
        if (!reader)
            return;

        QMetaObject::invokeMethod(reader, [this, reader, key, apply] {
            const QVariant value = reader->value(key);
            QMetaObject::invokeMethod(this, [this, key, value, apply] {
                qDebug(logPowerSystem) << "DConfig value changed:" << key << value;
                m_applyingConfig = true;
                apply(key, value);
                m_applyingConfig = false;
            });
        });
    });
}

void SystemPowerManager::migrateLegacyConfig()
{
    const QString legacyPath = QStringLiteral("/var/lib/dde-daemon/power/config.json");
    QFileInfo fileInfo(legacyPath);
    if (!fileInfo.exists() || fileInfo.isSymLink())
        return;

    QFile file(legacyPath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    file.close();
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning(logPowerSystem) << "Cannot migrate legacy power config:" << error.errorString();
        return;
    }

    bool migrated = true;
    auto setBool = [this, &migrated](const char *key, bool value) {
        const QString configKey = QLatin1String(key);
        m_config->setValue(configKey, value);
        migrated = migrated && (m_config->value(configKey).toBool() == value);
    };
    auto setUInt = [this, &migrated](const char *key, uint value) {
        const QString configKey = QLatin1String(key);
        m_config->setValue(configKey, value);
        migrated = migrated && (m_config->value(configKey).toUInt() == value);
    };
    auto setString = [this, &migrated](const char *key, const QString &value) {
        const QString configKey = QLatin1String(key);
        m_config->setValue(configKey, value);
        migrated = migrated && (m_config->value(configKey).toString() == value);
    };

    const QJsonObject config = document.object();
    if (config.contains(QStringLiteral("PowerSavingModeEnabled")))
        setBool(kPowerSavingModeEnabled,
                config.value(QStringLiteral("PowerSavingModeEnabled")).toBool());
    if (config.contains(QStringLiteral("PowerSavingModeAuto")))
        setBool(kPowerSavingModeAuto,
                config.value(QStringLiteral("PowerSavingModeAuto")).toBool());
    if (config.contains(QStringLiteral("PowerSavingModeAutoWhenBatteryLow")))
        setBool(kPowerSavingModeAutoWhenBatteryLow,
                config.value(QStringLiteral("PowerSavingModeAutoWhenBatteryLow")).toBool());
    if (config.contains(QStringLiteral("PowerSavingModeBrightnessDropPercent"))) {
        uint drop = config.value(QStringLiteral("PowerSavingModeBrightnessDropPercent")).toInt();
        if (drop == 0)
            drop = 20;
        setUInt(kPowerSavingModeBrightnessDropPercent, drop);
    }
    if (config.contains(QStringLiteral("PowerSavingModeAutoBatteryPercent"))) {
        uint percent = config.value(QStringLiteral("PowerSavingModeAutoBatteryPercent")).toInt();
        if (percent < 10)
            percent = 20;
        setUInt(kPowerSavingModeAutoBatteryPercent, percent);
    }
    if (config.contains(QStringLiteral("Mode"))) {
        QString mode = config.value(QStringLiteral("Mode")).toString();
        if (!isValidPowerMode(mode))
            mode = QStringLiteral("balance");
        setString(kMode, mode);
    }

    // This is a one-time migration, matching dde-daemon. Keeping the legacy file would
    // reapply stale values over DConfig on every service restart. If the DConfig backend
    // does not echo the migrated values, preserve the legacy file for the next startup.
    if (!migrated) {
        qWarning(logPowerSystem) << "DConfig verification failed; legacy power config kept";
        return;
    }

    // Move the source out of the legacy path instead of unlinking it. That preserves a
    // recovery copy and avoids treating a racy replacement as a file to delete.
    if (QFileInfo(legacyPath).isSymLink())
        return;
    if (!QFile::rename(legacyPath, legacyPath + QLatin1String(".migrated")))
        qWarning(logPowerSystem) << "Failed to archive migrated legacy power config";
}

void SystemPowerManager::enqueuePowerControl(const QStringList &arguments)
{
    m_powerControlQueue.enqueue(arguments);
    if (!m_powerControlBusy)
        runNextPowerControl();
}

void SystemPowerManager::runNextPowerControl()
{
    if (m_powerControlQueue.isEmpty()) {
        m_powerControlBusy = false;
        return;
    }
    m_powerControlBusy = true;
    m_powerControlProcess->start(QStringLiteral("/usr/sbin/deepin-power-control"),
                                 m_powerControlQueue.dequeue());
}


void SystemPowerManager::updateHasBattery(bool has)
{
    if (m_hasBattery != has) {
        m_hasBattery = has;
        Q_EMIT hasBatteryChanged();
    }
}

void SystemPowerManager::updateBatteryInfo(double pct, uint status,
                                            quint64 tte, quint64 ttf, double cap)
{
    if (m_batteryPercentage != pct) { 
        qWarning(logPowerSystem) << "batteryPercentage changed:" << m_batteryPercentage << "→" << pct;
        m_batteryPercentage = pct;
        Q_EMIT batteryPercentageChanged();
    }

    if (m_batteryStatus != status) {
        m_batteryStatus = status;
        Q_EMIT batteryStatusChanged();
    }

    if (m_batteryTimeToEmpty != tte)
    {
        m_batteryTimeToEmpty = tte;
        Q_EMIT batteryTimeToEmptyChanged();
    }

    if (m_batteryTimeToFull != ttf) { 
        m_batteryTimeToFull = ttf; 
        Q_EMIT batteryTimeToFullChanged(); 
    }
    if (m_batteryCapacity != cap) { 
        m_batteryCapacity = cap; 
        Q_EMIT batteryCapacityChanged(); 
    }
    const bool wasBatteryLow = m_batteryLow;
    recalcBatteryLow();
    if (wasBatteryLow != m_batteryLow)
        updatePowerMode(false);
    Q_EMIT BatteryDisplayUpdate(QDateTime::currentSecsSinceEpoch());
}

void SystemPowerManager::notifyPropertyChanged()
{
    const int signalIndex = senderSignalIndex();
    const QMetaObject *mo = metaObject();
    for (int i = mo->propertyOffset(); i < mo->propertyCount(); ++i) {
        const QMetaProperty property = mo->property(i);
        if (!property.hasNotifySignal() || property.notifySignal().methodIndex() != signalIndex)
            continue;
        QDBusMessage message = QDBusMessage::createSignal(
            kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"));
        message << QLatin1String(kInterface)
                << QVariantMap{{QString::fromLatin1(property.name()), property.read(this)}}
                << QStringList();
        m_conn->send(message);
        return;
    }
}

QList<QDBusObjectPath> SystemPowerManager::GetBatteries()
{
    QList<QDBusObjectPath> paths;
    paths.reserve(m_batteries.size());
    for (const auto *battery : std::as_const(m_batteries))
        paths.append(battery->objectPath());
    return paths;
}

void SystemPowerManager::Refresh()
{
    RefreshMains();
    RefreshBatteries();
}

void SystemPowerManager::RefreshBatteries()
{
    if (m_batteryManager)
        m_batteryManager->refreshBatteries();
}

void SystemPowerManager::RefreshMains()
{
    if (m_batteryManager)
        m_batteryManager->refreshMains();
}

// This mirrors the session persistence helper while retaining the system plugin's
// own loading/applying guards and DConfig instance.
void SystemPowerManager::persist(const char *key, const QVariant &value)
{
    if (!m_config || m_loadingConfig)
        return;
    const QString configKey = QLatin1String(key);
    if (m_applyingConfig && m_config->value(configKey) == value)
        return;
    m_writingConfigKeys.insert(configKey);
    m_config->setValue(configKey, value);
    m_writingConfigKeys.remove(configKey);
}

QString SystemPowerManager::mappedDspcMode(const QString &mode) const
{
    // Parse on demand so runtime DConfig mapping changes take effect immediately; this
    // path is used only when a power mode is applied.
    const QJsonObject mapping = QJsonDocument::fromJson(m_powerMappingConfig.toUtf8()).object();
    const QJsonObject entry = mapping.value(mode).toObject();
    return entry.value(QStringLiteral("DSPCConfig")).toString();
}

void SystemPowerManager::applyMode(const QString &mode)
{
    const QString logicalMode = m_batteryLow && mode == QLatin1String("powersave")
        ? QStringLiteral("lowBattery") : mode;
    QString dspc = mappedDspcMode(logicalMode);
    if (dspc.isEmpty())
        dspc = logicalMode == QLatin1String("lowBattery") ? QStringLiteral("lowbat")
             : logicalMode == QLatin1String("powersave") ? QStringLiteral("saving")
             : logicalMode;
    if (dspc != QLatin1String("performance") && dspc != QLatin1String("balance")
        && dspc != QLatin1String("saving") && dspc != QLatin1String("lowbat")) {
        qWarning(logPowerSystem) << "Ignoring invalid DSPC mode mapping:" << dspc;
        dspc = logicalMode == QLatin1String("lowBattery") ? QStringLiteral("lowbat")
             : logicalMode == QLatin1String("powersave") ? QStringLiteral("saving")
             : logicalMode;
    }
    enqueuePowerControl({QStringLiteral("set"), dspc});
}

void SystemPowerManager::setMode(const QString &mode)
{
    if (!isValidPowerMode(mode)) {
        qWarning(logPowerSystem) << "Invalid power mode" << mode;
        return;
    }

    const bool powerSaving = mode == QLatin1String("powersave")
        || mode == QLatin1String("lowBattery");
    const QString exposedMode = mode == QLatin1String("lowBattery")
        ? QStringLiteral("powersave") : mode;

    applyMode(mode);
    const bool wasSettingMode = m_settingMode;
    m_settingMode = true;
    setPowerSavingModeEnabled(powerSaving);
    m_settingMode = wasSettingMode;
    if (!powerSaving)
        m_lastMode = mode;
    if (m_shortIdleState) {
        QTimer::singleShot(500, this, [this]() {
            if (m_shortIdleState)
                applyMode(QStringLiteral("powersave"));
        });
    }

    if (m_mode == exposedMode)
        return;
    m_mode = exposedMode;
    Q_EMIT modeChanged();
    persist(kMode, exposedMode);
}

void SystemPowerManager::setPowerSavingModeEnabled(bool value)
{
    if (!m_settingMode && !authorizePowerAction())
        return;

    if (m_psmEnabled == value)
        return;
    m_psmEnabled = value;
    Q_EMIT powerSavingModeEnabledChanged();
    persist(kPowerSavingModeEnabled, value);
    if (m_loadingConfig || m_settingMode)
        return;

    m_suppressModeUpdate = true;
    setPowerSavingModeAuto(false);
    setPowerSavingModeAutoWhenBatteryLow(false);
    m_suppressModeUpdate = false;
    setMode(value ? QStringLiteral("powersave") : QStringLiteral("balance"));
}

void SystemPowerManager::setPowerSavingModeAuto(bool value)
{
    if (!m_suppressModeUpdate && !authorizePowerAction())
        return;

    if (m_psmAuto == value)
        return;
    m_psmAuto = value;
    Q_EMIT powerSavingModeAutoChanged();
    persist(kPowerSavingModeAuto, value);
    if (!m_loadingConfig && !m_suppressModeUpdate)
        updatePowerMode(false);
}

void SystemPowerManager::setPowerSavingModeAutoWhenBatteryLow(bool value)
{
    if (!m_suppressModeUpdate && !authorizePowerAction())
        return;

    if (m_psmAutoLow == value)
        return;
    m_psmAutoLow = value;
    Q_EMIT powerSavingModeAutoWhenBatteryLowChanged();
    persist(kPowerSavingModeAutoWhenBatteryLow, value);
    recalcBatteryLow();
    if (!m_loadingConfig && !m_suppressModeUpdate)
        updatePowerMode(false);
}

void SystemPowerManager::setPowerSavingModeBrightnessDropPercent(uint value)
{
    if (!authorizePowerAction())
        return;

    if (m_psmDrop == value)
        return;
    m_psmDrop = value;
    Q_EMIT powerSavingModeBrightnessDropPercentChanged();
    persist(kPowerSavingModeBrightnessDropPercent, value);
}

void SystemPowerManager::setPowerSavingModeAutoBatteryPercent(uint value)
{
    if (!authorizePowerAction())
        return;

    if (m_psmAutoPct == value)
        return;
    m_psmAutoPct = value;
    Q_EMIT powerSavingModeAutoBatteryPercentChanged();
    persist(kPowerSavingModeAutoBatteryPercent, value);
    recalcBatteryLow();
    if (!m_loadingConfig && !m_suppressModeUpdate)
        updatePowerMode(false);
}

void SystemPowerManager::setPowerSavingModeBrightnessData(const QString &value)
{
    if (!authorizePowerAction())
        return;

    if (m_psmBrightnessData == value)
        return;
    m_psmBrightnessData = value;
    Q_EMIT powerSavingModeBrightnessDataChanged();
}

void SystemPowerManager::setSupportSwitchPowerMode(bool value)
{
    if (!authorizePowerAction())
        return;

    if (m_supportSwitchPowerMode == value)
        return;
    m_supportSwitchPowerMode = value;
    Q_EMIT supportSwitchPowerModeChanged();
}

void SystemPowerManager::SetCpuGovernor(const QString &gov)
{
    if (!authorizePowerAction())
        return;

    setCpuGovernor(gov);
}

void SystemPowerManager::SetCpuBoost(bool on)
{
    if (!authorizePowerAction())
        return;

    setCpuBoost(on);
}

void SystemPowerManager::LockCpuFreq(const QString &gov, int lockTime)
{
    Q_UNUSED(gov);
    Q_UNUSED(lockTime);
}

void SystemPowerManager::SetMode(const QString &mode)
{
    if (!authorizePowerAction())
        return;

    if (m_mode == mode) {
        const QString error = QStringLiteral("repeat set mode");
        if (calledFromDBus())
            sendErrorReply(QLatin1String(kLegacyDBusError), error);
        else
            qWarning(logPowerSystem) << error;
        return;
    }
    if (!isValidPowerMode(mode)) {
        const QString error = QStringLiteral("PowerMode \"%1\" mode is not supported").arg(mode);
        if (calledFromDBus())
            sendErrorReply(QLatin1String(kLegacyDBusError), error);
        else
            qWarning(logPowerSystem) << error;
        return;
    }

    m_suppressModeUpdate = true;
    if (m_mode == QLatin1String("powersave") || mode == QLatin1String("powersave")) {
        setPowerSavingModeAuto(false);
        setPowerSavingModeAutoWhenBatteryLow(false);
    }
    m_suppressModeUpdate = false;
    setMode(mode);
}

void SystemPowerManager::SetAllowCaller(const QString &uniqueName)
{
    if (!addAllowedCaller(uniqueName)) {
        const QString error = QStringLiteral("invalid caller %1").arg(uniqueName);
        if (calledFromDBus())
            sendErrorReply(QDBusError::InvalidArgs, error);
        else
            qWarning(logPowerSystem) << error;
    }
}

void SystemPowerManager::SetTlpMode(const QString &mode)
{
    if (!authorizePowerAction())
        return;

    QString error;
    if (m_tlpMode == mode)
        error = QStringLiteral("repeat set tlp mode");
    else if (!isValidPowerMode(mode))
        error = QStringLiteral("PowerMode \"%1\" mode is not supported").arg(mode);
    if (!error.isEmpty()) {
        if (calledFromDBus())
            sendErrorReply(QLatin1String(kLegacyDBusError), error);
        else
            qWarning(logPowerSystem) << error;
        return;
    }

    m_tlpMode = mode;
    Q_EMIT tlpModeChanged();
    applyMode(mode);
}

void SystemPowerManager::SetShortIdleState(bool state)
{
    if (!authorizePowerAction())
        return;
    setShortIdleState(state);
}
void SystemPowerManager::SetIdleState(bool state)
{
    if (!authorizePowerAction())
        return;
    qInfo(logPowerSystem) << "SetIdleState:" << m_idleStatePath << state;
    setShortIdleState(state);
    writeStateFile(m_idleStatePath, state);
}
void SystemPowerManager::SetScreenState(bool state)
{
    if (!authorizePowerAction())
        return;
    qInfo(logPowerSystem) << "SetScreenState:" << m_idleScreenStatePath << state;
    writeStateFile(m_idleScreenStatePath, state);
}
void SystemPowerManager::setShortIdleState(bool state)
{
    qInfo(logPowerSystem) << "SetShortIdleState:" << state;
    if (!m_shortIdleEnabled) {
        qInfo(logPowerSystem) << "Short idle is disabled by DConfig";
        return;
    }
    if (m_shortIdleState == state) {
        qInfo(logPowerSystem) << "Short idle state is unchanged:" << state;
        return;
    }
    m_shortIdleState = state;
    Q_EMIT shortIdleStateChanged();
    persist(kShortIdleState, state);
    QString powerState = m_mode;
    if (state)
        powerState = m_batteryLow ? QStringLiteral("lowBattery") : QStringLiteral("powersave");
    else if (m_mode == QLatin1String("powersave") && m_batteryLow)
        powerState = QStringLiteral("lowBattery");
    applyMode(powerState);
    enqueuePowerControl({QStringLiteral("idle"), QStringLiteral("wifi"),
                         state ? QStringLiteral("on") : QStringLiteral("off")});
}
bool SystemPowerManager::writeStateFile(const QString &path, bool state)
{
    if (!QFileInfo::exists(path)) {
        const QString error = QStringLiteral("%1 not found").arg(path);
        qWarning(logPowerSystem) << error;
        if (calledFromDBus())
            sendErrorReply(QDBusError::Failed, error);
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        const QString error = QStringLiteral("Failed to read %1: %2").arg(path, file.errorString());
        qWarning(logPowerSystem) << error;
        if (calledFromDBus())
            sendErrorReply(QDBusError::Failed, error);
        return false;
    }
    const QByteArray current = file.readAll().trimmed();
    file.close();
    const QByteArray value = state ? QByteArrayLiteral("1") : QByteArrayLiteral("0");
    qInfo(logPowerSystem) << "Current content=" << current << "will set" << value;
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(value) != value.size()) {
        const QString error = QStringLiteral("Failed to write %1: %2").arg(path, file.errorString());
        qWarning(logPowerSystem) << error;
        if (calledFromDBus())
            sendErrorReply(QDBusError::Failed, error);
        return false;
    }
    file.close();
    ::sync();
    qInfo(logPowerSystem) << "Successfully updated" << path << "with value" << value;
    return true;
}

void SystemPowerManager::recalcBatteryLow()
{
    bool old = m_batteryLow;
    // Legacy dde-daemon tracked low battery from capacity only. AC state gates the
    // general battery-power auto switch; the low-battery auto switch is independent.
    m_batteryLow = m_hasBattery
                   && m_batteryPercentage <= static_cast<double>(m_psmAutoPct);
    qDebug(logPowerSystem) << "recalcBatteryLow:" << old << "→" << m_batteryLow
                             << "(HasBattery=" << m_hasBattery
                             << " pct=" << m_batteryPercentage
                             << " threshold=" << m_psmAutoPct << ")";
}

void SystemPowerManager::updatePowerMode(bool init)
{
    if (!m_initDone)
        return;

    bool enablePowerSave = m_psmAuto && m_onBattery;
    bool enableLowPower = m_psmAutoLow && m_batteryLow;

    qDebug(logPowerSystem) << "updatePowerMode: init=" << init
                             << " PSMAuto=" << m_psmAuto
                             << " OnBattery=" << m_onBattery
                             << " PSMAutoLow=" << m_psmAutoLow
                             << " BatteryLow=" << m_batteryLow
                             << " currentMode=" << m_mode
                             << " lastMode=" << m_lastMode;

    // When both automatic switches are disabled, preserve the user's current mode.
    // dde-daemon returned here instead of forcing a rollback to lastMode.
    if (!m_psmAuto && !m_psmAutoLow && !init)
        return;

    QString target = init ? m_mode : m_lastMode;
    if (enablePowerSave || enableLowPower)
        target = "powersave";
    setMode(target);
}
