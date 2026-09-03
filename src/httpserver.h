#ifndef HTTPSERVER_H
#define HTTPSERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMap>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

class LocalSend;

// 极简 HTTP/1.1 服务器：仅实现 Localsend 需要的几个路由。
// 不依赖 QHttpServer 模块，避免额外依赖。接收文件时直接流式写入磁盘。
class HttpServer : public QObject
{
    Q_OBJECT
public:
    explicit HttpServer(LocalSend* core, QObject* parent = nullptr);
    bool listen(quint16 port);
    bool isListening() const;
    void close();
    QString errorString() const;
    quint16 port() const;

    // 是否自动接收（true = 跳过确认对话框，用于 selftest）
    bool autoAccept() const { return m_autoAccept; }
    void setAutoAccept(bool v) { m_autoAccept = v; }

signals:
    void logMessage(const QString& msg);

    // 收到对端 prepare-upload 请求，等待 GUI 确认或拒绝
    // fileNames/fileSizes/filePreviews 的 key 是 fileId（previews 仅文本消息非空）
    // peerFingerprint 用于与对端「验证」页比对（官方协议的验证 = 双方指纹比对）
    void receiveRequest(const QString& pendingId,
                        const QString& peerAlias,
                        const QStringList& fileNames,
                        const QMap<QString, qint64>& fileSizes,
                        const QMap<QString, QString>& filePreviews,
                        const QString& peerFingerprint);

public slots:
    void acceptPrepare(const QString& pendingId);
    void rejectPrepare(const QString& pendingId);

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    struct Conn {
        QByteArray buf;
        bool headerDone = false;
        QString method;
        QString path;
        QMap<QString, QString> headers;
        QMap<QString, QString> query;
        qint64 contentLength = 0;
        qint64 bodyReceived = 0;
        bool isUpload = false;
        QString sessionId, fileId, token;
        QString fileName;
        QFile* out = nullptr;
        qint64 total = 0;
        // chunked 传输解码状态（当客户端未带 Content-Length 时）
        bool chunked = false;
        QByteArray raw;          // 尚未解码的原始字节
        int chunkState = 0;      // 0=待解析长度行, 1=待收数据
        qint64 chunkRemain = 0;
    };

    // 等待用户确认的 prepare-upload 请求
    struct PendingPrepare {
        QTcpSocket* sock;
        Conn* conn;
        QString peerAlias;
        QMap<QString, QString> fileNames;   // fileId -> fileName
        QMap<QString, qint64> fileSizes;    // fileId -> size
        QMap<QString, QString> filePreviews;// fileId -> 文本消息内容（非空 = 消息）
        QJsonObject filesObj;               // 原始 files 对象（回传给 handlePrepareUpload）
        QJsonObject infoObj;                // 原始 info 对象
        QString peerIp;
        bool isMessageTransfer = false;     // 所有条目都带 preview = 纯消息传输
    };

    void parseHeaders(Conn* c, const QByteArray& head);
    void processComplete(Conn* c, QTcpSocket* sock);
    void finishConn(Conn* c, QTcpSocket* sock);
    void handleUploadData(Conn* c, QTcpSocket* sock, const QByteArray& data);
    void processChunked(Conn* c, QTcpSocket* sock);
    void sendJson(QTcpSocket* sock, int code, const QJsonObject& obj);
    void sendStatus(QTcpSocket* sock, int code, const QByteArray& text);
    void sendNoContent(QTcpSocket* sock);
    void parseQuery(Conn* c, const QString& q);

    LocalSend* m_core;
    QTcpServer* m_server;
    QMap<QTcpSocket*, Conn*> m_conns;
    QMap<QString, PendingPrepare> m_pendingPrepares;
    int m_pendingCounter = 0;
    bool m_autoAccept = false;   // selftest 用，跳过确认
};

#endif // HTTPSERVER_H
