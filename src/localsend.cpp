#include "localsend.h"
#include "logger.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QSslError>
#include <QSslCertificate>
#include <QSslKey>
#include <QNetworkInterface>
#include <QHostInfo>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QDateTime>
#include <QUuid>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QMimeDatabase>
#include <QRegExp>

// 协议常量（Localsend v2.2）
static const quint16 LS_PORT = 53317;
static const QString LS_GROUP = "224.0.0.167";
static const QString LS_VERSION = "2.0";
static const QString LS_API = "/api/localsend/v2";

// 超时常量（毫秒）
// 官方接收端会把 prepare-upload 响应挂起，直到接收者做出选择（接受/拒绝），
// 用户操作可能远超 5 秒，因此必须给足等待时间。
// 副作用：对明文端口做 TLS 握手的挂起场景也要等 30s 才回退，可接受。
static const int kPrepareTimeoutMs = 30000;   // prepare-upload 请求超时（等待对方用户确认）
static const int kUploadStallMs    = 60000;   // 上传过程中"无任何进度"的容忍时间

// ---- TLS 客户端配置 ----
// 官方 LocalSend 的 HTTPS 服务端在 TLS 握手时会要求客户端证书
// （缺证书时服务端回 tlsv13 alert certificate required）。
// 资源里内置一张自签客户端证书，所有出站 https 请求都携带它。
static QSslConfiguration tlsConfig()
{
    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setPeerVerifyMode(QSslSocket::VerifyNone);   // 对端是自签名证书
    QFile crt(":/localsend/client.crt");
    QFile key(":/localsend/client.key");
    if (crt.open(QIODevice::ReadOnly) && key.open(QIODevice::ReadOnly)) {
        QByteArray pem = crt.readAll();
        cfg.setLocalCertificateChain(QSslCertificate::fromData(pem, QSsl::Pem));
        QSslKey privKey(&key, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
        if (!privKey.isNull())
            cfg.setPrivateKey(privKey);
        qDebug() << "[TLS] 证书链:" << cfg.localCertificateChain().size()
                 << "私钥:" << (privKey.isNull() ? "解析失败" : "OK")
                 << "证书有效:" << (cfg.localCertificateChain().isEmpty()
                     ? "无" : (cfg.localCertificateChain().first().isNull() ? "空" : "OK"));
        Logger::log(QString("[TLS] 客户端证书: 链=%1 私钥=%2")
                        .arg(cfg.localCertificateChain().size())
                        .arg(privKey.isNull() ? "失败" : "OK"));
    } else {
        qDebug() << "[TLS] 警告：内置客户端证书资源缺失 crt=" << crt.exists()
                 << "key=" << key.exists();
        Logger::log("[TLS] 警告：内置客户端证书资源缺失，https 可能被对端拒绝");
    }
    return cfg;
}

// ---- 网卡选择：优先局域网网卡，避免被 VPN/隧道劫持组播 ----
static bool isLanAddress(const QHostAddress& a)
{
    if (a.protocol() != QAbstractSocket::IPv4Protocol) return false;
    quint32 ip = a.toIPv4Address();
    quint32 b = (ip >> 24) & 0xFF;
    quint32 c = (ip >> 16) & 0xFF;
    if (b == 10) return true;                          // 10.0.0.0/8
    if (b == 192 && c == 168) return true;             // 192.168.0.0/16
    if (b == 172 && c >= 16 && c <= 31) return true;   // 172.16.0.0/12
    if (b == 169 && c == 254) return true;             // 169.254.0.0/16 链路本地
    return false;
}

static bool isVirtualIface(const QString& name)
{
    return name.startsWith("tailscale") || name.startsWith("utun")
        || name.startsWith("docker") || name.startsWith("virbr")
        || name.startsWith("vboxnet") || name.startsWith("br-")
        || name.startsWith("tun") || name.startsWith("tap");
}

// 选取局域网网卡：UP + 含 IPv4 私网地址，排除回环与虚拟/隧道网卡
static QNetworkInterface pickLanInterface()
{
    QList<QNetworkInterface> all = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& i : all) {
        if (!(i.flags() & QNetworkInterface::IsUp)) continue;
        if (i.flags() & QNetworkInterface::IsLoopBack) continue;
        if (isVirtualIface(i.name())) continue;
        for (const QNetworkAddressEntry& e : i.addressEntries())
            if (isLanAddress(e.ip())) return i;
    }
    // 退化：任意 UP + 非回环 + 有 IPv4
    for (const QNetworkInterface& i : all) {
        if (!(i.flags() & QNetworkInterface::IsUp)) continue;
        if (i.flags() & QNetworkInterface::IsLoopBack) continue;
        for (const QNetworkAddressEntry& e : i.addressEntries())
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol) return i;
    }
    return QNetworkInterface();
}

LocalSend::LocalSend(QObject* parent)
    : QObject(parent)
{
    m_udp = new QUdpSocket(this);
    m_udp->setProxy(QNetworkProxy::NoProxy);   // 强制直连，避免组播套接字走 SOCKS 代理
    m_announceTimer = new QTimer(this);
    m_expireTimer = new QTimer(this);
    m_nam = new QNetworkAccessManager(this);
    m_nam->setProxy(QNetworkProxy::NoProxy);   // 出站请求同样强制直连

    m_fingerprint = QUuid::createUuid().toString(QUuid::WithoutBraces)
                        .mid(0, 16);
    m_alias = QHostInfo::localHostName();
    if (m_alias.isEmpty()) m_alias = "UOS-Device";
    m_saveDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (m_saveDir.isEmpty()) m_saveDir = QDir::homePath() + "/Downloads";
    QDir().mkpath(m_saveDir);

    connect(m_announceTimer, &QTimer::timeout, this, &LocalSend::onAnnounceTimer);
    connect(m_expireTimer,  &QTimer::timeout, this, &LocalSend::expireDevices);
}

LocalSend::~LocalSend() { stop(); }

// 统一日志出口：发 logMessage 信号。GUI 连接后由 MainWindow::onLog 统一落盘
// （此前 dbg 与 onLog 各写一次导致日志每行重复两遍，已修复）。
void LocalSend::dbg(const QString& msg)
{
    emit logMessage(msg);
}

bool LocalSend::start(quint16 port)
{
    m_port = port;

    // 组播 UDP 接收：绑定到所有 IPv4 接口
    if (!m_udp->bind(QHostAddress::AnyIPv4, m_port,
                     QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        dbg(QString("[发现] UDP 绑定端口 %1 失败: %2").arg(m_port).arg(m_udp->errorString()));
    }

    // 在所有 UP 且含 IPv4 的网卡上加入组播，确保能收到任意网卡上的发现包
    QNetworkInterface lan = pickLanInterface();
    QList<QNetworkInterface> joinList;
    QStringList joinedNames;
    if (lan.isValid()) { joinList.append(lan); joinedNames.append(lan.name()); }
    for (const QNetworkInterface& i : QNetworkInterface::allInterfaces()) {
        if (!(i.flags() & QNetworkInterface::IsUp)) continue;
        if (i.flags() & QNetworkInterface::IsLoopBack) continue;
        bool hasV4 = false;
        for (const auto& e : i.addressEntries())
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol) { hasV4 = true; break; }
        if (!hasV4) continue;
        if (!joinedNames.contains(i.name())) { joinList.append(i); joinedNames.append(i.name()); }
    }
    QStringList joined;
    for (const QNetworkInterface& i : joinList) {
        if (m_udp->joinMulticastGroup(QHostAddress(LS_GROUP), i))
            joined.append(i.name());
        else
            dbg(QString("[发现] 加入组播 %1 @ %2 失败: %3")
                          .arg(LS_GROUP).arg(i.name()).arg(m_udp->errorString()));
    }
    m_mcSummary = joined.isEmpty() ? "无" : joined.join(", ");
    dbg(QString("[发现] 组播 %1 已加入接口: %2").arg(LS_GROUP).arg(m_mcSummary));

    // 发送组播走 LAN 网卡（决定源 IP，供对端回连）
    if (lan.isValid()) m_udp->setMulticastInterface(lan);

    connect(m_udp, &QUdpSocket::readyRead, this, &LocalSend::onUdpReadyRead);

    m_announceTimer->start(3000);   // 每 3 秒广播一次
    m_expireTimer->start(5000);     // 每 5 秒清理过期设备
    sendAnnouncement();
    return true;
}

void LocalSend::stop()
{
    m_announceTimer->stop();
    m_expireTimer->stop();
    if (m_udp) {
        m_udp->leaveMulticastGroup(QHostAddress(LS_GROUP));
        m_udp->close();
    }
}

QList<Device> LocalSend::devices() const
{
    QList<Device> list;
    for (const auto& d : m_devices) list.append(d);
    return list;
}

void LocalSend::setAlias(const QString& a)
{
    if (!a.isEmpty()) m_alias = a;
    sendAnnouncement();
}

// ---------------- 发现 ----------------

void LocalSend::sendAnnouncement()
{
    QNetworkInterface lan = pickLanInterface();
    if (lan.isValid()) m_udp->setMulticastInterface(lan);
    QJsonObject obj = myInfo(true);
    obj["announce"] = true;
    QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    m_udp->writeDatagram(data, QHostAddress(LS_GROUP), LS_PORT);
}

void LocalSend::sendRegisterTo(const Device& dev)
{
    if (dev.ip.isEmpty() || dev.port == 0) return;
    QJsonObject obj = myInfo(true);
    obj["announce"] = false;
    QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QNetworkRequest req(QUrl(QString("%1://%2:%3%4/register")
                            .arg(dev.protocol).arg(dev.ip).arg(dev.port).arg(LS_API)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (dev.protocol.compare("https", Qt::CaseInsensitive) == 0)
        req.setSslConfiguration(tlsConfig());
    QNetworkReply* rep = m_nam->post(req, data);
    connect(rep, &QNetworkReply::sslErrors, this, [this, rep](const QList<QSslError>& errs) {
        rep->ignoreSslErrors();
        dbg(QString("[发现] register 忽略 %1 项 SSL 错误（自签名证书）").arg(errs.size()));
    });
    connect(rep, &QNetworkReply::finished, rep, &QNetworkReply::deleteLater);
}

void LocalSend::refreshDiscovery()
{
    sendAnnouncement();
    for (const auto& d : m_devices) sendRegisterTo(d);
}

void LocalSend::addPeerForTest(const QString& fp, const QString& ip, int port,
                              const QString& alias, const QString& protocol)
{
    Device d;
    d.fingerprint = fp;
    d.ip = ip;
    d.port = port;
    d.protocol = protocol;
    d.alias = alias;
    d.deviceType = "desktop";
    d.lastSeen = QDateTime::currentMSecsSinceEpoch();
    m_devices[fp] = d;
}

void LocalSend::onUdpReadyRead()
{
    while (m_udp->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(int(m_udp->pendingDatagramSize()));
        QHostAddress addr;
        quint16 port = 0;
        m_udp->readDatagram(data.data(), data.size(), &addr, &port);
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(data, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;
        QJsonObject obj = doc.object();
        QString fp = obj.value("fingerprint").toString();
        if (fp.isEmpty() || fp == m_fingerprint) continue;  // 忽略自己

        Device dev;
        dev.fingerprint = fp;
        dev.alias = obj.value("alias").toString(fp);
        dev.version = obj.value("version").toString();
        dev.deviceModel = obj.value("deviceModel").toString();
        dev.deviceType = obj.value("deviceType").toString("desktop");
        dev.protocol = obj.value("protocol").toString("http");
        dev.port = obj.value("port").toInt(m_port);
        dev.download = obj.value("download").toBool(false);
        dev.ip = addr.toString().replace("::ffff:", "");
        if (dev.ip.startsWith("%")) dev.ip = dev.ip.split("%").first();
        dev.lastSeen = QDateTime::currentMSecsSinceEpoch();

        bool isNew = !m_devices.contains(fp);
        bool protoChanged = !isNew && m_devices.value(fp).protocol != dev.protocol;
        m_devices[fp] = dev;

        if (isNew || protoChanged) {
            dbg(QString("[发现] 设备 %1  别名=%2  IP=%3:%4  协议=%5  型号=%6  类型=%7")
                          .arg(fp.left(8)).arg(dev.alias).arg(dev.ip).arg(dev.port)
                          .arg(dev.protocol).arg(dev.deviceModel.isEmpty() ? "-" : dev.deviceModel)
                          .arg(dev.deviceType));
        }

        // 按协议：收到 announce=true 时回 register，让对方也发现我们
        if (obj.value("announce").toBool(false))
            sendRegisterTo(dev);

        if (isNew) emit devicesChanged();
    }
}

void LocalSend::onAnnounceTimer()
{
    sendAnnouncement();
}

void LocalSend::expireDevices()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    for (auto it = m_devices.begin(); it != m_devices.end();) {
        if (now - it.value().lastSeen > 30000) {
            dbg(QString("[发现] 设备离线移除: %1 (%2)").arg(it.value().alias).arg(it.key().left(8)));
            it = m_devices.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) emit devicesChanged();
}

// ---------------- 信息/注册 ----------------

QJsonObject LocalSend::myInfo(bool includePort) const
{
    QJsonObject obj;
    obj["alias"] = m_alias;
    obj["version"] = LS_VERSION;
    obj["deviceModel"] = m_deviceModel;
    obj["deviceType"] = "desktop";
    obj["fingerprint"] = m_fingerprint;
    obj["protocol"] = "http";      // 我们只提供明文 HTTP 服务（局域网，与官方 http 模式一致）
    obj["download"] = true;
    if (includePort) obj["port"] = m_port;
    return obj;
}

QJsonObject LocalSend::handleRegister(const QJsonObject& peer, const QString& peerIp)
{
    QString fp = peer.value("fingerprint").toString();
    if (!fp.isEmpty() && fp != m_fingerprint) {
        Device dev;
        dev.fingerprint = fp;
        dev.alias = peer.value("alias").toString(fp);
        dev.version = peer.value("version").toString();
        dev.deviceModel = peer.value("deviceModel").toString();
        dev.deviceType = peer.value("deviceType").toString("desktop");
        dev.protocol = peer.value("protocol").toString("http");
        dev.port = peer.value("port").toInt(m_port);
        dev.download = peer.value("download").toBool(false);
        dev.ip = peerIp;
        dev.lastSeen = QDateTime::currentMSecsSinceEpoch();
        bool isNew = !m_devices.contains(fp);
        m_devices[fp] = dev;
        if (isNew) {
            dbg(QString("[发现] register 来自 %1 (%2) @ %3:%4 协议=%5")
                          .arg(dev.alias).arg(fp.left(8)).arg(dev.ip).arg(dev.port).arg(dev.protocol));
            emit devicesChanged();
        }
    }
    return myInfo(false);   // 响应不含 port（对端已知）
}

// ---------------- 接收（服务端侧逻辑） ----------------

QJsonObject LocalSend::handlePrepareUpload(const QJsonObject& body,
                                           const QString& peerIp,
                                           QString& sessionIdOut)
{
    QJsonObject info = body.value("info").toObject();
    QJsonObject files = body.value("files").toObject();
    if (files.isEmpty()) {
        sessionIdOut.clear();
        return QJsonObject();   // 上层需返回 400
    }
    RecvSession s;
    s.sessionId = newSessionId();
    s.peerAlias = info.value("alias").toString("未知设备");
    s.dir = m_saveDir;
    for (auto it = files.begin(); it != files.end(); ++it) {
        QJsonObject f = it.value().toObject();
        QString id = f.value("id").toString(it.key());
        s.tokens[id] = QUuid::createUuid().toString(QUuid::WithoutBraces).mid(0, 12);
        s.fileNames[id] = f.value("fileName").toString("file");
        s.fileSizes[id] = qint64(f.value("size").toDouble());
        s.previews[id] = f.value("preview").toString();   // 官方协议：文本消息内容
    }
    m_recvSessions[s.sessionId] = s;
    sessionIdOut = s.sessionId;

    dbg(QString("[收] 创建接收会话 %1  来自=%2@%3  文件数=%4")
                  .arg(s.sessionId).arg(s.peerAlias).arg(peerIp).arg(s.tokens.size()));

    emit receiveStarted(s.sessionId, s.peerAlias);

    QJsonObject resp;
    resp["sessionId"] = s.sessionId;
    QJsonObject ft;
    for (auto it = s.tokens.begin(); it != s.tokens.end(); ++it)
        ft[it.key()] = it.value();
    resp["files"] = ft;
    return resp;
}

QString LocalSend::beginUpload(const QString& sessionId, const QString& fileId,
                               const QString& token, qint64 total,
                               QString& fileNameOut)
{
    if (!m_recvSessions.contains(sessionId)) {
        dbg(QString("[收] beginUpload 拒绝：未知 session=%1").arg(sessionId));
        return QString();
    }
    RecvSession& s = m_recvSessions[sessionId];
    if (s.tokens.value(fileId) != token) {
        dbg(QString("[收] beginUpload 拒绝：token 不匹配 fileId=%1").arg(fileId));
        return QString();
    }
    if (s.received.contains(fileId)) {
        dbg(QString("[收] beginUpload 拒绝：重复上传 fileId=%1").arg(fileId));
        return QString();
    }
    fileNameOut = sanitizeFileName(s.fileNames.value(fileId));
    QDir().mkpath(s.dir);
    QString path = s.dir + "/" + fileNameOut;
    if (QFile::exists(path)) {
        // 同名冲突：加 (n)
        QFileInfo fi(path);
        int n = 1;
        QString base = fi.completeBaseName();
        QString suf = fi.suffix().isEmpty() ? "" : "." + fi.suffix();
        do {
            path = s.dir + "/" + base + QString(" (%1)").arg(++n) + suf;
        } while (QFile::exists(path));
        fileNameOut = QFileInfo(path).fileName();
    }
    s.received[fileId] = 0;
    dbg(QString("[收] 落盘开始 %1 (%2 字节) -> %3")
                  .arg(fileNameOut).arg(total).arg(path));
    return path;
}

void LocalSend::finishUpload(const QString& sessionId, const QString& fileId,
                             const QString& savedPath)
{
    if (!m_recvSessions.contains(sessionId)) return;
    RecvSession& s = m_recvSessions[sessionId];
    s.savedPaths[fileId] = savedPath;
    dbg(QString("[收] 落盘完成 %1 -> %2").arg(s.fileNames.value(fileId)).arg(savedPath));
    emit receiveFinished(sessionId, s.fileNames.value(fileId), savedPath);

    // 官方协议：preview 非空 = 文本消息
    QString preview = s.previews.value(fileId);
    if (!preview.isEmpty())
        emit messageReceived(sessionId, s.fileNames.value(fileId), preview);
}

void LocalSend::cancelSession(const QString& sessionId)
{
    m_recvSessions.remove(sessionId);
}

// 文本消息投递：消息传输不建会话、不落盘，内容来自 prepare-upload 的 preview 字段
void LocalSend::deliverMessage(const QString& peerAlias, const QString& fileName,
                               const QString& content)
{
    dbg(QString("[收] 文本消息 来自=%1  %2 字符").arg(peerAlias).arg(content.size()));
    emit messageReceived(QString(), fileName, content);
}

// ---------------- 发送（客户端侧） ----------------

quint32 LocalSend::sendFiles(const QString& fingerprint,
                             const QList<OutgoingFile>& files)
{
    quint32 taskId = ++m_taskCounter;

    if (files.isEmpty()) {
        emit sendFinished(taskId, "", false, "未选择文件");
        return taskId;
    }
    if (!m_devices.contains(fingerprint)) {
        emit sendFinished(taskId, "", false, "目标设备已离线（未在设备列表中）");
        return taskId;
    }
    Device dev = m_devices.value(fingerprint);
    if (dev.ip.isEmpty() || dev.port == 0) {
        emit sendFinished(taskId, dev.alias, false, "目标设备不可达（IP/端口缺失）");
        return taskId;
    }

    // 候选协议：优先对端声明的协议，失败后自动回退到另一种
    QStringList protos;
    if (dev.protocol.compare("https", Qt::CaseInsensitive) == 0)
        protos << "https" << "http";
    else
        protos << "http" << "https";
    if (!QSslSocket::supportsSsl()) {
        protos.removeAll("https");   // Qt 没编译进 SSL，https 必然失败
        dbg(QString("[发] 警告：Qt 无 SSL 支持，已移除 https 候选，仅尝试 %1")
                      .arg(protos.join(",")));
    }

    SendCtx ctx;
    ctx.taskId = taskId;
    ctx.dev = dev;
    ctx.files = files;
    ctx.protos = protos;
    ctx.protoIdx = 0;

    dbg(QString("[发] 任务 #%1 启动：目标=%2 (%3:%4) 声明协议=%5 文件数=%6 候选协议=%7")
                  .arg(taskId).arg(dev.alias).arg(dev.ip).arg(dev.port)
                  .arg(dev.protocol).arg(files.size()).arg(protos.join(" > ")));

    tryPrepare(ctx);
    return taskId;
}

// 用 ctx.protos[ctx.protoIdx] 发起 prepare-upload
void LocalSend::tryPrepare(const SendCtx& ctxIn)
{
    SendCtx ctx = ctxIn;
    QString proto = ctx.protos.value(ctx.protoIdx, "http");
    ctx.base = QString("%1://%2:%3").arg(proto).arg(ctx.dev.ip).arg(ctx.dev.port);

    QJsonObject info = myInfo(true);
    QJsonObject filesObj;
    for (const auto& f : ctx.files) {
        QJsonObject fo;
        fo["id"] = f.id;
        fo["fileName"] = f.fileName;
        fo["size"] = f.size;
        fo["fileType"] = f.fileType;
        fo["sha256"] = QJsonValue();
        // 官方协议：文本消息把全文放在 preview 字段，接收端按消息展示
        if (!f.preview.isEmpty())
            fo["preview"] = f.preview;
        filesObj[f.id] = fo;
    }
    QJsonObject body;
    body["info"] = info;
    body["files"] = filesObj;
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QUrl url(ctx.base + LS_API + "/prepare-upload");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setHeader(QNetworkRequest::ContentLengthHeader, payload.size());
    if (proto.compare("https", Qt::CaseInsensitive) == 0) {
        // 官方 LocalSend 的 https 模式：自签名证书 + 要求客户端证书，均在此处理
        req.setSslConfiguration(tlsConfig());
    }

    dbg(QString("[发] 任务 #%1 第 %2 次尝试 prepare-upload: %3")
                  .arg(ctx.taskId).arg(ctx.protoIdx + 1).arg(url.toString()));

    QNetworkReply* rep = m_nam->post(req, payload);
    m_sendCtx[rep] = ctx;

    // 超时保护：TLS 握手指明文 HTTP 服务时会永久挂起（对端把 ClientHello 当垃圾数据不回包），
    // 没有超时就永远等不到 finished，协议回退也就无从触发。
    QTimer* to = new QTimer(rep);            // 父对象为 rep，自动回收
    to->setSingleShot(true);
    connect(to, &QTimer::timeout, this, [this, rep, proto]() {
        if (rep->isFinished()) return;
        dbg(QString("[发] %1 请求超时（%2ms），中止并尝试回退").arg(proto).arg(kPrepareTimeoutMs));
        rep->abort();                        // 触发 finished(OperationCanceled)
    });
    to->start(kPrepareTimeoutMs);

    connect(rep, &QNetworkReply::sslErrors, this, [this, rep](const QList<QSslError>& errs) {
        rep->ignoreSslErrors();   // 自签名证书，忽略
        QStringList sl;
        for (const QSslError& e : errs) sl << e.errorString();
        dbg(QString("[发] 忽略 SSL 错误（自签名证书）: %1").arg(sl.join("; ")));
    });
    connect(rep, &QNetworkReply::finished, this, [this, rep]() { onPrepareDone(rep); });
}

// sslErrors 槽（保留用于旧式连接；实际由 lambda 处理）
void LocalSend::onSslErrors(QNetworkReply* reply, const QList<QSslError>& errors)
{
    reply->ignoreSslErrors();
    dbg(QString("[发] onSslErrors: 忽略 %1 项证书错误").arg(errors.size()));
}

void LocalSend::onPrepareDone(QNetworkReply* reply)
{
    SendCtx ctx = m_sendCtx.take(reply);
    QString err = reply->errorString();
    int errCode = int(reply->error());
    bool isHttps = reply->url().scheme().compare("https", Qt::CaseInsensitive) == 0;
    QVariant stv = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    int httpStatus = stv.isValid() ? stv.toInt() : 0;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        QString effErr = (reply->error() == QNetworkReply::OperationCanceledError)
                             ? QString("连接超时（%1 ms 无响应）").arg(kPrepareTimeoutMs)
                             : err;
        dbg(QString("[发] 任务 #%1 prepare-upload 失败（%2, code=%3, HTTP=%4）: %5")
                      .arg(ctx.taskId).arg(reply->url().scheme()).arg(errCode)
                      .arg(httpStatus).arg(effErr));

        // HTTP 层被对端明确拒绝：说明对端服务器应答了（连接没问题），
        // 不需要协议回退，直接给出可读的原因。
        if (httpStatus > 0) {
            QString reason;
            if (httpStatus == 403)
                reason = "对方拒绝或取消了本次传输（HTTP 403）";
            else if (httpStatus == 429)
                reason = "对方正忙，已有传输在进行（HTTP 429）";
            else if (httpStatus >= 400)
                reason = QString("对方返回错误（HTTP %1）").arg(httpStatus);
            if (!reason.isEmpty()) {
                finishTask(ctx.taskId, ctx.dev.alias, false, reason);
                return;
            }
        }

        // 回退到下一个候选协议（仅网络层错误才回退）
        if (ctx.protoIdx + 1 < ctx.protos.size()) {
            SendCtx next = ctx;
            next.protoIdx = ctx.protoIdx + 1;
            dbg(QString("[发] 任务 #%1 协议回退：%2 -> %3")
                          .arg(ctx.taskId)
                          .arg(ctx.protos.value(ctx.protoIdx))
                          .arg(ctx.protos.value(next.protoIdx)));
            tryPrepare(next);
            return;
        }
        finishTask(ctx.taskId, ctx.dev.alias, false,
                   QString("prepare-upload 失败(%1, code=%2): %3")
                       .arg(isHttps ? "https" : "http").arg(errCode).arg(effErr));
        return;
    }

    QByteArray raw = reply->readAll();
    QJsonObject resp = QJsonDocument::fromJson(raw).object();
    QString sessionId = resp.value("sessionId").toString();
    QJsonObject fileTokens = resp.value("files").toObject();
    if (sessionId.isEmpty()) {
        // 官方协议（send_provider.dart L354 / receive_controller.dart L533）：
        // 文本消息传输时，接收方确认后直接回 204 No Content（不建会话、不需要上传），
        // 官方发送端将 204 解读为 "Read and close" = 成功。此处保持一致。
        if (httpStatus == 204) {
            dbg(QString("[发] 任务 #%1 对方已读取并关闭消息（HTTP 204，无需上传）")
                          .arg(ctx.taskId));
            finishTask(ctx.taskId, ctx.dev.alias, true, "对方已读取并关闭消息");
            return;
        }
        dbg(QString("[发] 任务 #%1 未获得 sessionId（HTTP %2），原始响应: %3")
                      .arg(ctx.taskId).arg(httpStatus)
                      .arg(QString::fromUtf8(raw.left(300))));
        QString reason = (httpStatus >= 400)
                             ? QString("对方拒绝或取消了本次传输（HTTP %1）").arg(httpStatus)
                             : QString("对方响应异常（HTTP %1，无 sessionId）").arg(httpStatus);
        finishTask(ctx.taskId, ctx.dev.alias, false, reason);
        return;
    }

    dbg(QString("[发] 任务 #%1 prepare 成功（%2, HTTP %3）session=%4 token 数=%5")
                  .arg(ctx.taskId).arg(reply->url().scheme()).arg(httpStatus)
                  .arg(sessionId).arg(fileTokens.size()));

    QSharedPointer<SendSession> ss(new SendSession);
    ss->taskId = ctx.taskId;
    ss->deviceAlias = ctx.dev.alias;
    ss->base = ctx.base;
    ss->proto = QUrl(ctx.base).scheme();
    ss->sessionId = sessionId;
    ss->total = ctx.files.size();

    for (const auto& f : ctx.files) {
        QString token = fileTokens.value(f.id).toString();
        QString url = QString("%1%2/upload?sessionId=%3&fileId=%4&token=%5")
                          .arg(ctx.base, LS_API, sessionId, f.id, token);
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, f.fileType);
        req.setHeader(QNetworkRequest::ContentLengthHeader, f.size);  // 强制非分块，明确长度
        if (ss->proto.compare("https", Qt::CaseInsensitive) == 0)
            req.setSslConfiguration(tlsConfig());

        QNetworkReply* up;
        if (!f.content.isEmpty()) {
            // 文本消息：直接上传内联字节
            up = m_nam->post(req, f.content);
        } else {
            QFile* file = new QFile(f.filePath);
            if (!file->open(QIODevice::ReadOnly)) {
                dbg(QString("[发] 任务 #%1 无法打开文件: %2").arg(ctx.taskId).arg(f.filePath));
                ss->anyError = true;
                ss->errors.append("无法打开: " + f.filePath);
                ss->done++;
                delete file;
                continue;
            }
            dbg(QString("[发] 任务 #%1 上传 %2 (%3 字节) -> %4")
                          .arg(ctx.taskId).arg(f.fileName).arg(f.size).arg(ss->proto));
            up = m_nam->post(req, file);
            file->setParent(up);
        }

        // 上传停滞保护：只要还有字节在推进就不断重置计时器
        QTimer* stall = new QTimer(up);
        stall->setSingleShot(true);
        connect(stall, &QTimer::timeout, this, [this, up, f]() {
            if (up->isFinished()) return;
            dbg(QString("[发] 上传 %1 停滞超过 %2 ms，中止连接").arg(f.fileName).arg(kUploadStallMs));
            up->abort();
        });

        connect(up, &QNetworkReply::sslErrors, this, [this, up](const QList<QSslError>&) {
            up->ignoreSslErrors();
        });
        connect(up, &QNetworkReply::uploadProgress,
                this, [this, ctx, f, stall](qint64 sent, qint64 total) {
                    stall->start(kUploadStallMs);   // 有进度就续命
                    emit sendProgress(ctx.taskId, ctx.dev.alias, f.fileName, sent, total);
                });
        stall->start(kUploadStallMs);
        connect(up, &QNetworkReply::finished, this, [this, up, ss, f]() {
            onUploadDone(up, ss, f.fileName);
        });
    }

    if (ss->done >= ss->total && ss->total > 0)
        finishTask(ss->taskId, ss->deviceAlias, !ss->anyError,
                   ss->anyError ? ss->errors.join("; ") : "发送完成");
}

void LocalSend::onUploadDone(QNetworkReply* reply,
                             QSharedPointer<SendSession> ss,
                             const QString& fileName)
{
    if (reply->error() != QNetworkReply::NoError) {
        ss->anyError = true;
        ss->errors.append(fileName + ": " + reply->errorString());
        dbg(QString("[发] 任务 #%1 上传失败 %2 (code=%3): %4")
                      .arg(ss->taskId).arg(fileName).arg(int(reply->error()))
                      .arg(reply->errorString()));
    } else {
        dbg(QString("[发] 任务 #%1 上传成功 %2").arg(ss->taskId).arg(fileName));
    }
    reply->deleteLater();
    ss->done++;
    if (ss->done >= ss->total) {
        if (ss->anyError)
            finishTask(ss->taskId, ss->deviceAlias, false,
                       "部分文件发送失败: " + ss->errors.join("; "));
        else
            finishTask(ss->taskId, ss->deviceAlias, true, "发送完成");
    }
}

void LocalSend::finishTask(quint32 taskId, const QString& alias, bool ok, const QString& msg)
{
    dbg(QString("[发] 任务 #%1 结束：%2  %3").arg(taskId).arg(ok ? "成功" : "失败").arg(msg));
    emit sendFinished(taskId, alias, ok, msg);
}

// ---------------- 工具 ----------------

QString LocalSend::getLocalIp() const
{
    QNetworkInterface lan = pickLanInterface();
    if (lan.isValid()) {
        for (const auto& e : lan.addressEntries())
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol)
                return e.ip().toString();
    }
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags() & QNetworkInterface::IsLoopBack) continue;
        if (!(iface.flags() & QNetworkInterface::IsUp)) continue;
        for (const auto& addr : iface.addressEntries()) {
            QHostAddress a = addr.ip();
            if (a.protocol() == QAbstractSocket::IPv4Protocol)
                return a.toString();
        }
    }
    return "127.0.0.1";
}

// multicastSummary() 在头文件内联实现，返回 m_mcSummary。

QString LocalSend::newSessionId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).mid(0, 12);
}

QString LocalSend::sanitizeFileName(const QString& name) const
{
    QString s = name;
    s.replace(QRegExp("[\\\\/:*?\"<>|]"), "_");
    if (s.isEmpty()) s = "file";
    return s;
}
