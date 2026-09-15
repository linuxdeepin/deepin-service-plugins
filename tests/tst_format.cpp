// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "format.h"

#include <QTest>
#include <QImage>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

// Unit test for FormatPicture::getPictureType (wallpaperslideshow plugin).
// The helper resolves a file's MIME type via QMimeDatabase and maps the
// supported image MIME names to a short type token; unsupported files yield an
// empty string. Fixtures are real images written through QImage::save so the
// MIME detection is content-based and deterministic.
//
// GUI-less: QMimeDatabase / QImage::save do not require a QGuiApplication in
// Qt6 (verified by reviewer's real run). Temp fixtures live in a QTemporaryDir
// so they are removed on scope exit even when a QVERIFY fails mid-test.
class TestFormatPicture : public QObject
{
    Q_OBJECT

private slots:
    void getPictureType_data();
    void getPictureType();
    void gifMapsToJpegByActualBehavior();
    void unknownFileReturnsEmpty();
};

void TestFormatPicture::getPictureType_data()
{
    QTest::addColumn<QString>("format");
    QTest::addColumn<QString>("expected");

    QTest::newRow("png") << "PNG" << "png";
    QTest::newRow("bmp") << "BMP" << "bmp";
    QTest::newRow("jpeg") << "JPEG" << "jpeg";
    QTest::newRow("tiff") << "TIFF" << "tiff"; // correct map: image/tiff -> "tiff"
}

void TestFormatPicture::getPictureType()
{
    QFETCH(QString, format);
    QFETCH(QString, expected);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("img"));

    QImage img(1, 1, QImage::Format_RGB32);
    img.fill(Qt::black);
    if (!img.save(path, format.toLatin1().constData()))
        QSKIP("image encoder unavailable on this platform");

    QCOMPARE(FormatPicture::getPictureType(path), expected);
    // dir removes itself + contents on destruction (RAII)
}

void TestFormatPicture::gifMapsToJpegByActualBehavior()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("img.gif"));

    // Qt cannot *write* GIF, so write the minimal GIF89a magic header that
    // QMimeDatabase matches for image/gif (the freedesktop magic only checks
    // the 6-byte "GIF89a"/"GIF87a" header at offset 0).
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(f.errorString()));
    static const unsigned char gif[] = {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, // "GIF89a"
        0x01, 0x00, 0x01, 0x00,              // 1x1 logical screen
        0x00, 0x00, 0x00,                    // no GCT, bg 0, aspect 0
        0x3B                                 // trailer
    };
    QCOMPARE(f.write(reinterpret_cast<const char *>(gif), sizeof(gif)),
             qint64(sizeof(gif)));
    f.close();

    // Defect #4 (recorded, not fixed): typeMap maps image/gif -> "jpeg".
    // Assert the *actual* behavior; flip to "gif" once the mapping is fixed.
    QCOMPARE(FormatPicture::getPictureType(path), QStringLiteral("jpeg"));
}

void TestFormatPicture::unknownFileReturnsEmpty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("not_image.txt"));

    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("not an image\n");
    f.close();

    QCOMPARE(FormatPicture::getPictureType(path), QString());
}

QTEST_GUILESS_MAIN(TestFormatPicture)

#include "tst_format.moc"
