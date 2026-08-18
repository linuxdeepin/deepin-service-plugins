// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "utils.h"

#include <QTest>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QVariantMap>

// Unit test for the wallpaperslideshow plugin's `utils` helper class.
// Covers the pure, side-effect-free static helpers. Filesystem-touching
// helpers use QTemporaryDir/QTemporaryFile for isolation.
class TestWpsslUtils : public QObject
{
    Q_OBJECT

private slots:
    void isURI_data();
    void isURI();
    void deCodeURI_data();
    void deCodeURI();
    void enCodeURI_data();
    void enCodeURI();
    void isSolidWallpaper_data();
    void isSolidWallpaper();
    void isDirDistinguishesDirAndFile();
    void isFilesInDir();
    void isFileExistsForPlainPath();
    void isFileExistsUriInputReturnsFalseDueToDecodeBug();
    void isDirForNonExistentPathReturnsFalse();
    void writeStringToFileEmptyNameReturnsFalse();
    void writeStringToFileNonEmptyAlwaysFailsDueToSwapDirBug();
    void writeStringToFileUnwritableParentReturnsFalse();
    void checkWallpaperLockedStatusReturnsBool();
    void userHomeDirIsNonEmpty();
    void userDataConfigCacheRuntimeDirsNonEmpty();
    void writeWallpaperConfigIsolatedByXdgConfigHome();
};

void TestWpsslUtils::isURI_data()
{
    QTest::addColumn<QString>("uri");
    QTest::addColumn<bool>("expected");

    QTest::newRow("file-uri") << "file:///home/uos/pic.png" << true;
    QTest::newRow("http-uri") << "http://example.com/a.png" << true;
    QTest::newRow("scheme-only") << "scheme://" << true;
    QTest::newRow("plain-path") << "/home/uos/pic.png" << false;
    QTest::newRow("relative") << "pic.png" << false;
    QTest::newRow("empty") << "" << false;
}

void TestWpsslUtils::isURI()
{
    QFETCH(QString, uri);
    QFETCH(bool, expected);
    QCOMPARE(utils::isURI(uri), expected);
}

void TestWpsslUtils::deCodeURI_data()
{
    QTest::addColumn<QString>("uri");
    QTest::addColumn<QString>("expected");

    QTest::newRow("file-uri") << "file:///home/uos/pic.png" << "/home/uos/pic.png";
    QTest::newRow("http-uri") << "http://example.com/a/b.png" << "/a/b.png";
    QTest::newRow("plain-path") << "/home/uos/pic.png" << "/home/uos/pic.png";
    QTest::newRow("empty") << "" << "";
}

void TestWpsslUtils::deCodeURI()
{
    QFETCH(QString, uri);
    QFETCH(QString, expected);
    QCOMPARE(utils::deCodeURI(uri), expected);
}

void TestWpsslUtils::enCodeURI_data()
{
    QTest::addColumn<QString>("content");
    QTest::addColumn<QString>("scheme");
    QTest::addColumn<QString>("expected");

    QTest::newRow("encode-uri") << "file:///home/uos/x" << "file" << "file/home/uos/x";
    QTest::newRow("encode-plain") << "/home/uos/x" << "file" << "file/home/uos/x";
    QTest::newRow("encode-http") << "http://h/a" << "file" << "file/a";
}

void TestWpsslUtils::enCodeURI()
{
    QFETCH(QString, content);
    QFETCH(QString, scheme);
    QFETCH(QString, expected);
    QCOMPARE(utils::enCodeURI(content, scheme), expected);
}

void TestWpsslUtils::isSolidWallpaper_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("expected");

    QTest::newRow("custom-cache") << "/var/cache/wallpapers/custom-solidwallpapers/blue.png" << true;
    QTest::newRow("share-dir") << "/usr/share/wallpapers/deepin-solidwallpapers/red.jpg" << true;
    QTest::newRow("user-pic") << "/home/uos/Pictures/photo.png" << false;
    QTest::newRow("unrelated-cache") << "/var/cache/wallpapers/other/x.png" << false;
}

void TestWpsslUtils::isSolidWallpaper()
{
    QFETCH(QString, path);
    QFETCH(bool, expected);
    QCOMPARE(utils::isSolidWallpaper(path), expected);
}

void TestWpsslUtils::isDirDistinguishesDirAndFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(utils::isDir(dir.path()));

    QTemporaryFile file;
    QVERIFY(file.open());
    QVERIFY(!utils::isDir(file.fileName()));
}

void TestWpsslUtils::isFilesInDir()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString a = dir.filePath("a.txt");
    const QString b = dir.filePath("b.txt");
    QVERIFY(QFile(a).open(QIODevice::WriteOnly));
    QVERIFY(QFile(b).open(QIODevice::WriteOnly));

    QVERIFY(utils::isFilesInDir({ "a.txt", "b.txt" }, dir.path()));
    // missing "c.txt" -> false
    QVERIFY(!utils::isFilesInDir({ "a.txt", "c.txt" }, dir.path()));
    // non-existent directory -> false
    QVERIFY(!utils::isFilesInDir({ "a.txt" }, dir.path() + "/nope"));
}

void TestWpsslUtils::isFileExistsForPlainPath()
{
    // NOTE: utils::isFileExists decodes the URI into a local variable `path`
    // but then checks QFile::exists(filename) (the original input). For plain
    // (non-URI) paths the decoded value equals the input, so the check still
    // works. URI inputs are a latent bug (tracked separately); here we only
    // exercise plain paths where the contract holds.
    QTemporaryFile file;
    QVERIFY(file.open());
    QVERIFY(utils::isFileExists(file.fileName()));
    QVERIFY(!utils::isFileExists(file.fileName() + ".missing"));
}

void TestWpsslUtils::userHomeDirIsNonEmpty()
{
    const QString home = utils::GetUserHomeDir();
    QVERIFY(!home.isEmpty());
    QVERIFY(QFileInfo(home).isDir());
}

// ---- branch-coverage additions ----

void TestWpsslUtils::isFileExistsUriInputReturnsFalseDueToDecodeBug()
{
    // Defect #1 (recorded, not fixed): isFileExists decodes the URI into a
    // local `path` but checks QFile::exists(filename) (the raw URI string),
    // so a file:// URI never resolves even if the underlying path exists.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("real.png"));
    QVERIFY(QFile(target).open(QIODevice::WriteOnly));

    const QString uri = QStringLiteral("file://") + target;
    QVERIFY(QFile::exists(target));           // the plain path does exist
    QVERIFY(!utils::isFileExists(uri));      // ... but isFileExists(URI) is false
    // sanity: the decoded path itself is reachable
    QCOMPARE(utils::deCodeURI(uri), target);
}

void TestWpsslUtils::isDirForNonExistentPathReturnsFalse()
{
    QVERIFY(!utils::isDir(QStringLiteral("/nonexistent_dde_svc_dir_xyz/path")));
}

void TestWpsslUtils::writeStringToFileEmptyNameReturnsFalse()
{
    QCOMPARE(utils::WriteStringToFile(QString(), QStringLiteral("x")), false);
}

void TestWpsslUtils::writeStringToFileNonEmptyAlwaysFailsDueToSwapDirBug()
{
    // Defect #6 (recorded, not fixed): WriteStringToFile builds swapFile as
    // "<filename>/.swap" and calls QDir::mkpath(swapFile), which creates
    // ".swap" as a DIRECTORY; subsequently opening that directory path with
    // QFile::open(WriteOnly) fails (EISDIR), so the function always returns
    // false for any non-empty filename. Assert the actual behavior.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("out.txt"));
    QCOMPARE(utils::WriteStringToFile(target, QStringLiteral("hello")), false);
    // the directory "<target>/.swap" was created as a side effect; cleanup
    QDir(dir.filePath(QStringLiteral("out.txt/.swap"))).removeRecursively();
}

void TestWpsslUtils::writeStringToFileUnwritableParentReturnsFalse()
{
    // mkpath("<filename>/.swap") fails when <filename> itself is a file (not a
    // directory), so the swap dir cannot be created underneath it.
    QTemporaryFile fileParent;
    QVERIFY(fileParent.open());
    // filename = an existing regular file -> swapFile = "<file>/.swap"
    QCOMPARE(utils::WriteStringToFile(fileParent.fileName(), QStringLiteral("x")), false);
}

void TestWpsslUtils::checkWallpaperLockedStatusReturnsBool()
{
    // smoke test: callable without crash; return value depends on external
    // /var/lib/deepin/permission-manager/wallpaper_locked state, not asserted
    // (clean CI/build env has no lock file -> false; real DDE sessions may
    // differ). Previously a tautological QVERIFY(locked==false||locked==true)
    // which is always true and validated nothing.
    const bool locked = utils::checkWallpaperLockedStatus();
    Q_UNUSED(locked);
}

void TestWpsslUtils::userDataConfigCacheRuntimeDirsNonEmpty()
{
    QVERIFY(!utils::GetUserDataDir().isEmpty());
    QVERIFY(!utils::GetUserConfigDir().isEmpty());
    QVERIFY(!utils::GetUserCacheDir().isEmpty());
    // RuntimeLocation may legitimately be empty in some sandboxes; only
    // require the function to be callable without crashing.
    (void) utils::GetUserRuntimeDir();
}

void TestWpsslUtils::writeWallpaperConfigIsolatedByXdgConfigHome()
{
    // writeWallpaperConfig writes to a file-static path derived from
    // QStandardPaths::ConfigLocation at static-init time. It is only safe to
    // exercise when XDG_CONFIG_HOME is redirected (via the ctest ENVIRONMENT
    // property) away from the real user config dir; otherwise skip.
    if (!qEnvironmentVariableIsSet("XDG_CONFIG_HOME"))
        QSKIP("XDG_CONFIG_HOME not set; cannot isolate writeWallpaperConfig "
              "without polluting the real user config dir");

    const QString baseDir = utils::GetUserConfigDir() + QStringLiteral("/dde-appearance");
    const QString configFile = baseDir + QStringLiteral("/config.json");

    // clean slate (covers the "dir does not exist" branch on first call)
    QFile::remove(configFile);
    QDir().rmdir(baseDir);

    QVariantMap data;
    data.insert(QStringLiteral("w"), QStringLiteral("1.jpg"));
    data.insert(QStringLiteral("t"), QStringLiteral("slideshow"));
    utils::writeWallpaperConfig(QVariant(data));

    QVERIFY(QFile::exists(configFile));
    QFile f(configFile);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    QCOMPARE(doc.toVariant().toMap().value(QStringLiteral("w")).toString(),
             QStringLiteral("1.jpg"));

    // second call: dir already exists -> covers the "dir exists" branch
    utils::writeWallpaperConfig(QVariant(data));
    QVERIFY(QFile::exists(configFile));

    // cleanup
    QFile::remove(configFile);
    QDir().rmdir(baseDir);
}

QTEST_GUILESS_MAIN(TestWpsslUtils)

#include "tst_wpssl_utils.moc"
