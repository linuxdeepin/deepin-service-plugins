// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "utils.h"

#include <QTest>
#include <QByteArray>
#include <QString>
#include <cstdint>

// Unit test for the xsettings plugin's Utils byte-manipulation helpers.
// These are pure functions operating on QByteArray / scalar values, used to
// (de)serialise XSETTINGS wire data (little-endian integers, length-prefixed
// strings, 4-byte padding).
class TestXsUtils : public QObject
{
    Q_OBJECT

private slots:
    void getPad_data();
    void getPad();
    void readIntegerUint32();
    void readIntegerTooShort();
    void writeIntegerLittleEndian();
    void readString();
    void readStringTooShort();
    void readSkip();
    void readSkipTooShort();
    void writeString();
    void writeSkip();
    void hasXsValueForAllAlternatives();
    // --- branch-coverage additions ---
    void readIntegerUint16();
    void writeIntegerRoundTripUint32();
    void readStringZeroLength();
    void readSkipZeroLength();
};

void TestXsUtils::getPad_data()
{
    QTest::addColumn<int>("e");
    QTest::addColumn<int>("expected");

    QTest::newRow("0") << 0 << 0;
    QTest::newRow("1") << 1 << 3;
    QTest::newRow("2") << 2 << 2;
    QTest::newRow("3") << 3 << 1;
    QTest::newRow("4") << 4 << 0;
    QTest::newRow("5") << 5 << 3;
    QTest::newRow("6") << 6 << 2;
    QTest::newRow("7") << 7 << 1;
    QTest::newRow("8") << 8 << 0;
}

void TestXsUtils::getPad()
{
    QFETCH(int, e);
    QFETCH(int, expected);
    QCOMPARE(Utils::getPad(e), expected);
}

void TestXsUtils::readIntegerUint32()
{
    QByteArray arr;
    arr.append('\x01');
    arr.append('\x00');
    arr.append('\x00');
    arr.append('\x00');

    uint32_t value = 0;
    QVERIFY(Utils::readInteger(arr, value));
    QCOMPARE(value, uint32_t(1));
    QCOMPARE(arr.size(), 0);
}

void TestXsUtils::readIntegerTooShort()
{
    QByteArray arr;
    arr.append('\xff');
    arr.append('\x00'); // only 2 bytes for a uint32

    uint32_t value = 42;
    QVERIFY(!Utils::readInteger(arr, value));
    QCOMPARE(value, uint32_t(42)); // unchanged on failure
    QCOMPARE(arr.size(), 2);       // buffer unchanged on failure
}

void TestXsUtils::writeIntegerLittleEndian()
{
    QByteArray arr;
    QVERIFY(Utils::writeInteger(arr, uint32_t(1)));
    QCOMPARE(arr.size(), 4);
    QCOMPARE(uint8_t(arr.at(0)), uint8_t(0x01));
    QCOMPARE(uint8_t(arr.at(1)), uint8_t(0x00));
    QCOMPARE(uint8_t(arr.at(2)), uint8_t(0x00));
    QCOMPARE(uint8_t(arr.at(3)), uint8_t(0x00));
}

void TestXsUtils::readString()
{
    QByteArray arr("hello");

    QString value;
    QVERIFY(Utils::readString(arr, value, 3));
    QCOMPARE(value, QStringLiteral("hel"));
    QCOMPARE(arr, QByteArray("lo"));

    QVERIFY(Utils::readString(arr, value, 2));
    QCOMPARE(value, QStringLiteral("lo"));
    QCOMPARE(arr.size(), 0);
}

void TestXsUtils::readStringTooShort()
{
    QByteArray arr("hi"); // 2 bytes
    QString value = QStringLiteral("unchanged");
    QVERIFY(!Utils::readString(arr, value, 5));
    QCOMPARE(value, QStringLiteral("unchanged"));
    QCOMPARE(arr, QByteArray("hi"));
}

void TestXsUtils::readSkip()
{
    QByteArray arr("abcd");
    QVERIFY(Utils::readSkip(arr, 2));
    QCOMPARE(arr, QByteArray("cd"));
}

void TestXsUtils::readSkipTooShort()
{
    QByteArray arr("ab");
    QVERIFY(!Utils::readSkip(arr, 5));
    QCOMPARE(arr, QByteArray("ab"));
}

void TestXsUtils::writeString()
{
    QByteArray arr;
    QVERIFY(Utils::writeString(arr, QByteArray("xyz")));
    QCOMPARE(arr, QByteArray("xyz"));
}

void TestXsUtils::writeSkip()
{
    QByteArray arr;
    QVERIFY(Utils::writeSkip(arr, 3));
    QCOMPARE(arr.size(), 3);
    QCOMPARE(uint8_t(arr.at(0)), uint8_t(0));
    QCOMPARE(uint8_t(arr.at(1)), uint8_t(0));
    QCOMPARE(uint8_t(arr.at(2)), uint8_t(0));
}

void TestXsUtils::hasXsValueForAllAlternatives()
{
    // XsValue is std::variant<int, double, QString, ColorValueInfo>; a
    // default-constructed variant holds int(0), so every alternative —
    // including the default — reports "has value".
    QVERIFY(Utils::hasXsValue(XsValue(5)));
    QVERIFY(Utils::hasXsValue(XsValue(1.5)));
    QVERIFY(Utils::hasXsValue(XsValue(QStringLiteral("x"))));
    QVERIFY(Utils::hasXsValue(XsValue(ColorValueInfo{ 1, 2, 3, 4 })));
    QVERIFY(Utils::hasXsValue(XsValue{}));
}

// ---- branch-coverage additions ----

void TestXsUtils::readIntegerUint16()
{
    // Exercises the readInteger template for a 2-byte type (covers the
    // sizeof(Value)==2 code path in utils.h, complementing the uint32 case).
    QByteArray arr;
    arr.append('\x12');
    arr.append('\x34');

    uint16_t value = 0;
    QVERIFY(Utils::readInteger(arr, value));
    QCOMPARE(value, uint16_t(0x3412)); // little-endian
    QCOMPARE(arr.size(), 0);
}

void TestXsUtils::writeIntegerRoundTripUint32()
{
    // write + read round-trip covers the write path's append + the read path
    // over a non-trivial value.
    QByteArray arr;
    QVERIFY(Utils::writeInteger(arr, uint32_t(0x12345678)));
    QCOMPARE(arr.size(), 4);
    uint32_t back = 0;
    QVERIFY(Utils::readInteger(arr, back));
    QCOMPARE(back, uint32_t(0x12345678));
    QVERIFY(arr.isEmpty());
}

void TestXsUtils::readStringZeroLength()
{
    QByteArray arr("abc");
    QString value = QStringLiteral("unchanged");
    QVERIFY(Utils::readString(arr, value, 0));
    QCOMPARE(value, QString());        // left(0) == empty
    QCOMPARE(arr, QByteArray("abc"));  // nothing consumed
}

void TestXsUtils::readSkipZeroLength()
{
    QByteArray arr("abc");
    QVERIFY(Utils::readSkip(arr, 0));
    QCOMPARE(arr, QByteArray("abc"));  // nothing removed
}

QTEST_GUILESS_MAIN(TestXsUtils)

#include "tst_xsutils.moc"
