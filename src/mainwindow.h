#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <DMainWindow>
#include <DProgressBar>
#include <QMap>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QStackedWidget>
#include <QDragEnterEvent>
#include "localsend.h"

class HttpServer;
class SpinLogo;
class QTimer;

class MainWindow : public Dtk::Widget::DMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

    // 调试用：直接切换页面（0=接收 1=发送 2=设置），供 --page 参数使用
    void showPage(int index) { m_navList->setCurrentRow(index); }

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    // 导航
    void switchPage(int index);

    // 发送页
    void refreshDevices();
    void onDevicesChanged();
    void onDeviceSelectionChanged();
    void chooseFiles();
    void chooseFolder();
    void addTextMessage();
    void sendSelected();
    void removeSelectedFiles();
    void clearFileList();
    void updateSendButton();
    void onSendProgress(quint32 taskId, const QString& deviceAlias,
                        const QString& fileName, qint64 sent, qint64 total);
    void onSendFinished(quint32 taskId, const QString& deviceAlias,
                        bool ok, const QString& msg);

    // 接收页
    void chooseSaveDir();
    void onReceiveStarted(const QString& sid, const QString& from);
    void onReceiveProgress(const QString& sid, const QString& name, qint64 r, qint64 t);
    void onReceiveFinished(const QString& sid, const QString& name, const QString& path);
    void onRecvItemActivated(QListWidgetItem* item);
    void showRecvContextMenu(const QPoint& pos);
    void openSelectedFile();
    void revealSelectedFile();
    void copySelectedPath();
    void clearRecvList();
    void onNearbyTick();          // 接收页设备名轮播

    // 接收确认（异步 prepare-upload）
    void onReceiveRequest(const QString& pendingId,
                          const QString& peerAlias,
                          const QStringList& fileNames,
                          const QMap<QString, qint64>& fileSizes,
                          const QMap<QString, QString>& filePreviews,
                          const QString& peerFingerprint);
    void onMessageReceived(const QString& sid, const QString& name,
                           const QString& content);

    // 设置页
    void applyAlias();
    void openLogDir();
    void showLogDialog();

    void onLog(const QString& msg);
    void updateInfoBar(quint16 port);

private:
    void setupUi();
    QWidget* createSendPage();
    QWidget* createRecvPage();
    QWidget* createSettingsPage();
    void addFilesToList(const QStringList& paths);
    QString makeTextFile(const QString& text) const;   // 文本消息 → outbox 临时 .txt
    QString formatSize(qint64 bytes) const;
    QIcon deviceIcon(const QString& deviceType) const;
    void refreshSendStatus();

    // 文本消息确认框（BUG2）：返回 1=同意 0=拒绝 2=一律拒绝
    int askReceiveTextMessage(const QString& peerAlias);
    // 文本消息查看弹窗（BUG4）：文字可选中 + 复制全部按钮
    void showMessageDialog(const QString& senderAlias, const QString& content);
    // 「一律拒绝」屏蔽键：指纹优先，缺失时退化为别名
    QString blockKey(const QString& fp, const QString& alias) const
    { return fp.isEmpty() ? QString("alias:") + alias : fp; }

    LocalSend* m_core;
    HttpServer* m_server;
    quint16 m_port = 53317;

    // 左侧导航
    QListWidget* m_navList;
    QStackedWidget* m_stack;

    // ---- 发送页 ----
    QListWidget* m_deviceList;        // 附近设备列表（多选）
    QListWidget* m_fileList;          // 待发送文件列表
    QPushButton* m_btnChooseFile;
    QPushButton* m_btnChooseFolder;
    QPushButton* m_btnAddText;
    QPushButton* m_btnSend;
    QPushButton* m_btnRemoveFile;
    QPushButton* m_btnClearFiles;
    QPushButton* m_btnRefresh;
    QLabel* m_deviceHint;
    Dtk::Widget::DProgressBar* m_sendProgress;
    QLabel* m_sendStatus;

    // ---- 接收页 ----
    SpinLogo* m_spinLogo = nullptr;        // 旋转动态 Logo（对齐官方接收页）
    QLabel* m_nearbyName = nullptr;        // 附近设备名（大字居中，轮播）
    QTimer* m_nearbyTimer = nullptr;
    QStringList m_nearbyNames;
    int m_nearbyIdx = 0;
    QLabel* m_saveDirLabel;
    QPushButton* m_btnChooseDir;
    QListWidget* m_recvList;
    QLabel* m_recvStatus;
    QPushButton* m_btnOpenFile;
    QPushButton* m_btnOpenFolder;
    QPushButton* m_btnClearRecv;

    // ---- 设置页 ----
    QLabel* m_aliasLabel;
    QLineEdit* m_aliasEdit;
    QPushButton* m_btnApplyAlias;
    QLabel* m_logPathLabel;

    // ---- 底部状态栏 ----
    QLabel* m_infoBar;

    // 数据
    QList<OutgoingFile> m_selectedFiles;
    QMap<QString, QListWidgetItem*> m_recvItems;   // "sid|name" -> item
    QMap<quint32, SendTask> m_sendTasks;           // taskId -> 任务状态（多目标并发）
    QSet<QString> m_blockedTextSenders;            // 「一律拒绝」的文本消息发送方（重启前生效）
};

#endif // MAINWINDOW_H
