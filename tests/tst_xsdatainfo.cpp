// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "xsdatainfo.h"

#include <QTest>
#include <QByteArray>
#include <QSharedPointer>
#include <QString>
#include <variant>
#include <cstdint>

// Unit test for xsettings XSItemInfo / XSDataInfo — the XSETTINGS wire
// marshal/unmarshal layer (src/plugin-qt/xsettings/impl/xsdatainfo.cpp).
// These are pure byte-serialization classes with no external (D-Bus / DConfig /
// XCB) dependencies; they build on the already-tested Utils byte helpers.
//
// Core strategy: round-trip. Construct an XSItemInfo from (prop, XsValue),
// marshal it to a QByteArray, unmarshal a fresh XSItemInfo from those bytes,
// and verify the name/value survive. This exercises both write and read paths
// for every supported type (Integer / String / Color). Additional slots cover
// type-mismatch marshal failures (via modifyProperty), the default/unknown-type
// no-op branch, the double-variant constructor's else branch, XSDataInfo
// collection operations, and the free function xsValueToString.
class TestXSDataInfo : public QObject
{
    Q_OBJECT

private slots:
    // --- XSItemInfo construction + round-trip ---
    void integerConstructorAndRoundTrip();
    void stringConstructorAndRoundTrip();
    void colorConstructorAndRoundTrip();
    void doubleConstructorHitsElseBranch();
    void modifyPropertyUpdatesValueAndSerial();

    // --- XSItemInfo marshal failure branches ---
    void marshalTypeMismatchIntToStringReturnsFalse();
    void marshalTypeMismatchStringToIntReturnsFalse();
    void marshalTypeMismatchColorToIntReturnsFalse();
    void unMarshalUnknownTypeIsNoOp();
    void marshalUnknownTypeReturnsFalse();

    // --- XSDataInfo ---
    void emptyDataUnmarshalZerosAllFields();
    void nonEmptyZeroItemsUnmarshalPreservesHeader();
    void insertAndListProps();
    void getPropItemFound();
    void getPropItemNotFoundReturnsNull();
    void increaseSerialAndNumSettings();
    void fullRoundTripMultipleItems();
    void marshalFailureReturnsEmpty();

    // --- xsValueToString ---
    void xsValueToStringInteger();
    void xsValueToStringString();
    void xsValueToStringColor();
    void xsValueToStringTypeMismatchNullptr();
    void xsValueToStringUnknownType();
};

// ---- XSItemInfo construction + round-trip ----

void TestXSDataInfo::integerConstructorAndRoundTrip()
{
    XSItemInfo src(QStringLiteral("Dpi"), XsValue(96));
    QCOMPARE(src.getHeadName(), QStringLiteral("Dpi"));

    QByteArray bytes;
    QVERIFY(src.marshalXSItemInfoData(bytes));
    QVERIFY(!bytes.isEmpty());

    XSItemInfo dst(bytes); // unmarshal
    QCOMPARE(dst.getHeadName(), QStringLiteral("Dpi"));
    const XsValue v = dst.getValue();
    const auto *ival = std::get_if<int>(&v);
    QVERIFY(ival != nullptr);
    QCOMPARE(*ival, 96);
}

void TestXSDataInfo::stringConstructorAndRoundTrip()
{
    XSItemInfo src(QStringLiteral("Name"), XsValue(QStringLiteral("hello")));
    QCOMPARE(src.getHeadName(), QStringLiteral("Name"));

    QByteArray bytes;
    QVERIFY(src.marshalXSItemInfoData(bytes));

    XSItemInfo dst(bytes);
    QCOMPARE(dst.getHeadName(), QStringLiteral("Name"));
    const XsValue v = dst.getValue();
    const auto *sval = std::get_if<QString>(&v);
    QVERIFY(sval != nullptr);
    QCOMPARE(*sval, QStringLiteral("hello"));
}

void TestXSDataInfo::colorConstructorAndRoundTrip()
{
    const ColorValueInfo color{ 10, 20, 30, 40 };
    XSItemInfo src(QStringLiteral("Color"), XsValue(color));
    QCOMPARE(src.getHeadName(), QStringLiteral("Color"));

    QByteArray bytes;
    QVERIFY(src.marshalXSItemInfoData(bytes));

    XSItemInfo dst(bytes);
    QCOMPARE(dst.getHeadName(), QStringLiteral("Color"));
    const XsValue v = dst.getValue();
    const auto *cval = std::get_if<ColorValueInfo>(&v);
    QVERIFY(cval != nullptr);
    QCOMPARE((*cval)[0], uint16_t(10));
    QCOMPARE((*cval)[1], uint16_t(20));
    QCOMPARE((*cval)[2], uint16_t(30));
    QCOMPARE((*cval)[3], uint16_t(40));
}

void TestXSDataInfo::doubleConstructorHitsElseBranch()
{
    // XsValue holds a double, which matches none of int/QString/ColorValueInfo
    // in the (prop, value) constructor → falls to the else branch (qDebug, no
    // init). head.name stays default-constructed (empty QString).
    XSItemInfo src(QStringLiteral("Ignored"), XsValue(1.5));
    QCOMPARE(src.getHeadName(), QString()); // not initialized by any init path
}

void TestXSDataInfo::modifyPropertyUpdatesValueAndSerial()
{
    XSItemInfo src(QStringLiteral("Dpi"), XsValue(96));
    // modifyProperty increments lastChangeSerial and replaces value.
    XsSetting setting;
    setting.type = HeadTypeInteger;
    setting.prop = QStringLiteral("Dpi");
    setting.value = XsValue(144);
    src.modifyProperty(setting);

    const XsValue v = src.getValue();
    const auto *ival = std::get_if<int>(&v);
    QVERIFY(ival != nullptr);
    QCOMPARE(*ival, 144);

    // Round-trip survives the modify (header type still Integer, value is int).
    QByteArray bytes;
    QVERIFY(src.marshalXSItemInfoData(bytes));
    XSItemInfo dst(bytes);
    QCOMPARE(dst.getHeadName(), QStringLiteral("Dpi"));
    const XsValue v2 = dst.getValue();
    const auto *ival2 = std::get_if<int>(&v2);
    QVERIFY(ival2 != nullptr);
    QCOMPARE(*ival2, 144);
}

// ---- XSItemInfo marshal failure branches ----

void TestXSDataInfo::marshalTypeMismatchIntToStringReturnsFalse()
{
    // Create an Integer item, then change value to a QString via modifyProperty.
    // head.type is still HeadTypeInteger but value holds QString →
    // std::get_if<int> returns nullptr → break → marshal returns false.
    XSItemInfo item(QStringLiteral("Dpi"), XsValue(96));
    XsSetting setting;
    setting.type = HeadTypeString;
    setting.value = XsValue(QStringLiteral("mismatch"));
    item.modifyProperty(setting);

    QByteArray out;
    QVERIFY(!item.marshalXSItemInfoData(out)); // header written, but ret=false
}

void TestXSDataInfo::marshalTypeMismatchStringToIntReturnsFalse()
{
    XSItemInfo item(QStringLiteral("Name"), XsValue(QStringLiteral("hi")));
    XsSetting setting;
    setting.type = HeadTypeInteger;
    setting.value = XsValue(42);
    item.modifyProperty(setting);

    QByteArray out;
    QVERIFY(!item.marshalXSItemInfoData(out)); // str==nullptr → break → false
}

void TestXSDataInfo::marshalTypeMismatchColorToIntReturnsFalse()
{
    XSItemInfo item(QStringLiteral("Color"), XsValue(ColorValueInfo{ 1, 2, 3, 4 }));
    XsSetting setting;
    setting.type = HeadTypeColor;
    setting.value = XsValue(42);
    item.modifyProperty(setting);

    QByteArray out;
    QVERIFY(!item.marshalXSItemInfoData(out)); // colorValue==nullptr → break → false
}

void TestXSDataInfo::unMarshalUnknownTypeIsNoOp()
{
    // Marshal a valid integer item, corrupt the type byte to an unknown value,
    // then unmarshal — the default branch in unMarshalXSItemInfoData is a no-op
    // (value stays default-constructed).
    XSItemInfo src(QStringLiteral("X"), XsValue(1));
    QByteArray bytes;
    QVERIFY(src.marshalXSItemInfoData(bytes));
    QCOMPARE(uint8_t(bytes.at(0)), uint8_t(HeadTypeInteger));

    bytes[0] = static_cast<char>(99); // unknown type
    XSItemInfo item(bytes); // unmarshal; default branch no-op
    QCOMPARE(item.getHeadName(), QStringLiteral("X"));
}

void TestXSDataInfo::marshalUnknownTypeReturnsFalse()
{
    // Build on the unknown-type item from above: marshalling it hits the
    // default branch in marshalXSItemInfoData → ret stays false.
    XSItemInfo src(QStringLiteral("X"), XsValue(1));
    QByteArray tmp;
    QVERIFY(src.marshalXSItemInfoData(tmp));
    tmp[0] = static_cast<char>(99);
    XSItemInfo item(tmp); // head.type is now 99

    QByteArray out;
    QVERIFY(!item.marshalXSItemInfoData(out));
}

// ---- XSDataInfo ----

void TestXSDataInfo::emptyDataUnmarshalZerosAllFields()
{
    QByteArray empty;
    XSDataInfo data(empty); // datas.isEmpty() → zeros, return

    // Marshal back: header should be 12 zero bytes (byteOrder=0, pad=3, serial=0, numSettings=0).
    QByteArray marshaled = data.marshalSettingData();
    QCOMPARE(marshaled.size(), 12);
    for (int i = 0; i < marshaled.size(); i++)
        QCOMPARE(uint8_t(marshaled.at(i)), uint8_t(0));
    QCOMPARE(data.listProps(), QStringLiteral("[]"));
}

void TestXSDataInfo::nonEmptyZeroItemsUnmarshalPreservesHeader()
{
    // Marshal an empty XSDataInfo (12-byte header, numSettings=0), then
    // unmarshal from non-empty data → covers the !isEmpty + loop-not-entered path.
    QByteArray empty;
    XSDataInfo src(empty);
    src.increaseSerial(); // serial becomes 1
    QByteArray marshaled = src.marshalSettingData();
    QVERIFY(!marshaled.isEmpty());
    QCOMPARE(marshaled.size(), 12);

    XSDataInfo dst(marshaled);
    QCOMPARE(dst.listProps(), QStringLiteral("[]"));
    // serial=1 should survive the round-trip; verify by re-marshalling and
    // checking the serial field (bytes 4-7, little-endian uint32).
    QByteArray remarshaled = dst.marshalSettingData();
    uint32_t serial = uint8_t(remarshaled.at(4))
                      | (uint32_t(uint8_t(remarshaled.at(5))) << 8)
                      | (uint32_t(uint8_t(remarshaled.at(6))) << 16)
                      | (uint32_t(uint8_t(remarshaled.at(7))) << 24);
    QCOMPARE(serial, uint32_t(1));
}

void TestXSDataInfo::insertAndListProps()
{
    QByteArray empty;
    XSDataInfo data(empty);

    auto item1 = QSharedPointer<XSItemInfo>(new XSItemInfo(QStringLiteral("Dpi"), XsValue(96)));
    auto item2 = QSharedPointer<XSItemInfo>(new XSItemInfo(QStringLiteral("Name"), XsValue(QStringLiteral("x"))));
    data.inserItem(item1);
    data.inserItem(item2);

    QCOMPARE(data.listProps(), QStringLiteral("[\"Dpi\",\"Name\"]"));
}

void TestXSDataInfo::getPropItemFound()
{
    QByteArray empty;
    XSDataInfo data(empty);
    auto item = QSharedPointer<XSItemInfo>(new XSItemInfo(QStringLiteral("Dpi"), XsValue(96)));
    data.inserItem(item);

    QSharedPointer<XSItemInfo> found = data.getPropItem(QStringLiteral("Dpi"));
    QVERIFY(found != nullptr);
    QCOMPARE(found->getHeadName(), QStringLiteral("Dpi"));
}

void TestXSDataInfo::getPropItemNotFoundReturnsNull()
{
    QByteArray empty;
    XSDataInfo data(empty);
    auto item = QSharedPointer<XSItemInfo>(new XSItemInfo(QStringLiteral("Dpi"), XsValue(96)));
    data.inserItem(item);

    QVERIFY(data.getPropItem(QStringLiteral("Absent")) == nullptr);
}

void TestXSDataInfo::increaseSerialAndNumSettings()
{
    QByteArray empty;
    XSDataInfo data(empty);
    data.increaseSerial();
    data.increaseSerial();
    data.increaseNumSettings();
    data.increaseNumSettings();
    data.increaseNumSettings();

    // Verify via re-marshal: serial=2 at bytes 4-7, numSettings=3 at bytes 8-11.
    QByteArray marshaled = data.marshalSettingData();
    uint32_t serial = uint8_t(marshaled.at(4))
                      | (uint32_t(uint8_t(marshaled.at(5))) << 8)
                      | (uint32_t(uint8_t(marshaled.at(6))) << 16)
                      | (uint32_t(uint8_t(marshaled.at(7))) << 24);
    uint32_t numSettings = uint8_t(marshaled.at(8))
                           | (uint32_t(uint8_t(marshaled.at(9))) << 8)
                           | (uint32_t(uint8_t(marshaled.at(10))) << 16)
                           | (uint32_t(uint8_t(marshaled.at(11))) << 24);
    QCOMPARE(serial, uint32_t(2));
    QCOMPARE(numSettings, uint32_t(3));
}

void TestXSDataInfo::fullRoundTripMultipleItems()
{
    // Build a setting with 3 items of different types, marshal, unmarshal, verify.
    QByteArray empty;
    XSDataInfo src(empty);
    src.increaseSerial();

    auto itemInt = QSharedPointer<XSItemInfo>(new XSItemInfo(QStringLiteral("Dpi"), XsValue(96)));
    auto itemStr = QSharedPointer<XSItemInfo>(new XSItemInfo(QStringLiteral("Name"), XsValue(QStringLiteral("hello"))));
    auto itemColor = QSharedPointer<XSItemInfo>(new XSItemInfo(QStringLiteral("Color"), XsValue(ColorValueInfo{ 10, 20, 30, 40 })));
    src.inserItem(itemInt);
    src.inserItem(itemStr);
    src.inserItem(itemColor);
    src.increaseNumSettings();
    src.increaseNumSettings();
    src.increaseNumSettings();

    QByteArray marshaled = src.marshalSettingData();
    QVERIFY(!marshaled.isEmpty());

    XSDataInfo dst(marshaled);
    QCOMPARE(dst.listProps(), QStringLiteral("[\"Dpi\",\"Name\",\"Color\"]"));

    // Verify each item's value survived the round-trip.
    auto di = dst.getPropItem(QStringLiteral("Dpi"));
    QVERIFY(di != nullptr);
    const XsValue vi = di->getValue();
    QCOMPARE(*std::get_if<int>(&vi), 96);

    auto dn = dst.getPropItem(QStringLiteral("Name"));
    QVERIFY(dn != nullptr);
    const XsValue vn = dn->getValue();
    QCOMPARE(*std::get_if<QString>(&vn), QStringLiteral("hello"));

    auto dc = dst.getPropItem(QStringLiteral("Color"));
    QVERIFY(dc != nullptr);
    const XsValue vc = dc->getValue();
    const auto *cval = std::get_if<ColorValueInfo>(&vc);
    QVERIFY(cval != nullptr);
    QCOMPARE((*cval)[0], uint16_t(10));
    QCOMPARE((*cval)[3], uint16_t(40));
}

void TestXSDataInfo::marshalFailureReturnsEmpty()
{
    // Insert an item whose marshal will fail (type mismatch via modifyProperty).
    // marshalSettingData should detect the failure and return an empty QByteArray.
    QByteArray empty;
    XSDataInfo data(empty);

    auto badItem = QSharedPointer<XSItemInfo>(new XSItemInfo(QStringLiteral("Dpi"), XsValue(96)));
    XsSetting setting;
    setting.value = XsValue(QStringLiteral("mismatch")); // int→string mismatch
    badItem->modifyProperty(setting);
    data.inserItem(badItem);
    data.increaseNumSettings();

    QByteArray marshaled = data.marshalSettingData();
    QVERIFY(marshaled.isEmpty()); // marshalXSItemInfoData returned false → return {}
}

// ---- xsValueToString ----

void TestXSDataInfo::xsValueToStringInteger()
{
    XsValue v(42);
    QCOMPARE(xsValueToString(v, HeadTypeInteger), QStringLiteral("42"));
}

void TestXSDataInfo::xsValueToStringString()
{
    XsValue v(QStringLiteral("hi"));
    QCOMPARE(xsValueToString(v, HeadTypeString), QStringLiteral("hi"));
}

void TestXSDataInfo::xsValueToStringColor()
{
    XsValue v(ColorValueInfo{ 1, 2, 3, 4 });
    QCOMPARE(xsValueToString(v, HeadTypeColor), QStringLiteral("1,2,3,4"));
}

void TestXSDataInfo::xsValueToStringTypeMismatchNullptr()
{
    // value holds int, but type asked is String → get_if<QString>==nullptr → "nullptr"
    XsValue vInt(42);
    QCOMPARE(xsValueToString(vInt, HeadTypeString), QStringLiteral("nullptr"));

    // value holds int, type asked is Color → get_if<Color>==nullptr → "nullptr"
    QCOMPARE(xsValueToString(vInt, HeadTypeColor), QStringLiteral("nullptr"));

    // value holds QString, type asked is Integer → get_if<int>==nullptr → break → ""
    XsValue vStr(QStringLiteral("x"));
    QCOMPARE(xsValueToString(vStr, HeadTypeInteger), QString());

    // value holds Color, type asked is Integer → get_if<int>==nullptr → break → ""
    XsValue vColor(ColorValueInfo{ 1, 2, 3, 4 });
    QCOMPARE(xsValueToString(vColor, HeadTypeInteger), QString());
}

void TestXSDataInfo::xsValueToStringUnknownType()
{
    XsValue v(42);
    QCOMPARE(xsValueToString(v, 99), QStringLiteral("unknown"));
}

QTEST_GUILESS_MAIN(TestXSDataInfo)

#include "tst_xsdatainfo.moc"
