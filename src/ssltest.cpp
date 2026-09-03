// HTTPS 专项测试：验证本程序能连接「官方 LocalSend 的 https 模式」
// 官方客户端使用自签名证书，若不做 ignoreSslErrors 就会报 "SSL 握手失败"。
//
// 用法：./localsend-ssltest <对端 https 端口>
// 配套测试服务端：tools/fake_https_peer.py（自签名证书 + 极简 localsend 协议）
#include "localsend.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <QTimer>
#include <QEventLoop>
#include <QDebug>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    int peerPort = 54443;
    if (argc > 1) peerPort = QString(argv[1]).toInt();

    QTemporaryDir dir;
    QString sendDir = dir.path();
    QDir().mkpath(sendDir);

    LocalSend B;
    B.start(53390);   // 本机 UDP 端口（本用例不走组播，仅占位）

    auto dbg = [](const QString& s){ qDebug() << "[LOG]" << s; };
    QObject::connect(&B, &LocalSend::logMessage, dbg);

    // 声明为 https 的对端，且该对端真实提供 https 服务（自签名证书）
    B.addPeerForTest("securePeer", "127.0.0.1", peerPort, "SecurePeer", "https");

    QString filePath = sendDir + "/ssl.bin";
    QFile f(filePath);
    f.open(QIODevice::WriteOnly);
    QByteArray payload(128 * 1024, 'S');
    for (int i = 0; i < payload.size(); i += 331) payload[i] = char('a' + (i % 26));
    f.write(payload);
    f.close();

    QList<OutgoingFile> files;
    OutgoingFile of;
    of.id = "0";
    of.fileName = "ssl.bin";
    of.filePath = filePath;
    of.size = QFileInfo(filePath).size();
    of.fileType = "application/octet-stream";
    files.append(of);

    bool sendOk = false;
    QString sendMsg;
    QObject::connect(&B, &LocalSend::sendFinished,
                     [&](quint32, QString, bool ok, QString msg) {
                         sendOk = ok; sendMsg = msg;
                         qDebug() << "[B] sendFinished:" << ok << msg;
                     });

    QTimer::singleShot(300, [&]() { B.sendFiles("securePeer", files); });

    QEventLoop loop;
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    loop.exec();

    qDebug() << "RESULT:" << (sendOk ? "PASS" : "FAIL") << "|" << sendMsg;
    return sendOk ? 0 : 1;
}
