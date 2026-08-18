// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "keyfile.h"

#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>

// Unit test for the xsettings plugin's KeyFile (ini/.desktop parser).
// Exercises loadFile/getStr/getBool/containKey/getStrList/getMainKeys and the
// setKey/saveToFile round-trip. Temp fixtures live in a QTemporaryDir so they
// are removed on scope exit even when a QVERIFY fails mid-test.
class TestKeyFile : public QObject
{
    Q_OBJECT

private slots:
    void loadAndQuery();
    void getStrFallsBackToDefault();
    void getBoolFallsBackToDefaultForPresentSection();
    void getBoolMissingSectionReturnsFalseNotDefault();
    void getStrListSplitsOnSeparator();
    void customSeparator();
    void setKeyAndSaveRoundTrip();
    void deleteKeyRemovesEntry();

    // --- branch-coverage additions (no rewrite of above) ---
    void loadFileMissingFileReturnsFalse();
    void loadFileEmptyFileReturnsTrueNoSections();
    void loadFileSkipsCommentLine();
    void loadFileSkipsLineWithoutEquals();
    void loadFileKeyBeforeSectionReturnsFalse();
    void loadFileSectionLineWithTrailingJunkNotParsed();
    void loadFileValueContainingEquals();
    void loadFileMultipleSectionsAndMainKeys();
    void getStrEmptyValueFallsBackToDefault();
    void getStrListEmptyValueReturnsSingleEmptyElement();
    void getBoolMissingKeyDefaultFalse();
    void containKeyMissingSectionReturnsFalse();
    void deleteKeyMissingSectionReturnsFalse();
    void saveToFileUnwritablePathReturnsFalse();
    void printRunsLoopWithoutCrash();
};

// Write `content` to `<dir>/<name>` and return the path. The QTemporaryDir
// owns the cleanup (RAII); the caller keeps it alive for the test's scope.
static QString writeIni(const QTemporaryDir &dir, const QString &name, const QString &content)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    QTextStream(&f) << content;
    f.close();
    return path;
}

void TestKeyFile::loadAndQuery()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("a.ini"),
        QStringLiteral("[Display]\nWidth=1920\nHeight=1080\nEnabled=true\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));

    QCOMPARE(kf.getStr("Display", "Width"), QStringLiteral("1920"));
    QCOMPARE(kf.getStr("Display", "Height"), QStringLiteral("1080"));
    QCOMPARE(kf.getBool("Display", "Enabled"), true);
    QVERIFY(kf.containKey("Display", "Width"));
    QVERIFY(!kf.containKey("Display", "Missing"));
    QCOMPARE(kf.getMainKeys(), (QStringList{ QStringLiteral("Display") }));
}

void TestKeyFile::getStrFallsBackToDefault()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("a.ini"), QStringLiteral("[Display]\nWidth=1920\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));

    // missing key inside an existing section -> default
    QCOMPARE(kf.getStr("Display", "Missing", QStringLiteral("def")), QStringLiteral("def"));
    // missing section -> default
    QCOMPARE(kf.getStr("Absent", "Width", QStringLiteral("def")), QStringLiteral("def"));
}

void TestKeyFile::getBoolFallsBackToDefaultForPresentSection()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("a.ini"), QStringLiteral("[Display]\nWidth=1920\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));

    // Key present but not a bool literal -> default value is kept.
    QCOMPARE(kf.getBool("Display", "Width", true), true);
    // Key absent (empty value) inside existing section -> default.
    QCOMPARE(kf.getBool("Display", "Missing", true), true);

    // Explicit false literal.
    const QString path2 = writeIni(dir, QStringLiteral("b.ini"), QStringLiteral("[S]\nFlag=false\n"));
    KeyFile kf2;
    QVERIFY(kf2.loadFile(path2));
    QCOMPARE(kf2.getBool("S", "Flag", true), false);
}

void TestKeyFile::getBoolMissingSectionReturnsFalseNotDefault()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("a.ini"), QStringLiteral("[Display]\nWidth=1920\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));

    // Defect #3 (recorded, not fixed): when the section is missing getBool
    // returns false (NOT defaultValue). Assert the *actual* behavior; flip to
    // `true` once getBool honors defaultValue for a missing section.
    QCOMPARE(kf.getBool("Absent", "Flag", true), false);
}

void TestKeyFile::getStrListSplitsOnSeparator()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("a.ini"), QStringLiteral("[S]\nNames=a;b;c\n"));
    KeyFile kf; // default separator ';'
    QVERIFY(kf.loadFile(path));
    QCOMPARE(kf.getStrList("S", "Names"),
             (QStringList{ QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") }));
}

void TestKeyFile::customSeparator()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("a.ini"), QStringLiteral("[S]\nNames=a,b,c\n"));
    KeyFile kf(',');
    QVERIFY(kf.loadFile(path));
    QCOMPARE(kf.getStrList("S", "Names"),
             (QStringList{ QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") }));
}

void TestKeyFile::setKeyAndSaveRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("a.ini"), QStringLiteral("[Display]\nWidth=1920\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));

    kf.setKey("Display", "Depth", "24");
    kf.setKey("Audio", "Volume", "50");

    const QString outPath = dir.filePath(QStringLiteral("out.ini"));
    QVERIFY(kf.saveToFile(outPath));

    KeyFile reloaded;
    QVERIFY(reloaded.loadFile(outPath));
    QCOMPARE(reloaded.getStr("Display", "Depth"), QStringLiteral("24"));
    QCOMPARE(reloaded.getStr("Audio", "Volume"), QStringLiteral("50"));
    QCOMPARE(reloaded.getStr("Display", "Width"), QStringLiteral("1920"));
}

void TestKeyFile::deleteKeyRemovesEntry()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("a.ini"),
        QStringLiteral("[Display]\nWidth=1920\nHeight=1080\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));

    QVERIFY(kf.containKey("Display", "Width"));
    kf.deleteKey("Display", "Width"); // removed from the in-memory map
    QVERIFY(!kf.containKey("Display", "Width"));
    QVERIFY(kf.containKey("Display", "Height"));

    const QString outPath = dir.filePath(QStringLiteral("out.ini"));
    QVERIFY(kf.saveToFile(outPath));
    KeyFile reloaded;
    QVERIFY(reloaded.loadFile(outPath));
    QVERIFY(!reloaded.containKey("Display", "Width"));
    QCOMPARE(reloaded.getStr("Display", "Height"), QStringLiteral("1080"));
}

// ---- branch-coverage additions ----

void TestKeyFile::loadFileMissingFileReturnsFalse()
{
    KeyFile kf;
    QVERIFY(!kf.loadFile(QStringLiteral("/nonexistent_dde_svc_kf_xyz/path.ini")));
}

void TestKeyFile::loadFileEmptyFileReturnsTrueNoSections()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("empty.ini"), QString());
    KeyFile kf;
    QVERIFY(kf.loadFile(path));
    QVERIFY(kf.getMainKeys().isEmpty());
}

void TestKeyFile::loadFileSkipsCommentLine()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // '#' comment line must be skipped, not treated as a key.
    const QString path = writeIni(dir, QStringLiteral("c.ini"),
        QStringLiteral("# a comment\n[S]\nK=1\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));
    QCOMPARE(kf.getStr("S", "K"), QStringLiteral("1"));
    QVERIFY(!kf.containKey("S", "# a comment"));
}

void TestKeyFile::loadFileSkipsLineWithoutEquals()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A line that is neither a section header nor a key=value pair is ignored.
    const QString path = writeIni(dir, QStringLiteral("n.ini"),
        QStringLiteral("[S]\nnonkeyline\nK=1\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));
    QVERIFY(!kf.containKey("S", "nonkeyline"));
    QCOMPARE(kf.getStr("S", "K"), QStringLiteral("1"));
}

void TestKeyFile::loadFileKeyBeforeSectionReturnsFalse()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A key=value line appearing before any [section] is a format error.
    const QString path = writeIni(dir, QStringLiteral("e.ini"),
        QStringLiteral("key=val\n[S]\nK=1\n"));
    KeyFile kf;
    QVERIFY(!kf.loadFile(path));
}

void TestKeyFile::loadFileSectionLineWithTrailingJunkNotParsed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // "[S]x" fails the strict section test (rPos+1 != line.size()), so it is
    // NOT registered as a section; the following key falls back to the prior
    // valid section "Good".
    const QString path = writeIni(dir, QStringLiteral("j.ini"),
        QStringLiteral("[Good]\nK=1\n[S]x\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));
    QVERIFY(kf.containKey("Good", "K"));
    QVERIFY(!kf.getMainKeys().contains(QStringLiteral("S]x")));
}

void TestKeyFile::loadFileValueContainingEquals()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // '=' inside the value must be preserved (mid(index+1) keeps the rest).
    const QString path = writeIni(dir, QStringLiteral("eq.ini"),
        QStringLiteral("[S]\nKey=a=b=c\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));
    QCOMPARE(kf.getStr("S", "Key"), QStringLiteral("a=b=c"));
}

void TestKeyFile::loadFileMultipleSectionsAndMainKeys()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("m.ini"),
        QStringLiteral("[A]\nx=1\n[B]\ny=2\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));
    const QStringList mains = kf.getMainKeys();
    QVERIFY(mains.contains(QStringLiteral("A")));
    QVERIFY(mains.contains(QStringLiteral("B")));
    QCOMPARE(mains.size(), 2);
    QCOMPARE(kf.getStr("A", "x"), QStringLiteral("1"));
    QCOMPARE(kf.getStr("B", "y"), QStringLiteral("2"));
}

void TestKeyFile::getStrEmptyValueFallsBackToDefault()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("em.ini"),
        QStringLiteral("[S]\nEmpty=\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));
    // empty stored value -> getStr returns the default
    QCOMPARE(kf.getStr("S", "Empty", QStringLiteral("def")), QStringLiteral("def"));
}

void TestKeyFile::getStrListEmptyValueReturnsSingleEmptyElement()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("sl.ini"),
        QStringLiteral("[S]\nEmpty=\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));
    // QString::split on an empty string yields one empty element.
    QCOMPARE(kf.getStrList("S", "Empty"), (QStringList{ QString() }));
}

void TestKeyFile::getBoolMissingKeyDefaultFalse()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("b.ini"),
        QStringLiteral("[S]\nPresent=true\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));
    QCOMPARE(kf.getBool("S", "Present", false), true);
    // absent key inside existing section -> default kept (false here)
    QCOMPARE(kf.getBool("S", "Missing", false), false);
}

void TestKeyFile::containKeyMissingSectionReturnsFalse()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("ck.ini"),
        QStringLiteral("[S]\nK=1\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));
    QVERIFY(kf.containKey("S", "K"));
    QVERIFY(!kf.containKey("Absent", "K"));
}

void TestKeyFile::deleteKeyMissingSectionReturnsFalse()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("dk.ini"),
        QStringLiteral("[S]\nK=1\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));
    // Defect #2 (recorded): deleteKey always returns false even on success;
    // for a missing section it also returns false. Assert actual behavior.
    QCOMPARE(kf.deleteKey("Absent", "K"), false);
    // existing key still removed (verified elsewhere); ensure untouched here.
    QVERIFY(kf.containKey("S", "K"));
}

void TestKeyFile::saveToFileUnwritablePathReturnsFalse()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("u.ini"),
        QStringLiteral("[S]\nK=1\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));
    // parent directory does not exist -> QFile::open(WriteOnly) fails.
    QVERIFY(!kf.saveToFile(QStringLiteral("/nonexistent_dde_svc_parent_xyz/sub/out.ini")));
}

void TestKeyFile::printRunsLoopWithoutCrash()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeIni(dir, QStringLiteral("p.ini"),
        QStringLiteral("[S]\nK=1\nM=2\n"));
    KeyFile kf;
    QVERIFY(kf.loadFile(path));
    kf.print(); // exercises the debug iteration loop (no assertion needed)
}

QTEST_GUILESS_MAIN(TestKeyFile)

#include "tst_keyfile.moc"
