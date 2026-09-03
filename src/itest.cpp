// 集成测试：在同一进程内起两个真实 LocalSend 实例，依赖真实的 UDP 组播发现
// （224.0.0.167:53317 广播 + 双向 register），不再用 addPeerForTest 绕过。
// 验证端到端：发现 -> prepare-upload -> upload 流式落盘。
#include "localsend.h"
#include "httpserver.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <QTimer>
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
    srvA.setAutoAccept(true);   // itest 跳过确认对话框
    A.start(53317);  srvA.listen(53317);   // A：UDP+HTTP 均 53317（标准）
    B.start(53317);  srvB.listen(53318);   // B：UDP 53317（同组播），HTTP 53318

    qDebug() << "[诊断] A 本机IP =" << A.localIp() << " 组播接口 =" << A.multicastSummary();
    qDebug() << "[诊断] B 本机IP =" << B.localIp() << " 组播接口 =" << B.multicastSummary();

    QString peerA;          // B 发现到的 A 指纹
    bool sent = false;
    bool sendOk = false, recvOk = false;
    bool discovered = false;

    auto dbg = [](const QString& s){ qDebug() << "[LOG]" << s; };
    QObject::connect(&A, &LocalSend::logMessage, dbg);
    QObject::connect(&B, &LocalSend::logMessage, dbg);
    QObject::connect(&srvA, &HttpServer::logMessage, dbg);
    QObject::connect(&srvB, &HttpServer::logMessage, dbg);

    QObject::connect(&B, &LocalSend::devicesChanged, [&]() {
        for (const Device& d : B.devices()) {
            if (d.fingerprint != B.fingerprint()) {   // 排除自己
                peerA = d.fingerprint;
                discovered = true;
                qDebug() << "[发现] B 看到 A:" << d.alias << d.ip << d.port;
            }
        }
    });
    QObject::connect(&A, &LocalSend::receiveFinished,
                     [&](QString, QString, QString path) {
                         recvOk = true;
                         qDebug() << "[A] receiveFinished:" << path;
                     });
    QObject::connect(&B, &LocalSend::sendFinished,
                     [&](quint32 taskId, QString alias, bool ok, QString msg) {
                         Q_UNUSED(taskId); Q_UNUSED(alias);
                         sendOk = ok;
                         qDebug() << "[B] sendFinished:" << ok << msg;
                     });

    // 造测试文件（200KB，验证分片落盘）
    QString filePath = sendDir + "/itest.bin";
    QFile f(filePath);
    f.open(QIODevice::WriteOnly);
    QByteArray payload(200 * 1024, 'X');
    for (int i = 0; i < payload.size(); i += 997) payload[i] = char('0' + (i % 10));
    f.write(payload);
    f.close();
    qint64 origSize = QFileInfo(filePath).size();

    QList<OutgoingFile> files;
    OutgoingFile of;
    of.id = "0";
    of.fileName = "itest.bin";
    of.filePath = filePath;
    of.size = origSize;
    of.fileType = "application/octet-stream";
    files.append(of);

    B.refreshDiscovery();   // 主动广播一次，加速发现
    A.refreshDiscovery();

    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&]() {
        if (!sent && discovered) {
            qDebug() << "[测试] 通过真实发现发起发送，目标指纹" << peerA;
            B.sendFiles(peerA, files);
            sent = true;
        }
    });
    poll.start(400);

    QEventLoop loop;
    QTimer::singleShot(8000, &loop, &QEventLoop::quit);
    loop.exec();

    QString out = recvDir + "/itest.bin";
    QFile outf(out);
    bool fileMatch = outf.exists() && outf.size() == origSize;

    if (!discovered)
        qDebug() << "RESULT: FAIL | 组播发现未成功（peerA 为空）—— 检查盒上组播/回环是否可达";
    qDebug() << "RECV size=" << outf.size() << "orig=" << origSize;
    bool pass = discovered && sendOk && recvOk && fileMatch;
    qDebug() << "RESULT:" << (pass ? "PASS" : "FAIL")
             << "| discovered=" << discovered
             << "sendOk=" << sendOk << "recvOk=" << recvOk
             << "fileMatch=" << fileMatch;
    return pass ? 0 : 1;
}
