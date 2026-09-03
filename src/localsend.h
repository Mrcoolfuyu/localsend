#ifndef LOCALSEND_H
#define LOCALSEND_H

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QMap>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStringList>
#include <QSharedPointer>
#include <QSslError>
#include <QList>

// 一个被发现的对端设备
struct Device {
    QString fingerprint;
    QString alias;
    QString version;
    QString deviceModel;
    QString deviceType;   // mobile | desktop | web | headless | server
    QString protocol;     // http | https
    int port = 0;
    bool download = false;
    QString ip;           // 发现到的 IP
    qint64 lastSeen = 0;
};

// 待发送的文件
struct OutgoingFile {
    QString id;
    QString fileName;
    QString filePath;     // 本机绝对路径（文本消息为空）
    qint64 size = 0;
    QString fileType;     // MIME
    QString preview;      // 官方协议：文本消息时 = 文本全文（接收端按消息展示）
    QByteArray content;   // 文本消息的内联字节（filePath 为空时用它上传）
};

// 接收会话（由 prepare-upload 创建）
struct RecvSession {
    QString sessionId;
    QString peerAlias;
    QString dir;                          // 保存目录（空则用默认）
    QMap<QString, QString> tokens;        // fileId -> token
    QMap<QString, QString> fileNames;    // fileId -> fileName
    QMap<QString, qint64> fileSizes;     // fileId -> size
    QMap<QString, qint64> received;      // fileId -> 已收字节
    QMap<QString, QString> savedPaths;   // fileId -> 最终路径
    QMap<QString, QString> previews;     // fileId -> 文本消息内容（官方协议 preview 字段）
};

// 一次「发给某个设备」的任务状态，供 GUI 聚合多个目标
struct SendTask {
    quint32 id = 0;
    QString deviceAlias;
    QString deviceIp;
    quint16 devicePort = 0;
    QString protocol;      // 实际使用的协议
    int fileCount = 0;
    int filesDone = 0;
    int percent = 0;       // 0-100，粗略（按文件粒度 + 字节粒度取最大）
    bool finished = false;
    bool ok = false;
    QString message;
    QString lastFile;      // 最近一个正在传的文件名
};

class LocalSend : public QObject
{
    Q_OBJECT

    // 发送会话（一次 sendFiles 对应一个，用于汇总多个文件的上传结果）
    struct SendSession {
        quint32 taskId = 0;
        QString deviceAlias;
        QString base;
        QString proto;
        QString sessionId;
        int total = 0;
        int done = 0;
        bool anyError = false;
        QStringList errors;
    };

public:
    explicit LocalSend(QObject* parent = nullptr);
    ~LocalSend();

    bool start(quint16 port = 53317);
    void stop();

    QString alias() const { return m_alias; }
    void setAlias(const QString& a);
    void setDeviceModel(const QString& m) { m_deviceModel = m; }
    quint16 port() const { return m_port; }
    QString fingerprint() const { return m_fingerprint; }
    QString saveDir() const { return m_saveDir; }
    void setSaveDir(const QString& d) { m_saveDir = d; }
    QList<Device> devices() const;
    QString localIp() const { return getLocalIp(); }
    QString multicastSummary() const { return m_mcSummary; }

    void refreshDiscovery();                       // 重新广播 + 主动 register
    // 返回任务 ID；可同时对多个目标调用（多选发送）
    quint32 sendFiles(const QString& fingerprint,
                      const QList<OutgoingFile>& files);

    // 测试/编程注入对端（生产不可用发现时手动指定）
    void addPeerForTest(const QString& fp, const QString& ip, int port,
                       const QString& alias = "peer",
                       const QString& protocol = "http");

    // 供 HttpServer 回调的核心逻辑
    QJsonObject myInfo(bool includePort = true) const;
    QJsonObject handleRegister(const QJsonObject& peer, const QString& peerIp);
    QJsonObject handlePrepareUpload(const QJsonObject& body, const QString& peerIp,
                                    QString& sessionIdOut);
    // 返回最终保存路径；空串表示拒绝
    QString beginUpload(const QString& sessionId, const QString& fileId,
                        const QString& token, qint64 total, QString& fileNameOut);
    void finishUpload(const QString& sessionId, const QString& fileId,
                      const QString& savedPath);
    void cancelSession(const QString& sessionId);
    // 文本消息投递（消息传输不建会话不落盘，确认后由 HttpServer 调用）
    void deliverMessage(const QString& peerAlias, const QString& fileName,
                        const QString& content);

signals:
    void devicesChanged();
    void logMessage(const QString& msg);
    void receiveStarted(const QString& sessionId, const QString& fromAlias);
    void receiveProgress(const QString& sessionId, const QString& fileName,
                         qint64 received, qint64 total);
    void receiveFinished(const QString& sessionId, const QString& fileName,
                         const QString& savedPath);
    // 文本消息接收完成（官方协议 preview 字段非空时触发）
    void messageReceived(const QString& sessionId, const QString& fileName,
                         const QString& content);
    // 带任务 ID 的发送信号：一次多选发送会产生多个任务
    void sendProgress(quint32 taskId, const QString& deviceAlias,
                      const QString& fileName, qint64 sent, qint64 total);
    void sendFinished(quint32 taskId, const QString& deviceAlias,
                      bool ok, const QString& msg);

private slots:
    void onUdpReadyRead();
    void onAnnounceTimer();
    void expireDevices();
    void onPrepareDone(QNetworkReply* reply);
    void onUploadDone(QNetworkReply* reply, QSharedPointer<SendSession> ss,
                      const QString& fileName);
    void onSslErrors(QNetworkReply* reply, const QList<QSslError>& errors);

private:
    // 发送上下文（一次 sendFiles 一份，可能在 http/https 之间回退重试）
    struct SendCtx {
        quint32 taskId = 0;
        Device dev;
        QList<OutgoingFile> files;
        QStringList protos;                 // 候选协议，按优先级
        int protoIdx = 0;
        QString base;                       // 本次尝试的 scheme://ip:port
    };

    void sendAnnouncement();
    void sendRegisterTo(const Device& dev);
    QString getLocalIp() const;
    QString newSessionId() const;
    QString sanitizeFileName(const QString& name) const;
    void tryPrepare(const SendCtx& ctx);           // 用 ctx.protos[ctx.protoIdx] 发起 prepare-upload
    void finishTask(quint32 taskId, const QString& deviceAlias, bool ok, const QString& msg);
    // 统一日志出口：写文件 + 发 logMessage 信号
    void dbg(const QString& msg);

    QUdpSocket* m_udp = nullptr;
    QTimer* m_announceTimer = nullptr;
    QTimer* m_expireTimer = nullptr;
    QNetworkAccessManager* m_nam = nullptr;

    QString m_alias;
    QString m_deviceModel = "Huawei L420x";
    QString m_fingerprint;
    quint16 m_port = 53317;
    QString m_saveDir;
    QString m_mcSummary;           // 组播已加入的网卡名（诊断用）

    QMap<QString, Device> m_devices;        // fingerprint -> Device
    QMap<QString, RecvSession> m_recvSessions;

    QMap<QNetworkReply*, SendCtx> m_sendCtx;
    quint32 m_taskCounter = 0;
};

#endif // LOCALSEND_H
