#include "httpserver.h"
#include "localsend.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QUrl>
#include <QNetworkProxy>

HttpServer::HttpServer(LocalSend* core, QObject* parent)
    : QObject(parent), m_core(core)
{
    m_server = new QTcpServer(this);
    m_server->setProxy(QNetworkProxy::NoProxy);   // 强制直连，避免监听走 SOCKS 代理被 BIND 拒绝
    connect(m_server, &QTcpServer::newConnection, this, &HttpServer::onNewConnection);
}

bool HttpServer::listen(quint16 port)
{
    return m_server->listen(QHostAddress::Any, port);
}

quint16 HttpServer::port() const
{
    return m_server->serverPort();
}

bool HttpServer::isListening() const
{
    return m_server->isListening();
}

void HttpServer::close()
{
    m_server->close();
}

QString HttpServer::errorString() const
{
    return m_server->errorString();
}

void HttpServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket* sock = m_server->nextPendingConnection();
        Conn* c = new Conn;
        m_conns[sock] = c;
        connect(sock, &QTcpSocket::readyRead, this, &HttpServer::onReadyRead);
        connect(sock, &QTcpSocket::disconnected, this, &HttpServer::onDisconnected);
    }
}

void HttpServer::onReadyRead()
{
    QTcpSocket* sock = qobject_cast<QTcpSocket*>(sender());
    if (!sock) return;
    Conn* c = m_conns.value(sock);
    if (!c) return;

    if (!c->headerDone) {
        c->buf.append(sock->readAll());

        // 二进制垃圾检测：对明文端口发起的 TLS 握手（ClientHello 以 0x16 开头）
        // 永远不可能成为合法 HTTP 请求。立即 400 拒绝，让对方客户端快速走协议回退，
        // 而不是等对方超时（否则对方会挂起 30s）。
        if (!c->buf.isEmpty()) {
            unsigned char b0 = static_cast<unsigned char>(c->buf.at(0));
            bool methodStart = (b0 >= 'A' && b0 <= 'Z');   // HTTP 方法必以大写字母开头
            if (!methodStart) {
                sendStatus(sock, 400, "Bad Request");
                sock->disconnectFromHost();
                return;
            }
        }

        int idx = c->buf.indexOf("\r\n\r\n");
        if (idx < 0) return;                       // 头部未收全
        QByteArray head = c->buf.left(idx);
        QByteArray bodySoFar = c->buf.mid(idx + 4);
        parseHeaders(c, head);
        c->headerDone = true;
        c->isUpload = (c->method == "POST" && c->path.startsWith("/api/localsend/v2/upload"));
        if (c->isUpload && !c->chunked)
            c->total = c->contentLength;   // 完成判定用的总长度（chunked 时未知，保持 0）

        if (c->isUpload) {
            QString path = m_core->beginUpload(c->query.value("sessionId"),
                                              c->query.value("fileId"),
                                              c->query.value("token"),
                                              c->total, c->fileName);
            if (path.isEmpty()) {
                sendStatus(sock, 403, "Invalid token or session");
                sock->disconnectFromHost();
                return;
            }
            c->out = new QFile(path, this);
            if (!c->out->open(QIODevice::WriteOnly)) {
                sendStatus(sock, 500, "Cannot write file");
                sock->disconnectFromHost();
                return;
            }
            emit logMessage(QString("[收] 开始接收 %1 (total=%2 chunked=%3) -> %4")
                            .arg(c->fileName).arg(c->total).arg(c->chunked).arg(path));
            if (!bodySoFar.isEmpty())
                handleUploadData(c, sock, bodySoFar);
            else if (c->chunked)
                processChunked(c, sock);   // body 可能在 header 包内已含分块头
            return;
        }
        // 非上传：c->buf 仍保留 head+"\r\n\r\n"+body，供下方完成判定定位头分隔符
    } else if (c->isUpload && c->out) {
        handleUploadData(c, sock, sock->readAll());
        return;
    } else {
        // 非上传请求，继续累积 body
        c->buf.append(sock->readAll());
    }

    if (!c->isUpload) {
        int headerEnd = c->buf.indexOf("\r\n\r\n");
        if (headerEnd >= 0 && c->buf.size() - headerEnd - 4 >= c->contentLength) {
            processComplete(c, sock);
        }
    }
}

void HttpServer::handleUploadData(Conn* c, QTcpSocket* sock, const QByteArray& data)
{
    if (c->chunked) {
        if (!data.isEmpty()) c->raw.append(data);
        processChunked(c, sock);
        return;
    }
    if (!data.isEmpty()) {
        c->out->write(data);
        c->bodyReceived += data.size();
        emit m_core->receiveProgress(c->query.value("sessionId"),
                                     c->fileName, c->bodyReceived, c->total);
    }
    if (c->bodyReceived >= c->total) finishConn(c, sock);
}

// 解析 HTTP chunked 传输编码（无 Content-Length 时），直到收到 "0\r\n\r\n"
void HttpServer::processChunked(Conn* c, QTcpSocket* sock)
{
    while (true) {
        if (c->chunkState == 0) {                 // 解析长度行 "<hex>\r\n"
            int nl = c->raw.indexOf("\r\n");
            if (nl < 0) return;
            QByteArray line = c->raw.left(nl).trimmed();
            c->raw = c->raw.mid(nl + 2);
            bool ok = false;
            qint64 size = line.toLongLong(&ok, 16);
            if (!ok) continue;                    // 空行 / trailer，跳过
            if (size == 0) {                      // 0 长度块 = 传输结束
                finishConn(c, sock);
                return;
            }
            c->chunkRemain = size;
            c->chunkState = 1;
        } else {                                  // 收 chunk 数据
            if (c->raw.size() < c->chunkRemain) return;
            QByteArray data = c->raw.left(c->chunkRemain);
            c->out->write(data);
            c->bodyReceived += data.size();
            emit m_core->receiveProgress(c->query.value("sessionId"),
                                         c->fileName, c->bodyReceived, 0);
            c->raw = c->raw.mid(c->chunkRemain);
            if (c->raw.startsWith("\r\n")) c->raw = c->raw.mid(2);
            c->chunkRemain = 0;
            c->chunkState = 0;
        }
    }
}

void HttpServer::finishConn(Conn* c, QTcpSocket* sock)
{
    emit logMessage(QString("[收] finishConn 触发 bodyReceived=%1 total=%2 chunked=%3")
                    .arg(c->bodyReceived).arg(c->total).arg(c->chunked));
    c->out->close();
    QString saved = c->out->fileName();
    delete c->out; c->out = nullptr;
    m_core->finishUpload(c->query.value("sessionId"), c->query.value("fileId"), saved);
    emit logMessage(QString("[收] 完成 %1 -> %2").arg(c->fileName).arg(saved));
    sendStatus(sock, 200, "OK");
    sock->disconnectFromHost();
}

void HttpServer::onDisconnected()
{
    QTcpSocket* sock = qobject_cast<QTcpSocket*>(sender());
    if (!sock) return;
    Conn* c = m_conns.take(sock);
    if (c) {
        if (c->out) {   // 仍打开 = 传输未完成即断开，记一条截断告警
            emit logMessage(QString("[收] 传输中断（连接关闭）bodyReceived=%1 / total=%2")
                            .arg(c->bodyReceived).arg(c->total));
            c->out->close(); delete c->out;
        }
        delete c;
    }
    sock->deleteLater();
}

void HttpServer::parseHeaders(Conn* c, const QByteArray& head)
{
    QList<QByteArray> lines = head.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        QByteArray line = lines[i].trimmed();
        if (i == 0) {
            QList<QByteArray> parts = line.split(' ');
            if (parts.size() >= 2) {
                c->method = QString::fromLatin1(parts[0]);
                QString url = QString::fromLatin1(parts[1]);
                int qpos = url.indexOf('?');
                if (qpos >= 0) {
                    c->path = url.left(qpos);
                    parseQuery(c, url.mid(qpos + 1));
                } else {
                    c->path = url;
                }
            }
        } else {
            int colon = line.indexOf(':');
            if (colon > 0) {
                QString key = QString::fromLatin1(line.left(colon)).trimmed().toLower();
                QString val = QString::fromLatin1(line.mid(colon + 1)).trimmed();
                c->headers[key] = val;
                if (key == "content-length")
                    c->contentLength = val.toLongLong();
                if (key == "transfer-encoding" && val.contains("chunked", Qt::CaseInsensitive))
                    c->chunked = true;
            }
        }
    }
}

void HttpServer::parseQuery(Conn* c, const QString& q)
{
    for (const QString& pair : q.split('&')) {
        int eq = pair.indexOf('=');
        if (eq >= 0)
            c->query[pair.left(eq)] = QUrl::fromPercentEncoding(pair.mid(eq + 1).toUtf8());
        else if (!pair.isEmpty())
            c->query[pair] = "";
    }
}

void HttpServer::processComplete(Conn* c, QTcpSocket* sock)
{
    int headerEnd = c->buf.indexOf("\r\n\r\n");
    QByteArray body = c->buf.mid(headerEnd + 4);

    QString p = c->path;
    if (p == "/api/localsend/v2/info") {
        sendJson(sock, 200, m_core->myInfo(false));
        sock->disconnectFromHost();
    } else if (p == "/api/localsend/v2/register") {
        QJsonParseError err;
        QJsonObject obj = QJsonDocument::fromJson(body, &err).object();
        QJsonObject resp = m_core->handleRegister(obj, sock->peerAddress().toString().replace("::ffff:", ""));
        sendJson(sock, 200, resp);
        sock->disconnectFromHost();
    } else if (p == "/api/localsend/v2/prepare-upload") {
        // ---- 异步接收确认流程 ----
        // 不立即响应，而是保存请求、发出信号等 GUI 确认
        QJsonParseError err;
        QJsonObject reqObj = QJsonDocument::fromJson(body, &err).object();
        if (reqObj.isEmpty() || !reqObj.contains("files") || !reqObj.contains("info")) {
            sendStatus(sock, 400, "Invalid body");
            sock->disconnectFromHost();
            return;
        }

        QJsonObject info = reqObj.value("info").toObject();
        QJsonObject files = reqObj.value("files").toObject();

        PendingPrepare pp;
        pp.sock = sock;
        pp.conn = c;
        pp.peerAlias = info.value("alias").toString("未知设备");
        pp.peerIp = sock->peerAddress().toString().replace("::ffff:", "");
        pp.infoObj = info;
        pp.filesObj = files;

        QStringList fnames;
        QMap<QString, qint64> fsizes;
        QMap<QString, QString> fpreviews;
        bool allPreview = true;
        for (auto it = files.begin(); it != files.end(); ++it) {
            QJsonObject f = it.value().toObject();
            QString fid = it.key().isEmpty() ? f.value("id").toString() : it.key();
            QString fname = f.value("fileName").toString("file");
            qint64 fsize = qint64(f.value("size").toDouble(0));
            QString pv = f.value("preview").toString();
            pp.fileNames[fid] = fname;
            pp.fileSizes[fid] = fsize;
            pp.filePreviews[fid] = pv;
            if (pv.isEmpty()) allPreview = false;
            fnames.append(fname);
            fsizes[fid] = fsize;
            fpreviews[fid] = pv;
        }
        // 官方协议：所有条目都带 preview = 纯文本消息传输（接收方确认后回 204，
        // 不建会话、不落盘，内容只在 preview 中）
        pp.isMessageTransfer = allPreview;

        QString pendingId = QString("pp_%1").arg(++m_pendingCounter);
        m_pendingPrepares[pendingId] = pp;

        emit logMessage(QString("[收] 收到来自 %1 的传输请求（%2 个文件），等待用户确认…")
                        .arg(pp.peerAlias).arg(fnames.size()));

        if (m_autoAccept) {
            // selftest 模式：自动接受
            acceptPrepare(pendingId);
        } else {
            emit receiveRequest(pendingId, pp.peerAlias, fnames, fsizes, fpreviews,
                                info.value("fingerprint").toString());
        }

        // 注意：此处不调用 disconnectFromHost！socket 保持打开等 accept/reject

    } else if (p == "/api/localsend/v2/cancel") {
        m_core->cancelSession(c->query.value("sessionId"));
        sendStatus(sock, 200, "OK");
        sock->disconnectFromHost();
    } else {
        sendStatus(sock, 404, "Not Found");
        sock->disconnectFromHost();
    }
}

// 用户确认接收 → 创建 session 并返回 token
void HttpServer::acceptPrepare(const QString& pendingId)
{
    if (!m_pendingPrepares.contains(pendingId)) return;
    PendingPrepare pp = m_pendingPrepares.take(pendingId);
    QTcpSocket* sock = pp.sock;
    Conn* c = pp.conn;

    // 调用核心逻辑创建 session
    QString sessionId;
    QJsonObject body;
    body["info"] = pp.infoObj;
    body["files"] = pp.filesObj;
    QJsonObject resp = m_core->handlePrepareUpload(body, pp.peerIp, sessionId);

    if (sessionId.isEmpty()) {
        sendStatus(sock, 400, "Cannot create session");
        sock->disconnectFromHost();
        return;
    }

    emit logMessage(QString("[收] 用户已确认接收来自 %1 的传输（session=%2）")
                    .arg(pp.peerAlias).arg(sessionId));

    sendJson(sock, 200, resp);
    sock->disconnectFromHost();
}

// 用户拒绝接收 → 返回 403
void HttpServer::rejectPrepare(const QString& pendingId)
{
    if (!m_pendingPrepares.contains(pendingId)) return;
    PendingPrepare pp = m_pendingPrepares.take(pendingId);
    QTcpSocket* sock = pp.sock;

    emit logMessage(QString("[收] 用户已拒绝来自 %1 的传输请求").arg(pp.peerAlias));

    sendStatus(sock, 403, "Rejected by user");
    sock->disconnectFromHost();
}

void HttpServer::sendJson(QTcpSocket* sock, int code, const QJsonObject& obj)
{
    QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QByteArray resp = QString("HTTP/1.1 %1 OK\r\n").arg(code).toUtf8();
    resp += "Content-Type: application/json\r\n";
    resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    resp += "Access-Control-Allow-Origin: *\r\n";
    resp += "Connection: close\r\n\r\n";
    resp += body;
    sock->write(resp);
}

void HttpServer::sendStatus(QTcpSocket* sock, int code, const QByteArray& text)
{
    QByteArray resp = QString("HTTP/1.1 %1 %2\r\n").arg(code).arg(QString(text)).toUtf8();
    resp += "Content-Type: text/plain\r\n";
    resp += "Content-Length: " + QByteArray::number(text.size()) + "\r\n";
    resp += "Connection: close\r\n\r\n";
    resp += text;
    sock->write(resp);
}

// 204 No Content：官方对文本消息确认后的响应（无 body，无 Content-Length）
void HttpServer::sendNoContent(QTcpSocket* sock)
{
    QByteArray resp = "HTTP/1.1 204 No Content\r\n";
    resp += "Connection: close\r\n\r\n";
    sock->write(resp);
}
