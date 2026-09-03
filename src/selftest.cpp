// 无界面自测：在同一进程内起两个 LocalSend + HttpServer，
// 通过回环地址 127.0.0.1 真实跑一遍 Localsend 协议（prepare-upload -> upload 流式落盘）。
// 验证核心传输链路，不依赖显示器。
#include "localsend.h"
#include "httpserver.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QDebug>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    QString recvDir = dir.path() + "/recv";
    QString sendDir = dir.path() + "/send";
    QDir().mkpath(recvDir);
    QDir().mkpath(sendDir);

    LocalSend A, B;
    HttpServer srvA(&A), srvB(&B);
    A.setSaveDir(recvDir);
    srvA.setAutoAccept(true);   // selftest 跳过确认对话框
    A.start(53317);
    qDebug() << "listenA(53317) =" << srvA.listen(53317);
    B.start(53318);
    qDebug() << "listenB(53318) =" << srvB.listen(53318);

    // 转发诊断日志
    auto dbg = [](const QString& s){ qDebug() << "[LOG]" << s; };
    QObject::connect(&A, &LocalSend::logMessage, dbg);
    QObject::connect(&B, &LocalSend::logMessage, dbg);
    QObject::connect(&srvA, &HttpServer::logMessage, dbg);
    QObject::connect(&srvB, &HttpServer::logMessage, dbg);

    // B 认识 A（回环地址，绕过组播发现）
    B.addPeerForTest("peerA", "127.0.0.1", 53317, "A");

    // 造测试文件（较大，模拟多段流式到达，验证分片落盘与长度判定）
    QString filePath = sendDir + "/hello.bin";
    QFile f(filePath);
    f.open(QIODevice::WriteOnly);
    QByteArray payload(200 * 1024, 'X');
    for (int i = 0; i < payload.size(); i += 997) payload[i] = char('0' + (i % 10));  // 非全同，便于校验
    f.write(payload);
    f.close();
    qint64 origSize = QFileInfo(filePath).size();

    QList<OutgoingFile> files;
    OutgoingFile of;
    of.id = "0";
    of.fileName = "hello.bin";
    of.filePath = filePath;
    of.size = origSize;
    of.fileType = "text/plain";
    files.append(of);

    QEventLoop loop;
    bool sendOk = false, recvOk = false;
    quint32 taskId1 = 0;
    QObject::connect(&B, &LocalSend::sendFinished,
                     [&](quint32 taskId, QString alias, bool ok, QString msg) {
        Q_UNUSED(alias);
        if (taskId != taskId1) return;    // 只认本阶段的任务，避免后续阶段的信号串扰
        sendOk = ok;
        qDebug() << "[B] sendFinished:" << ok << msg;
    });
    QObject::connect(&A, &LocalSend::receiveFinished, [&](QString, QString, QString path) {
        recvOk = true;
        qDebug() << "[A] receiveFinished:" << path;
    });

    QTimer::singleShot(300, [&]() { taskId1 = B.sendFiles("peerA", files); });
    QTimer::singleShot(8000, &loop, &QEventLoop::quit);
    loop.exec();

    QString out = recvDir + "/hello.bin";
    QFile outf(out);
    bool fileMatch = outf.exists() && outf.size() == origSize;
    qDebug() << "RECV size=" << outf.size() << "orig=" << origSize;

    // ================= 阶段 2：协议回退 =================
    // 对端被声明为 https，但实际只提供明文 http 服务（模拟部分官方客户端的行为）。
    // 期望：https 尝试失败 → 自动回退 http → 传输仍然成功。
    qDebug() << "\n---- 阶段 2：https 声明 + http 实际服务，验证协议回退 ----";
    bool fbSendOk = false, fbRecvOk = false;
    B.addPeerForTest("peerA_https", "127.0.0.1", 53317, "A-https", "https");

    QEventLoop loop2;
    quint32 taskId2 = 0;
    QObject::connect(&B, &LocalSend::sendFinished,
                     [&](quint32 taskId, QString, bool ok, QString msg) {
                         if (taskId != taskId2) return;
                         fbSendOk = ok;
                         qDebug() << "[B] 回退测试 sendFinished:" << ok << msg;
                     });
    QObject::connect(&A, &LocalSend::receiveFinished,
                     [&](QString, QString, QString path) {
                         if (path.endsWith("hello2.bin")) fbRecvOk = true;
                     });

    QString filePath2 = sendDir + "/hello2.bin";
    QFile f2(filePath2);
    f2.open(QIODevice::WriteOnly);
    f2.write(QByteArray(64 * 1024, 'Z'));
    f2.close();
    QList<OutgoingFile> files2;
    OutgoingFile of2;
    of2.id = "0";
    of2.fileName = "hello2.bin";
    of2.filePath = filePath2;
    of2.size = QFileInfo(filePath2).size();
    of2.fileType = "application/octet-stream";
    files2.append(of2);

    QTimer::singleShot(300, [&]() { taskId2 = B.sendFiles("peerA_https", files2); });
    QTimer::singleShot(40000, &loop2, &QEventLoop::quit);
    loop2.exec();

    QFile outf2(recvDir + "/hello2.bin");
    bool fbFileMatch = outf2.exists() && outf2.size() == QFileInfo(filePath2).size();
    qDebug() << "RECV2 size=" << outf2.size() << "orig=" << QFileInfo(filePath2).size();

    // ================= 阶段 3：文本消息（官方协议 preview 字段） =================
    // 文本不落盘为业务文件，而是 fileName=UUID.txt + preview=全文；
    // 接收端应触发 messageReceived 且内容一致。
    qDebug() << "\n---- 阶段 3：文本消息（preview 字段） ----";
    bool msgSendOk = false, msgRecvOk = false;
    QString receivedMsg;
    B.addPeerForTest("peerA_msg", "127.0.0.1", 53317, "A-msg", "http");

    QList<OutgoingFile> files3;
    OutgoingFile msg;
    msg.id = "0";
    msg.fileName = QUuid::createUuid().toString(QUuid::WithoutBraces) + ".txt";
    msg.size = 12;
    msg.fileType = "text/plain";
    msg.preview = QString::fromUtf8("你好 LocalSend");   // 官方协议：全文放 preview
    msg.content = msg.preview.toUtf8();
    msg.size = msg.content.size();
    files3.append(msg);

    QEventLoop loop3;
    quint32 taskId3 = 0;
    QObject::connect(&B, &LocalSend::sendFinished,
                     [&](quint32 taskId, QString, bool ok, QString) {
                         if (taskId != taskId3) return;
                         msgSendOk = ok;
                     });
    QObject::connect(&A, &LocalSend::messageReceived,
                     [&](QString, QString, QString content) {
                         receivedMsg = content;
                         msgRecvOk = true;
                     });

    QTimer::singleShot(300, [&]() { taskId3 = B.sendFiles("peerA_msg", files3); });
    QTimer::singleShot(8000, &loop3, &QEventLoop::quit);
    loop3.exec();

    bool msgMatch = (receivedMsg == msg.preview);
    qDebug() << "MSG received=" << receivedMsg << "expected=" << msg.preview;

    bool pass = sendOk && recvOk && fileMatch && fbSendOk && fbRecvOk && fbFileMatch
                && msgSendOk && msgRecvOk && msgMatch;
    qDebug() << "RESULT:" << (pass ? "PASS" : "FAIL")
             << "| 阶段1: sendOk=" << sendOk << "recvOk=" << recvOk << "fileMatch=" << fileMatch
             << "| 阶段2(回退): sendOk=" << fbSendOk << "recvOk=" << fbRecvOk
             << "fileMatch=" << fbFileMatch
             << "| 阶段3(消息): sendOk=" << msgSendOk << "recvOk=" << msgRecvOk
             << "match=" << msgMatch;
    return pass ? 0 : 1;
}
