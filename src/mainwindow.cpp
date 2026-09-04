#include "mainwindow.h"
#include "httpserver.h"
#include "localsend.h"
#include "logger.h"
#include "spinlogo.h"

#include <DProgressBar>
#include <DFileDialog>
#include <DMessageBox>
#include <DTitlebar>
using namespace Dtk::Widget;

#include <QTabWidget>
#include <QListWidget>
#include <QAbstractItemView>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSplitter>
#include <QDateTime>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMenu>
#include <QAction>
#include <QProcess>
#include <QDesktopServices>
#include <QClipboard>
#include <QApplication>
#include <QFileIconProvider>
#include <QDialog>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QDirIterator>
#include <QTimer>
#include <QSet>
#include <QPixmap>
#include <QMimeDatabase>
#include <QUuid>
#include <algorithm>
#include <functional>

// ---------- 全局小工具：用系统默认程序打开 / 在文件管理器中定位 ----------

static bool revealInFolder(const QString& path)
{
    const QFileInfo fi(path);
    if (!fi.exists()) return false;

    // UOS/Deepin：dde-file-manager --show-item 可直接选中该文件
    if (QProcess::startDetached("dde-file-manager", QStringList() << "--show-item" << fi.absoluteFilePath()))
        return true;
    if (QProcess::startDetached("dde-file-manager", QStringList() << fi.absolutePath()))
        return true;
    if (QProcess::startDetached("nautilus", QStringList() << fi.absolutePath()))
        return true;
    if (QProcess::startDetached("thunar", QStringList() << fi.absolutePath()))
        return true;

    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
    return true;
}

static bool openWithDefaultApp(const QString& path)
{
    if (!QFileInfo(path).exists()) return false;
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

// ============================================================
// 构造 / 析构
// ============================================================

MainWindow::MainWindow(QWidget* parent)
    : DMainWindow(parent)
{
    setWindowTitle("LocalSend");
    setWindowIcon(QIcon(":/localsend/logo-512.png"));
    QApplication::setWindowIcon(QIcon(":/localsend/logo-512.png"));
    // 去掉 DTK 标题栏「图标 | 标题」之间的灰色竖线
    titlebar()->setSeparatorVisible(false);
    resize(940, 640);
    setMinimumSize(760, 520);

    m_core = new LocalSend(this);
    m_server = new HttpServer(m_core, this);

    Logger::log(QString("[GUI] 初始化：Qt=%1  资源图标=%2")
                    .arg(qVersion())
                    .arg(QFile(":/localsend/logo-512.png").exists() ? "已嵌入" : "缺失"));

    // ---- 绑定端口 ----
    if (!m_server->listen(m_port)) {
        bool ok = false;
        for (quint16 p = 53318; p < 53330; ++p) {
            m_server->close();
            if (m_server->listen(p)) { m_port = p; ok = true; break; }
        }
        if (!ok) {
            Logger::log(QString("[GUI] 致命：无法监听 53317-53329，错误=%1").arg(m_server->errorString()));
            DMessageBox::warning(this, "端口错误",
                QString("无法监听 53317（以及 53318–53329）端口，接收功能不可用。\n"
                        "请先关闭占用该端口的程序。\n错误详情：%1").arg(m_server->errorString()));
        } else {
            Logger::log(QString("[GUI] 53317 被占用，改用端口 %1").arg(m_port));
            DMessageBox::information(this, "端口变更",
                QString("默认 53317 被占用，已改用 %1。").arg(m_port));
        }
    }
    Logger::log(QString("[GUI] HTTP 服务监听端口 %1：%2")
                    .arg(m_port).arg(m_server->isListening() ? "成功" : "失败"));

    m_core->start(m_port);

    // ---- 构建 UI ----
    setupUi();

    // ---- 信号连接：核心 → UI ----
    connect(m_core, &LocalSend::devicesChanged, this, &MainWindow::onDevicesChanged);
    connect(m_core, &LocalSend::sendProgress, this, &MainWindow::onSendProgress);
    connect(m_core, &LocalSend::sendFinished, this, &MainWindow::onSendFinished);
    connect(m_core, &LocalSend::receiveStarted, this, &MainWindow::onReceiveStarted);
    connect(m_core, &LocalSend::receiveProgress, this, &MainWindow::onReceiveProgress);
    connect(m_core, &LocalSend::receiveFinished, this, &MainWindow::onReceiveFinished);
    connect(m_core, &LocalSend::logMessage, this, &MainWindow::onLog);
    connect(m_server, &HttpServer::logMessage, this, &MainWindow::onLog);

    // 接收确认（异步 prepare-upload）
    connect(m_server, &HttpServer::receiveRequest, this, &MainWindow::onReceiveRequest);
    connect(m_core, &LocalSend::messageReceived, this, &MainWindow::onMessageReceived);

    // 启动时刷新设备发现
    refreshDevices();
    onDevicesChanged();
}

MainWindow::~MainWindow() {}

// ============================================================
// UI 布局：左侧导航 + 右侧内容区（对齐官方 LocalSend 风格）
// ============================================================

void MainWindow::setupUi()
{
    // -------- 左侧导航栏 --------
    m_navList = new QListWidget;
    m_navList->setViewMode(QListView::ListMode);
    m_navList->setIconSize(QSize(22, 22));
    m_navList->setSpacing(2);
    m_navList->setFrameShape(QFrame::NoFrame);
    m_navList->setStyleSheet(
        "QListWidget { background: transparent; border: none; }"
        "QListWidget::item { padding: 9px 10px; border-radius: 6px; margin: 1px 6px; }"
        "QListWidget::item:selected { background: #e8f4ff; color: #0066cc; }"
        "QListWidget::item:hover { background: #eef3f7; }");

    QIcon icoRecv = QIcon::fromTheme("folder-download", QIcon::fromTheme("go-down", QIcon(":/localsend/logo-32.png")));
    QIcon icoSend = QIcon::fromTheme("mail-send", QIcon::fromTheme("go-up", QIcon(":/localsend/logo-32.png")));
    QIcon icoSet  = QIcon::fromTheme("preferences-system", QIcon::fromTheme("settings", QIcon(":/localsend/logo-32.png")));

    QListWidgetItem* navRecv = new QListWidgetItem(icoRecv, "  接收");
    navRecv->setSizeHint(QSize(140, 40));
    QListWidgetItem* navSend = new QListWidgetItem(icoSend, "  发送");
    navSend->setSizeHint(QSize(140, 40));
    QListWidgetItem* navSettings = new QListWidgetItem(icoSet, "  设置");
    navSettings->setSizeHint(QSize(140, 40));

    m_navList->addItem(navRecv);
    m_navList->addItem(navSend);
    m_navList->addItem(navSettings);
    m_navList->setCurrentRow(1);   // 默认发送页

    // -------- 右侧堆栈页面 --------
    m_stack = new QStackedWidget;
    m_stack->addWidget(createRecvPage());     // index 0
    m_stack->addWidget(createSendPage());     // index 1
    m_stack->addWidget(createSettingsPage()); // index 2
    m_stack->setCurrentIndex(1);

    // -------- 底部状态栏 --------
    m_infoBar = new QLabel;
    m_infoBar->setFrameShape(QFrame::StyledPanel);
    m_infoBar->setStyleSheet("QLabel { padding: 5px 10px; background: #f0f2f4; color: #555; font-size: 12px; }");
    updateInfoBar(m_port);

    // -------- 主布局 --------
    QWidget* central = new QWidget;
    QHBoxLayout* mainLayout = new QHBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 导航区域（顶部带官方 LOGO）
    QWidget* navPanel = new QWidget;
    navPanel->setFixedWidth(150);
    navPanel->setStyleSheet("QWidget { background: #f7f9fa; border-right: 1px solid #e0e0e0; }");
    QVBoxLayout* navLayout = new QVBoxLayout(navPanel);
    navLayout->setContentsMargins(0, 10, 0, 10);
    navLayout->setSpacing(0);

    QWidget* brand = new QWidget;
    QHBoxLayout* brandLayout = new QHBoxLayout(brand);
    brandLayout->setContentsMargins(14, 4, 8, 12);
    brandLayout->setSpacing(8);
    QLabel* brandIcon = new QLabel;
    brandIcon->setPixmap(QPixmap(":/localsend/logo-128.png").scaled(30, 30,
                                 Qt::KeepAspectRatio, Qt::SmoothTransformation));
    QLabel* brandText = new QLabel("LocalSend");
    brandText->setStyleSheet("font-size: 14px; font-weight: bold; color: #333;");
    brandLayout->addWidget(brandIcon);
    brandLayout->addWidget(brandText);
    brandLayout->addStretch();

    navLayout->addWidget(brand);
    navLayout->addWidget(m_navList);
    navLayout->addStretch();

    QLabel* brandFoot = new QLabel("v0.3.5 · Mr.cool");
    brandFoot->setStyleSheet("color: #9aa2a9; font-size: 11px; padding-left: 16px;");
    navLayout->addWidget(brandFoot);

    mainLayout->addWidget(navPanel);
    mainLayout->addWidget(m_stack, 1);

    QVBoxLayout* rootLayout = new QVBoxLayout;
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    rootLayout->addWidget(central, 1);
    rootLayout->addWidget(m_infoBar);

    QWidget* root = new QWidget;
    root->setLayout(rootLayout);
    setCentralWidget(root);

    // -------- 导航切换 --------
    connect(m_navList, &QListWidget::currentRowChanged, this, &MainWindow::switchPage);

    // 启用拖放
    setAcceptDrops(true);
}

// ============================================================
// 发送页
// ============================================================

QWidget* MainWindow::createSendPage()
{
    QWidget* page = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);

    // ---- 选择区 ----
    QLabel* selLabel = new QLabel("选择");
    selLabel->setStyleSheet("font-size: 15px; font-weight: bold; color: #333;");

    QHBoxLayout* selBtns = new QHBoxLayout;
    m_btnChooseFile = new QPushButton(QIcon::fromTheme("document-new", QIcon(":/localsend/logo-32.png")), " 文件");
    m_btnChooseFolder = new QPushButton(QIcon::fromTheme("folder", QIcon(":/localsend/logo-32.png")), " 文件夹");
    m_btnAddText = new QPushButton(QIcon::fromTheme("text-editor", QIcon::fromTheme("document-edit", QIcon(":/localsend/logo-32.png"))), " 文本");

    QString btnStyle =
        "QPushButton {"
        "  font-size: 13px;"
        "  padding: 10px 22px;"
        "  border: 1px solid #d5d5d5;"
        "  border-radius: 8px;"
        "  background: white;"
        "  text-align: center;"
        "}"
        "QPushButton:hover { background: #eef6ff; border-color: #4a9eff; }"
        "QPushButton:pressed { background: #d0e3ff; }";
    m_btnChooseFile->setStyleSheet(btnStyle);
    m_btnChooseFile->setFixedHeight(52);
    m_btnChooseFolder->setStyleSheet(btnStyle);
    m_btnChooseFolder->setFixedHeight(52);
    m_btnAddText->setStyleSheet(btnStyle);
    m_btnAddText->setFixedHeight(52);

    selBtns->addWidget(m_btnChooseFile);
    selBtns->addWidget(m_btnChooseFolder);
    selBtns->addWidget(m_btnAddText);
    selBtns->addStretch();

    QLabel* dropHint = new QLabel("提示：也可以直接把文件/文件夹从文件管理器拖到本窗口");
    dropHint->setStyleSheet("color: #98a0a8; font-size: 12px;");

    // ---- 设备区 ----
    QHBoxLayout* devHeader = new QHBoxLayout;
    QLabel* devLabel = new QLabel("附近的设备");
    devLabel->setStyleSheet("font-size: 15px; font-weight: bold; color: #333;");
    m_btnRefresh = new QPushButton("⟳ 刷新");
    m_btnRefresh->setToolTip("重新广播并刷新设备列表");
    m_btnRefresh->setFixedHeight(28);
    devHeader->addWidget(devLabel);
    devHeader->addStretch();
    devHeader->addWidget(m_btnRefresh);

    m_deviceHint = new QLabel("可多选（Ctrl / Shift + 点击），一次发送给多台设备");
    m_deviceHint->setStyleSheet("color: #98a0a8; font-size: 12px;");

    m_deviceList = new QListWidget;
    m_deviceList->setSelectionMode(QAbstractItemView::ExtendedSelection);   // 多选：支持一次发给多台
    m_deviceList->setAlternatingRowColors(true);
    m_deviceList->setIconSize(QSize(28, 28));
    m_deviceList->setMinimumHeight(100);
    m_deviceList->setStyleSheet(
        "QListWidget { border: 1px solid #ddd; border-radius: 6px; padding: 4px; background: white; }"
        "QListWidget::item { padding: 8px 10px; border-radius: 4px; margin: 1px 0; }"
        "QListWidget::item:selected { background: #e8f4ff; color: #0066cc; }"
        "QListWidget::item:hover { background: #f5f9ff; }");

    // ---- 文件列表区 ----
    QLabel* fileListLabel = new QLabel("待发送文件");
    fileListLabel->setStyleSheet("font-size: 15px; font-weight: bold; color: #333;");

    QHBoxLayout* fileToolbar = new QHBoxLayout;
    m_btnRemoveFile = new QPushButton("✕ 移除选中");
    m_btnClearFiles = new QPushButton("🗑 清空列表");
    QString smallBtnStyle =
        "QPushButton { font-size: 12px; padding: 5px 14px; border: 1px solid #ccc; border-radius: 4px; background: white; }"
        "QPushButton:hover { background: #f0f0f0; }"
        "QPushButton:disabled { color: #aaa; }";
    m_btnRemoveFile->setStyleSheet(smallBtnStyle);
    m_btnClearFiles->setStyleSheet(smallBtnStyle);
    fileToolbar->addWidget(m_btnRemoveFile);
    fileToolbar->addWidget(m_btnClearFiles);
    fileToolbar->addStretch();

    m_fileList = new QListWidget;
    m_fileList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileList->setAlternatingRowColors(true);
    m_fileList->setIconSize(QSize(24, 24));
    m_fileList->setMinimumHeight(80);
    m_fileList->setStyleSheet(
        "QListWidget { border: 1px solid #ddd; border-radius: 6px; padding: 4px; background: white; }"
        "QListWidget::item { padding: 6px 10px; border-radius: 4px; margin: 1px 0; }"
        "QListWidget::item:selected { background: #e8f4ff; color: #0066cc; }"
        "QListWidget::item:hover { background: #f5f9ff; }");

    // ---- 发送按钮 + 进度 ----
    QHBoxLayout* sendBar = new QHBoxLayout;
    m_btnSend = new QPushButton("发送到选定设备");
    m_btnSend->setEnabled(false);
    m_btnSend->setFixedHeight(40);
    m_btnSend->setMinimumWidth(180);
    m_btnSend->setStyleSheet(
        "QPushButton { font-size: 14px; font-weight: bold; border-radius: 6px; "
        "background: #4a9eff; color: white; border: none; padding: 0 26px; }"
        "QPushButton:hover { background: #3a8eef; }"
        "QPushButton:pressed { background: #2a7edf; }"
        "QPushButton:disabled { background: #cccccc; }");

    sendBar->addWidget(m_btnSend);
    sendBar->addStretch();

    m_sendProgress = new DProgressBar;
    m_sendProgress->setTextVisible(true);
    m_sendProgress->setFixedHeight(22);
    m_sendStatus = new QLabel("就绪 — 选择文件和目标设备后点击发送");
    m_sendStatus->setStyleSheet("color: #666; font-size: 12px;");
    m_sendStatus->setWordWrap(true);

    // ---- 组装 ----
    layout->addWidget(selLabel);
    layout->addLayout(selBtns);
    layout->addWidget(dropHint);
    layout->addSpacing(4);
    layout->addLayout(devHeader);
    layout->addWidget(m_deviceHint);
    layout->addWidget(m_deviceList, 2);
    layout->addSpacing(4);
    layout->addWidget(fileListLabel);
    layout->addLayout(fileToolbar);
    layout->addWidget(m_fileList, 2);
    layout->addSpacing(4);
    layout->addLayout(sendBar);
    layout->addWidget(m_sendProgress);
    layout->addWidget(m_sendStatus);

    // ---- 信号连接 ----
    connect(m_btnRefresh, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(m_btnChooseFile, &QPushButton::clicked, this, &MainWindow::chooseFiles);
    connect(m_btnChooseFolder, &QPushButton::clicked, this, &MainWindow::chooseFolder);
    connect(m_btnAddText, &QPushButton::clicked, this, &MainWindow::addTextMessage);
    connect(m_btnSend, &QPushButton::clicked, this, &MainWindow::sendSelected);
    connect(m_btnRemoveFile, &QPushButton::clicked, this, &MainWindow::removeSelectedFiles);
    connect(m_btnClearFiles, &QPushButton::clicked, this, &MainWindow::clearFileList);
    connect(m_deviceList, &QListWidget::itemSelectionChanged, this, &MainWindow::onDeviceSelectionChanged);
    // 文件列表选中项变化时刷新发送状态（BUG1：只发送列表中选中的文件）
    connect(m_fileList, &QListWidget::itemSelectionChanged, this, &MainWindow::updateSendButton);

    return page;
}

// ============================================================
// 接收页
// ============================================================

QWidget* MainWindow::createRecvPage()
{
    QWidget* page = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);

    // ---- 旋转 Logo + 附近设备名（对齐官方接收页视觉）----
    m_spinLogo = new SpinLogo;
    m_spinLogo->setFixedSize(150, 150);
    m_spinLogo->start();

    m_nearbyName = new QLabel("等待附近设备…");
    m_nearbyName->setStyleSheet("font-size: 24px; font-weight: 500; color: #333; background: transparent;");
    m_nearbyName->setAlignment(Qt::AlignCenter);

    QHBoxLayout* logoRow = new QHBoxLayout;
    logoRow->addStretch();
    QVBoxLayout* logoCol = new QVBoxLayout;
    logoCol->addWidget(m_spinLogo, 0, Qt::AlignHCenter);
    logoCol->addWidget(m_nearbyName);
    logoRow->addLayout(logoCol);
    logoRow->addStretch();

    m_nearbyTimer = new QTimer(this);
    connect(m_nearbyTimer, &QTimer::timeout, this, &MainWindow::onNearbyTick);
    m_nearbyTimer->start(3000);

    // 保存目录
    QHBoxLayout* dirRow = new QHBoxLayout;
    m_saveDirLabel = new QLabel("保存目录：" + m_core->saveDir());
    m_saveDirLabel->setWordWrap(true);
    m_btnChooseDir = new QPushButton("更改…");
    dirRow->addWidget(m_saveDirLabel, 1);
    dirRow->addWidget(m_btnChooseDir);

    // 接收历史
    m_recvList = new QListWidget;
    m_recvList->setAlternatingRowColors(true);
    m_recvList->setIconSize(QSize(24, 24));
    m_recvList->setStyleSheet(
        "QListWidget { border: 1px solid #ddd; border-radius: 6px; padding: 4px; background: white; }"
        "QListWidget::item { padding: 7px 10px; border-radius: 4px; margin: 1px 0; }"
        "QListWidget::item:selected { background: #e8f4ff; color: #0066cc; }");
    m_recvList->setContextMenuPolicy(Qt::CustomContextMenu);

    // 接收文件操作按钮
    QHBoxLayout* recvBtns = new QHBoxLayout;
    m_btnOpenFile = new QPushButton("打开");
    m_btnOpenFolder = new QPushButton("打开所在目录");
    m_btnClearRecv = new QPushButton("清空列表");
    QString smallBtnStyle =
        "QPushButton { font-size: 12px; padding: 5px 14px; border: 1px solid #ccc; border-radius: 4px; background: white; }"
        "QPushButton:hover { background: #f0f0f0; }"
        "QPushButton:disabled { color: #aaa; }";
    m_btnOpenFile->setStyleSheet(smallBtnStyle);
    m_btnOpenFolder->setStyleSheet(smallBtnStyle);
    m_btnClearRecv->setStyleSheet(smallBtnStyle);
    recvBtns->addWidget(m_btnOpenFile);
    recvBtns->addWidget(m_btnOpenFolder);
    recvBtns->addStretch();
    recvBtns->addWidget(m_btnClearRecv);

    m_recvStatus = new QLabel("监听中，等待传入传输…\n（所有传输都需要你手动确认接受；右键已接收的文件可打开或在目录中显示）");
    m_recvStatus->setStyleSheet("color: #888; font-size: 12px;");
    m_recvStatus->setAlignment(Qt::AlignCenter);
    m_recvStatus->setWordWrap(true);
    m_recvStatus->setMinimumHeight(56);

    layout->addLayout(logoRow);
    layout->addLayout(dirRow);
    layout->addWidget(m_recvList, 1);
    layout->addLayout(recvBtns);
    layout->addWidget(m_recvStatus);

    connect(m_btnChooseDir, &QPushButton::clicked, this, &MainWindow::chooseSaveDir);
    connect(m_recvList, &QListWidget::itemDoubleClicked, this, &MainWindow::onRecvItemActivated);
    connect(m_recvList, &QListWidget::customContextMenuRequested, this, &MainWindow::showRecvContextMenu);
    connect(m_btnOpenFile, &QPushButton::clicked, this, &MainWindow::openSelectedFile);
    connect(m_btnOpenFolder, &QPushButton::clicked, this, &MainWindow::revealSelectedFile);
    connect(m_btnClearRecv, &QPushButton::clicked, this, &MainWindow::clearRecvList);

    return page;
}

// ============================================================
// 设置页
// ============================================================

QWidget* MainWindow::createSettingsPage()
{
    QWidget* page = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);

    QLabel* title = new QLabel("设置");
    title->setStyleSheet("font-size: 18px; font-weight: bold; color: #333;");

    // 设备别名
    QLabel* aliasLbl = new QLabel("设备别名：");
    m_aliasEdit = new QLineEdit(m_core->alias());
    m_btnApplyAlias = new QPushButton("应用");

    QHBoxLayout* aliasRow = new QHBoxLayout;
    aliasRow->addWidget(aliasLbl);
    aliasRow->addWidget(m_aliasEdit, 1);
    aliasRow->addWidget(m_btnApplyAlias);

    // 设备信息
    QLabel* infoLabel = new QLabel();
    infoLabel->setWordWrap(true);
    infoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoLabel->setStyleSheet("background: #f5f5f5; padding: 12px; border-radius: 6px; font-size: 12px;");
    infoLabel->setText(
        QString("版本：LocalSend 0.3.5\n"
                "作者：Mr.cool（https://github.com/Mrcoolfuyu/）\n"
                "协议端口：%1\n"
                "本机 IP：%2\n"
                "指纹 ID：%3\n"
                "组播接口：%4\n\n"
                "提示：其他设备通过局域网组播发现此设备。若对方开启 HTTPS，"
                "本程序会自动携带客户端证书以 https 连接并接受其自签名证书，失败时自动回退 http。")
            .arg(m_port).arg(m_core->localIp()).arg(m_core->fingerprint())
            .arg(m_core->multicastSummary()));

    // 日志区
    QLabel* logTitle = new QLabel("调试日志");
    logTitle->setStyleSheet("font-size: 15px; font-weight: bold; color: #333;");
    m_logPathLabel = new QLabel(Logger::filePath());
    m_logPathLabel->setWordWrap(true);
    m_logPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_logPathLabel->setStyleSheet("background: #f5f5f5; padding: 8px; border-radius: 4px; font-size: 11px; color: #666;");

    QPushButton* btnViewLog = new QPushButton("查看日志");
    QPushButton* btnOpenLogDir = new QPushButton("打开日志目录");
    QHBoxLayout* logRow = new QHBoxLayout;
    logRow->addWidget(btnViewLog);
    logRow->addWidget(btnOpenLogDir);
    logRow->addStretch();

    layout->addWidget(title);
    layout->addSpacing(8);
    layout->addLayout(aliasRow);
    layout->addSpacing(8);
    layout->addWidget(infoLabel);
    layout->addSpacing(8);
    layout->addWidget(logTitle);
    layout->addWidget(m_logPathLabel);
    layout->addLayout(logRow);
    layout->addStretch();

    connect(m_btnApplyAlias, &QPushButton::clicked, this, &MainWindow::applyAlias);
    connect(btnViewLog, &QPushButton::clicked, this, &MainWindow::showLogDialog);
    connect(btnOpenLogDir, &QPushButton::clicked, this, &MainWindow::openLogDir);

    return page;
}

// ============================================================
// 导航切换
// ============================================================

void MainWindow::switchPage(int index)
{
    m_stack->setCurrentIndex(index);
}

// ============================================================
// 拖拽支持
// ============================================================

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dragMoveEvent(QDragMoveEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const QMimeData* mime = event->mimeData();
    if (!mime->hasUrls()) return;

    QStringList paths;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile())
            paths.append(url.toLocalFile());

    if (!paths.isEmpty()) {
        addFilesToList(paths);
        event->acceptProposedAction();
        // 拖入后自动切到发送页
        m_navList->setCurrentRow(1);
    }
}

// ============================================================
// 发送逻辑
// ============================================================

void MainWindow::refreshDevices()
{
    m_core->refreshDiscovery();
    m_deviceHint->setText("已广播发现请求，正在探测离线设备…");
    QTimer::singleShot(4500, this, [this]() {   // 探测 2s×2 次协议重试，等它结束再回填
        onDevicesChanged();
        m_deviceHint->setText("可多选（Ctrl / Shift + 点击），一次发送给多台设备");
    });
}

void MainWindow::onDevicesChanged()
{
    // 记住已选中的指纹，避免刷新后丢失选择
    QSet<QString> keep;
    for (QListWidgetItem* it : m_deviceList->selectedItems())
        keep.insert(it->data(Qt::UserRole).toString());

    m_deviceList->clear();
    for (const Device& d : m_core->devices()) {
        QString model = d.deviceModel.isEmpty() ? d.deviceType : d.deviceModel;
        QString text = QString("%1   %2 · %3")
                            .arg(d.alias)
                            .arg(d.protocol.toUpper())
                            .arg(model);
        QListWidgetItem* item = new QListWidgetItem(deviceIcon(d.deviceType), text);
        item->setData(Qt::UserRole, d.fingerprint);
        item->setToolTip(QString("%1\nIP: %2\n端口: %3\n型号: %4\n协议: %5\n指纹: %6\n\n提示：可多选后一次发送到多台设备")
                         .arg(d.alias).arg(d.ip).arg(d.port).arg(model)
                         .arg(d.protocol).arg(d.fingerprint));
        m_deviceList->addItem(item);
        if (keep.contains(d.fingerprint)) item->setSelected(true);
    }

    // 同步接收页的设备名轮播列表
    m_nearbyNames.clear();
    for (const Device& d : m_core->devices())
        m_nearbyNames.append(d.alias);
    m_nearbyIdx = 0;
    if (m_nearbyName)
        m_nearbyName->setText(m_nearbyNames.isEmpty()
                                  ? QString("等待附近设备…")
                                  : m_nearbyNames.first());

    updateSendButton();
}

void MainWindow::onDeviceSelectionChanged()
{
    updateSendButton();
}

// 接收页设备名轮播：多台设备时每 3 秒切换显示一台
void MainWindow::onNearbyTick()
{
    if (!m_nearbyName) return;
    if (m_nearbyNames.isEmpty()) {
        m_nearbyName->setText("等待附近设备…");
        return;
    }
    m_nearbyIdx = (m_nearbyIdx + 1) % m_nearbyNames.size();
    m_nearbyName->setText(m_nearbyNames.at(m_nearbyIdx));
}

void MainWindow::chooseFiles()
{
    QStringList paths = DFileDialog::getOpenFileNames(this, "选择要发送的文件");
    if (!paths.isEmpty())
        addFilesToList(paths);
}

void MainWindow::chooseFolder()
{
    QString dir = DFileDialog::getExistingDirectory(this, "选择要发送的文件夹");
    if (dir.isEmpty()) return;

    // 递归列出文件夹内所有文件
    QStringList paths;
    QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) paths.append(it.next());

    if (paths.isEmpty()) {
        DMessageBox::information(this, "提示", "所选文件夹为空或不含文件。");
        return;
    }
    addFilesToList(paths);
}

// 发送文本：与官方 LocalSend 协议一致 —— 文本作为「带 preview 字段的
// text/plain 虚拟文件」发送（fileName=UUID.txt，preview=全文），接收端
// 检测到 preview 非空即按消息弹窗展示，不会显示为 UUID.txt。
void MainWindow::addTextMessage()
{
    QDialog dlg(this);
    dlg.setWindowTitle("发送文本");
    dlg.resize(560, 380);
    dlg.setWindowIcon(QIcon(":/localsend/logo-512.png"));

    QLabel* hint = new QLabel("文本将作为消息发送，对方客户端会直接显示内容：");
    QPlainTextEdit* edit = new QPlainTextEdit(&dlg);
    edit->setPlaceholderText("输入要发送的文本内容…");
    QDialogButtonBox* bb = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText("添加到发送列表");
    bb->button(QDialogButtonBox::Cancel)->setText("取消");

    QVBoxLayout* lay = new QVBoxLayout(&dlg);
    lay->addWidget(hint);
    lay->addWidget(edit, 1);
    lay->addWidget(bb);

    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    QString text = edit->toPlainText();
    if (text.trimmed().isEmpty()) {
        DMessageBox::information(this, "提示", "文本内容为空。");
        return;
    }

    OutgoingFile f;
    f.id = QString::number(m_selectedFiles.size());
    f.fileName = QUuid::createUuid().toString(QUuid::WithoutBraces) + ".txt";
    f.size = text.toUtf8().size();
    f.fileType = "text/plain";
    f.preview = text;                 // 官方协议：全文放 preview
    f.content = text.toUtf8();        // 上传内联字节
    m_selectedFiles.append(f);

    QString brief = text.simplified();
    if (brief.size() > 40) brief = brief.left(40) + "…";
    QListWidgetItem* item = new QListWidgetItem(
        QIcon::fromTheme("mail-send", QIcon::fromTheme("text-editor", QIcon(":/localsend/logo-32.png"))),
        QString("[文本消息] %1  (%2 字节)").arg(brief).arg(f.size));
    item->setToolTip(text);
    m_fileList->addItem(item);

    updateSendButton();
    Logger::log(QString("[GUI] 添加文本消息 %1 字符").arg(text.size()));
}

void MainWindow::addFilesToList(const QStringList& paths)
{
    QFileIconProvider fip;
    QMimeDatabase mimeDb;

    for (const QString& p : paths) {
        QFileInfo fi(p);
        if (!fi.exists()) continue;

        if (fi.isDir()) {
            QDirIterator it(p, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                QString sub = it.next();
                QFileInfo sfi(sub);
                OutgoingFile f;
                f.id = QString::number(m_selectedFiles.size());
                f.fileName = p.section("/", -1) + "/" + sfi.fileName();
                f.filePath = sub;
                f.size = sfi.size();
                f.fileType = mimeDb.mimeTypeForFile(sub).name();
                m_selectedFiles.append(f);

                QListWidgetItem* item = new QListWidgetItem(fip.icon(sfi),
                    QString("%1  (%2)").arg(f.fileName).arg(formatSize(f.size)));
                item->setToolTip(sub);
                m_fileList->addItem(item);
            }
            continue;
        }

        // 去重
        bool dup = false;
        for (const auto& f : m_selectedFiles)
            if (f.filePath == p) { dup = true; break; }
        if (dup) continue;

        OutgoingFile f;
        f.id = QString::number(m_selectedFiles.size());
        f.fileName = fi.fileName();
        f.filePath = p;
        f.size = fi.size();
        f.fileType = mimeDb.mimeTypeForFile(p).name();
        m_selectedFiles.append(f);

        QListWidgetItem* item = new QListWidgetItem(fip.icon(fi),
            QString("%1  (%2)").arg(f.fileName).arg(formatSize(f.size)));
        item->setToolTip(p);
        m_fileList->addItem(item);
    }
    updateSendButton();
}

void MainWindow::removeSelectedFiles()
{
    QList<QListWidgetItem*> sel = m_fileList->selectedItems();
    if (sel.isEmpty()) return;

    // 从后往前删，避免索引漂移
    QList<int> rows;
    for (QListWidgetItem* it : sel) rows.append(m_fileList->row(it));
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) {
        if (row < 0 || row >= m_selectedFiles.size()) continue;
        m_selectedFiles.removeAt(row);
        delete m_fileList->takeItem(row);
    }

    // 重新编号 id
    for (int i = 0; i < m_selectedFiles.size(); ++i)
        m_selectedFiles[i].id = QString::number(i);

    updateSendButton();
}

void MainWindow::clearFileList()
{
    if (m_selectedFiles.isEmpty()) return;
    m_selectedFiles.clear();
    m_fileList->clear();
    updateSendButton();
}

void MainWindow::updateSendButton()
{
    bool hasFiles = !m_selectedFiles.isEmpty();
    int devCount = m_deviceList->selectedItems().size();
    m_btnSend->setEnabled(hasFiles && devCount > 0);
    m_btnSend->setText(devCount > 1
                       ? QString("发送到 %1 台设备").arg(devCount)
                       : "发送到选定设备");

    // BUG1：文件列表中选中了几项就只发送几项；未选中任何项 = 发送全部
    QList<QListWidgetItem*> fileSel = m_fileList->selectedItems();
    int sendCount = fileSel.isEmpty() ? m_selectedFiles.size() : fileSel.size();

    if (!hasFiles && devCount == 0)
        m_sendStatus->setText("就绪 — 选择文件和目标设备后点击发送");
    else if (!hasFiles)
        m_sendStatus->setText("请先添加要发送的文件（可拖拽或点击「文件」按钮）");
    else if (devCount == 0)
        m_sendStatus->setText("请在设备列表中选择至少一个目标（可多选）");
    else if (fileSel.isEmpty())
        m_sendStatus->setText(QString("准备就绪：%1 个文件 → %2 台设备")
                                  .arg(m_selectedFiles.size()).arg(devCount));
    else
        m_sendStatus->setText(QString("准备就绪：发送选定的 %1 / 共 %2 个文件 → %3 台设备")
                                  .arg(sendCount).arg(m_selectedFiles.size()).arg(devCount));

    m_btnRemoveFile->setEnabled(hasFiles);
    m_btnClearFiles->setEnabled(hasFiles);
}

void MainWindow::sendSelected()
{
    QList<QListWidgetItem*> targets = m_deviceList->selectedItems();
    if (targets.isEmpty()) { DMessageBox::information(this, "提示", "请先选择一个目标设备"); return; }
    if (m_selectedFiles.isEmpty()) { DMessageBox::information(this, "提示", "请先选择文件"); return; }

    // BUG1：只发送「待发送文件列表」中被选中的项；未选中任何项则发送全部
    QList<OutgoingFile> toSend;
    QList<QListWidgetItem*> fileSel = m_fileList->selectedItems();
    if (fileSel.isEmpty()) {
        toSend = m_selectedFiles;
    } else {
        QList<int> rows;
        for (QListWidgetItem* it : fileSel) rows.append(m_fileList->row(it));
        std::sort(rows.begin(), rows.end());
        for (int row : rows)
            if (row >= 0 && row < m_selectedFiles.size())
                toSend.append(m_selectedFiles.at(row));
        // 重编 id 为连续值（协议里 fileId 作为 files 对象的 key 必须唯一）
        for (int i = 0; i < toSend.size(); ++i)
            toSend[i].id = QString::number(i);
    }

    if (toSend.isEmpty()) { DMessageBox::information(this, "提示", "没有可发送的文件"); return; }

    m_sendTasks.clear();
    m_sendProgress->setValue(0);
    m_btnSend->setEnabled(false);

    for (QListWidgetItem* it : targets) {
        QString fp = it->data(Qt::UserRole).toString();
        QString alias = it->text().section('\n', 0, 0).trimmed();
        quint32 taskId = m_core->sendFiles(fp, toSend);

        SendTask t;
        t.id = taskId;
        t.deviceAlias = alias;
        t.fileCount = toSend.size();
        m_sendTasks[taskId] = t;
    }

    Logger::log(QString("[GUI] 发起多选发送：%1 个目标，各 %2 个文件%3")
                    .arg(m_sendTasks.size()).arg(toSend.size())
                    .arg(fileSel.isEmpty() ? QString() : "（仅列表选中项）"));
    refreshSendStatus();
}

void MainWindow::onSendProgress(quint32 taskId, const QString& deviceAlias,
                                const QString& fileName, qint64 sent, qint64 total)
{
    if (!m_sendTasks.contains(taskId)) {
        SendTask t;
        t.id = taskId;
        t.deviceAlias = deviceAlias;
        m_sendTasks[taskId] = t;
    }
    SendTask& t = m_sendTasks[taskId];
    if (!deviceAlias.isEmpty()) t.deviceAlias = deviceAlias;
    if (total > 0) {
        int pct = int(100 * sent / total);
        if (pct > t.percent) t.percent = pct;
    }
    t.lastFile = fileName;
    refreshSendStatus();
}

void MainWindow::onSendFinished(quint32 taskId, const QString& deviceAlias,
                                bool ok, const QString& msg)
{
    if (!m_sendTasks.contains(taskId)) {
        SendTask t;
        t.id = taskId;
        t.deviceAlias = deviceAlias;
        m_sendTasks[taskId] = t;
    }
    SendTask& t = m_sendTasks[taskId];
    if (!deviceAlias.isEmpty()) t.deviceAlias = deviceAlias;
    t.finished = true;
    t.ok = ok;
    t.message = msg;
    if (ok) t.percent = 100;
    refreshSendStatus();

    // 全部任务结束 → 汇总提示
    bool allDone = true;
    int okCount = 0, failCount = 0;
    QStringList fails;
    for (const SendTask& x : m_sendTasks) {
        if (!x.finished) { allDone = false; break; }
        if (x.ok) ++okCount; else { ++failCount; fails.append(QString("%1：%2").arg(x.deviceAlias).arg(x.message)); }
    }
    if (allDone) {
        updateSendButton();
        if (failCount == 0)
            DMessageBox::information(this, "发送完成",
                QString("已成功发送到 %1 台设备。").arg(okCount));
        else
            DMessageBox::warning(this, "发送结束",
                QString("成功 %1 台，失败 %2 台：\n\n%3").arg(okCount).arg(failCount).arg(fails.join("\n")));
    }
}

void MainWindow::refreshSendStatus()
{
    int sum = 0, n = 0;
    QStringList parts;
    for (auto it = m_sendTasks.begin(); it != m_sendTasks.end(); ++it) {
        const SendTask& t = it.value();
        sum += t.percent; ++n;
        if (t.finished)
            parts.append(QString("%1 %2").arg(t.deviceAlias).arg(t.ok ? "✓" : ("✗ " + t.message)));
        else
            parts.append(QString("%1 %2%%3").arg(t.deviceAlias).arg(t.percent)
                             .arg(t.lastFile.isEmpty() ? "" : (" (" + t.lastFile + ")")));
    }
    m_sendProgress->setValue(n ? sum / n : 0);
    if (!parts.isEmpty())
        m_sendStatus->setText(parts.join("   |   "));
}

// ============================================================
// 接收逻辑
// ============================================================

void MainWindow::chooseSaveDir()
{
    QString dir = DFileDialog::getExistingDirectory(this, "选择保存目录", m_core->saveDir());
    if (!dir.isEmpty()) {
        m_core->setSaveDir(dir);
        m_saveDirLabel->setText("保存目录：" + dir);
        updateInfoBar(m_port);
        Logger::log("[GUI] 保存目录已改为 " + dir);
    }
}

void MainWindow::onReceiveStarted(const QString& sid, const QString& from)
{
    m_recvStatus->setText(QString("来自 %1 的传入传输…").arg(from));
    m_navList->setCurrentRow(0);
}

void MainWindow::onReceiveProgress(const QString& sid, const QString& name, qint64 r, qint64 t)
{
    QString key = sid + "|" + name;
    QListWidgetItem* item = m_recvItems.value(key);
    if (!item) {
        item = new QListWidgetItem(QIcon::fromTheme("document-save"),
                                   QString("[接收中] %1").arg(name));
        m_recvItems[key] = item;
        m_recvList->addItem(item);
    }
    int pct = (t > 0) ? int(100 * r / t) : 0;
    item->setText(QString("[%1%] %2  (%3/%4)").arg(pct).arg(name).arg(formatSize(r)).arg(formatSize(t)));
    m_recvList->scrollToBottom();
}

void MainWindow::onReceiveFinished(const QString& sid, const QString& name, const QString& path)
{
    QString key = sid + "|" + name;
    QListWidgetItem* item = m_recvItems.value(key);

    QFileIconProvider fip;
    QIcon ico = fip.icon(QFileInfo(path));

    if (item) {
        item->setIcon(ico);
        item->setText(QString("[完成] %1  →  %2").arg(name).arg(path));
    } else {
        item = new QListWidgetItem(ico, QString("[完成] %1  →  %2").arg(name).arg(path));
        m_recvItems[key] = item;
        m_recvList->addItem(item);
    }
    item->setData(Qt::UserRole, path);
    item->setToolTip(QString("完整路径：%1\n\n双击打开；右键可「打开所在目录」/「复制路径」").arg(path));

    m_recvStatus->setText("接收完成：" + name + "\n（双击打开，或右键选择「打开所在目录」）");
    m_recvList->scrollToBottom();
}

// 收到文本消息：在接收列表中显示为消息条目，双击弹窗查看全文
void MainWindow::onMessageReceived(const QString& sid, const QString& senderAlias,
                                   const QString& content)
{
    // BUG3：key 加入时间戳防碰撞（sid/别名都可能重复，同内容消息也要各占一行）
    QString key = sid + "|" + senderAlias + "|" + QString::number(QDateTime::currentMSecsSinceEpoch());
    QListWidgetItem* item = new QListWidgetItem;
    m_recvItems[key] = item;
    m_recvList->addItem(item);

    QString brief = content.simplified();
    if (brief.size() > 60) brief = brief.left(60) + "…";
    item->setIcon(QIcon::fromTheme("mail-receive", QIcon::fromTheme("mail-send", QIcon(":/localsend/logo-32.png"))));
    // BUG3：%2 用发送方名字（此前只有一个 arg，%2 原样残留）
    item->setText(QString("[文本消息] %1  (来自 %2)").arg(brief, senderAlias));
    item->setData(Qt::UserRole, QString());          // 不按文件打开
    item->setData(Qt::UserRole + 1, content);        // 全文存 UserRole+1
    item->setData(Qt::UserRole + 2, senderAlias);    // 发送方名字存 UserRole+2（弹窗标题用）
    item->setToolTip(QString("文本消息全文：\n%1\n\n双击查看").arg(content));

    m_recvStatus->setText("收到文本消息\n（双击条目查看全文）");
    m_recvList->scrollToBottom();
    Logger::log(QString("[收] 文本消息 %1 字符（来自 %2），已显示在接收列表")
                    .arg(content.size()).arg(senderAlias));
}

// ---- BUG4：文本消息查看弹窗（普通 QDialog，绕开 DMessageBox 标题乱码）----

void MainWindow::showMessageDialog(const QString& senderAlias, const QString& content)
{
    QDialog dlg(this);
    dlg.setWindowTitle(senderAlias.isEmpty()
                       ? QString("文本消息")
                       : QString("文本消息 · 来自 %1").arg(senderAlias));
    dlg.setWindowIcon(QIcon(":/localsend/logo-512.png"));
    dlg.resize(560, 420);

    QVBoxLayout* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(14, 12, 14, 10);
    lay->setSpacing(8);

    QLabel* hint = new QLabel(senderAlias.isEmpty()
                              ? QString("文本内容（可选中复制）：")
                              : QString("来自 %1 的文本消息（可选中复制）：").arg(senderAlias));
    hint->setStyleSheet("color: #666; font-size: 12px;");
    lay->addWidget(hint);

    QTextEdit* edit = new QTextEdit(&dlg);
    edit->setReadOnly(true);
    edit->setPlainText(content);
    edit->setLineWrapMode(QTextEdit::WidgetWidth);
    // 关键：允许鼠标/键盘选中文字，方便手动复制
    edit->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    lay->addWidget(edit, 1);

    QDialogButtonBox* bb = new QDialogButtonBox(&dlg);
    QPushButton* btnCopy = bb->addButton("复制全部", QDialogButtonBox::ActionRole);
    QPushButton* btnClose = bb->addButton("关闭", QDialogButtonBox::RejectRole);
    connect(btnCopy, &QPushButton::clicked, btnCopy, [btnCopy, content]() {
        QApplication::clipboard()->setText(content);
        btnCopy->setText("已复制 ✓");
        QTimer::singleShot(1200, btnCopy, [btnCopy]() { btnCopy->setText("复制全部"); });
    });
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
    btnClose->setDefault(true);
    lay->addWidget(bb);

    dlg.exec();
}

// ---- 接收列表：打开 / 定位 ----

void MainWindow::onRecvItemActivated(QListWidgetItem* item)
{
    // 文本消息：专用弹窗（标题正常、文字可选中、可一键复制）
    QString msg = item->data(Qt::UserRole + 1).toString();
    if (!msg.isEmpty()) {
        showMessageDialog(item->data(Qt::UserRole + 2).toString(), msg);
        return;
    }
    QString path = item->data(Qt::UserRole).toString();
    if (path.isEmpty()) {
        DMessageBox::information(this, "提示", "该文件尚未接收完成。");
        return;
    }
    if (!QFileInfo(path).exists()) {
        DMessageBox::warning(this, "提示", "文件已不存在：\n" + path);
        return;
    }
    if (!openWithDefaultApp(path))
        DMessageBox::warning(this, "打开失败", "未能调用默认程序打开文件：\n" + path);
}

void MainWindow::showRecvContextMenu(const QPoint& pos)
{
    QListWidgetItem* item = m_recvList->itemAt(pos);
    if (!item) return;

    QString path = item->data(Qt::UserRole).toString();
    bool ready = !path.isEmpty() && QFileInfo(path).exists();

    QMenu menu(this);
    QAction* actOpen = menu.addAction(QIcon::fromTheme("document-open"), "打开该文件");
    QAction* actFolder = menu.addAction(QIcon::fromTheme("folder-open"), "打开所在目录");
    menu.addSeparator();
    QAction* actCopy = menu.addAction(QIcon::fromTheme("edit-copy"), "复制完整路径");
    actOpen->setEnabled(ready);
    actFolder->setEnabled(ready);
    actCopy->setEnabled(!path.isEmpty());

    QAction* chosen = menu.exec(m_recvList->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == actOpen) {
        if (!openWithDefaultApp(path))
            DMessageBox::warning(this, "打开失败", "未能调用默认程序打开文件：\n" + path);
    } else if (chosen == actFolder) {
        if (!revealInFolder(path))
            DMessageBox::warning(this, "打开失败", "未能打开文件管理器：\n" + path);
    } else if (chosen == actCopy) {
        QApplication::clipboard()->setText(path);
        m_recvStatus->setText("已复制路径：" + path);
    }
}

void MainWindow::openSelectedFile()
{
    QListWidgetItem* item = m_recvList->currentItem();
    if (!item) { DMessageBox::information(this, "提示", "请先在接收列表中选择一个文件"); return; }
    onRecvItemActivated(item);
}

void MainWindow::revealSelectedFile()
{
    QListWidgetItem* item = m_recvList->currentItem();
    if (!item) { DMessageBox::information(this, "提示", "请先在接收列表中选择一个文件"); return; }
    QString path = item->data(Qt::UserRole).toString();
    if (path.isEmpty() || !QFileInfo(path).exists()) {
        DMessageBox::warning(this, "提示", "文件尚未接收完成或已不存在。");
        return;
    }
    if (!revealInFolder(path))
        DMessageBox::warning(this, "打开失败", "未能打开文件管理器：\n" + path);
}

void MainWindow::copySelectedPath()
{
    QListWidgetItem* item = m_recvList->currentItem();
    if (!item) return;
    QString path = item->data(Qt::UserRole).toString();
    if (!path.isEmpty()) QApplication::clipboard()->setText(path);
}

void MainWindow::clearRecvList()
{
    m_recvList->clear();
    m_recvItems.clear();
    m_recvStatus->setText("监听中，等待传入传输…\n（所有传输都需要你手动确认接受）");
}

// ============================================================
// 接收确认对话框（核心安全功能）
// ============================================================

// ---- BUG2：文本消息接收确认框 —— 一句话 + 三按钮「同意 / 拒绝 / 一律拒绝」----
// 返回：1=同意  0=拒绝  2=一律拒绝（Esc/关闭等同拒绝）

int MainWindow::askReceiveTextMessage(const QString& peerAlias)
{
    QDialog dlg(this);
    dlg.setWindowTitle("收到文本消息");
    dlg.setWindowIcon(QIcon(":/localsend/logo-512.png"));
    dlg.setMinimumWidth(380);

    QVBoxLayout* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(18, 16, 18, 12);
    lay->setSpacing(12);

    QLabel* tip = new QLabel(QString("%1希望给你发送文本消息，是否接收？").arg(peerAlias));
    tip->setWordWrap(true);
    tip->setStyleSheet("font-size: 14px;");
    lay->addWidget(tip);
    lay->addStretch();

    QDialogButtonBox* bb = new QDialogButtonBox(&dlg);
    QPushButton* btnAccept = bb->addButton("同意", QDialogButtonBox::AcceptRole);
    QPushButton* btnReject = bb->addButton("拒绝", QDialogButtonBox::RejectRole);
    QPushButton* btnBlock  = bb->addButton("一律拒绝", QDialogButtonBox::ActionRole);
    btnReject->setDefault(true);   // 默认聚焦「拒绝」，防止误点同意
    lay->addWidget(bb);

    int choice = 0;
    connect(btnAccept, &QPushButton::clicked, &dlg, [&]() { choice = 1; dlg.accept(); });
    connect(btnBlock,  &QPushButton::clicked, &dlg, [&]() { choice = 2; dlg.accept(); });
    connect(btnReject, &QPushButton::clicked, &dlg, [&]() { choice = 0; dlg.reject(); });
    connect(&dlg, &QDialog::rejected, &dlg, [&]() { choice = 0; });

    dlg.exec();
    return choice;
}

void MainWindow::onReceiveRequest(const QString& pendingId,
                                  const QString& peerAlias,
                                  const QStringList& fileNames,
                                  const QMap<QString, qint64>& fileSizes,
                                  const QMap<QString, QString>& filePreviews,
                                  const QString& peerFingerprint)
{
    m_navList->setCurrentRow(0);

    QString filesText;
    qint64 totalSize = 0;
    for (int i = 0; i < fileNames.size(); ++i) {
        QString fname = fileNames.at(i);
        qint64 fsize = fileSizes.value(fname, 0);
        if (fsize == 0 && i < fileSizes.size())
            fsize = fileSizes.values().at(i);
        totalSize += fsize;

        // 文本消息：直接显示内容摘要，而不是 UUID.txt 文件名
        QString pv = filePreviews.value(fname);
        if (pv.isEmpty()) {
            // fileId 与 fileName 未必相同，按内容匹配一次
            for (auto it = filePreviews.begin(); it != filePreviews.end(); ++it)
                if (it.value() == fname) { pv = it.value(); break; }
        }
        if (!pv.isEmpty()) {
            QString brief = pv.simplified();
            if (brief.size() > 80) brief = brief.left(80) + "…";
            filesText += QString("  • [文本消息] %1\n").arg(brief);
        } else {
            filesText += QString("  • %1 (%2)\n").arg(fname).arg(formatSize(fsize));
        }
    }

    // 纯消息传输（所有条目带 preview）：措辞改为「消息」，确认后不落盘
    bool isMessage = !filePreviews.isEmpty();
    for (auto it = filePreviews.begin(); it != filePreviews.end(); ++it)
        if (it.value().isEmpty()) { isMessage = false; break; }

    // ---- BUG2：文本消息走精简确认框（同意 / 拒绝 / 一律拒绝）----
    if (isMessage) {
        QString bk = blockKey(peerFingerprint, peerAlias);
        if (m_blockedTextSenders.contains(bk)) {
            // 此前选过「一律拒绝」：重启前对同一发送方静默自动拒绝
            m_server->rejectPrepare(pendingId);
            m_recvStatus->setText(QString("已自动拒绝来自 %1 的文本消息（此前已选「一律拒绝」）")
                                      .arg(peerAlias));
            Logger::log(QString("[GUI] 文本消息被「一律拒绝」规则拦截，来自 %1 (%2)")
                            .arg(peerAlias).arg(bk));
            return;
        }

        int ret = askReceiveTextMessage(peerAlias);
        if (ret == 1) {
            m_server->acceptPrepare(pendingId);
            m_recvStatus->setText(QString("已收到来自 %1 的文本消息").arg(peerAlias));
            Logger::log("[GUI] 用户接受文本消息，来自 " + peerAlias);
        } else if (ret == 2) {
            m_blockedTextSenders.insert(bk);
            m_server->rejectPrepare(pendingId);
            m_recvStatus->setText(QString("已「一律拒绝」来自 %1 的文本消息（重启前对该发送方自动拒绝）")
                                      .arg(peerAlias));
            Logger::log(QString("[GUI] 用户「一律拒绝」文本消息，来自 %1 (%2)")
                            .arg(peerAlias).arg(bk));
        } else {
            m_server->rejectPrepare(pendingId);
            m_recvStatus->setText("已拒绝来自 " + peerAlias + " 的文本消息");
            Logger::log("[GUI] 用户拒绝文本消息，来自 " + peerAlias);
        }
        return;
    }

    QString detail = QString("发送方：%1\n发送方指纹：%2%3\n\n%4：\n%5%6")
                        .arg(peerAlias)
                        .arg(peerFingerprint.isEmpty() ? "（对方未提供）" : peerFingerprint)
                        .arg(peerFingerprint.isEmpty() ? QString() : QString("（可在对方设备「验证」页与本机指纹比对）"))
                        .arg(isMessage ? "消息内容" : QString("文件列表（共 %1 个，%2）").arg(fileNames.size()).arg(formatSize(totalSize)))
                        .arg(filesText)
                        .arg(isMessage ? QString("\n接受后将直接显示为消息（不保存文件）。")
                                       : QString("\n保存到：%1").arg(m_core->saveDir()));

    m_recvStatus->setText(QString("⚠ %1 请求发送 %2，等待你确认…")
                              .arg(peerAlias)
                              .arg(isMessage ? "一条消息" : QString("%1 个文件").arg(fileNames.size())));

    int ret = DMessageBox::question(this, isMessage ? "收到文本消息" : "收到传输请求",
        QString("%1 请求向你发送%2。\n\n%3\n\n是否接受？")
            .arg(peerAlias)
            .arg(isMessage ? "一条文本消息" : "文件")
            .arg(detail),
        DMessageBox::Yes | DMessageBox::No, DMessageBox::No);

    if (ret == DMessageBox::Yes) {
        m_server->acceptPrepare(pendingId);
        m_recvStatus->setText(isMessage
                              ? QString("已收到来自 %1 的文本消息").arg(peerAlias)
                              : QString("已接受来自 %1 的传输，接收中…").arg(peerAlias));
        Logger::log(QString("[GUI] 用户接受%1，来自 %2")
                        .arg(isMessage ? "文本消息" : "传输请求").arg(peerAlias));
    } else {
        m_server->rejectPrepare(pendingId);
        m_recvStatus->setText("已拒绝来自 " + peerAlias + " 的传输请求");
        Logger::log("[GUI] 用户拒绝传输请求，来自 " + peerAlias);
    }
}

// ============================================================
// 设置页动作
// ============================================================

void MainWindow::applyAlias()
{
    QString a = m_aliasEdit->text().trimmed();
    if (a.isEmpty()) { DMessageBox::information(this, "设置", "别名不能为空。"); return; }
    m_core->setAlias(a);
    updateInfoBar(m_port);
    DMessageBox::information(this, "设置", "别名已更新并重新广播。");
}

void MainWindow::openLogDir()
{
    revealInFolder(Logger::filePath());
}

void MainWindow::showLogDialog()
{
    QDialog dlg(this);
    dlg.setWindowTitle("调试日志 — " + Logger::filePath());
    dlg.resize(860, 560);
    dlg.setWindowIcon(QIcon(":/localsend/logo-512.png"));

    QTextEdit* view = new QTextEdit(&dlg);
    view->setReadOnly(true);
    view->setLineWrapMode(QTextEdit::NoWrap);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(9);
    view->setFont(mono);
    view->setPlainText(Logger::tail(600).join("\n"));
    view->moveCursor(QTextCursor::End);
    view->ensureCursorVisible();

    QPushButton* btnRefresh = new QPushButton("刷新");
    QPushButton* btnCopy = new QPushButton("复制全部");
    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Close);
    bb->addButton(btnRefresh, QDialogButtonBox::ActionRole);
    bb->addButton(btnCopy, QDialogButtonBox::ActionRole);

    QVBoxLayout* lay = new QVBoxLayout(&dlg);
    lay->addWidget(view, 1);
    lay->addWidget(bb);

    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
    connect(btnRefresh, &QPushButton::clicked, [view]() {
        view->setPlainText(Logger::tail(600).join("\n"));
        view->moveCursor(QTextCursor::End);
        view->ensureCursorVisible();
    });
    connect(btnCopy, &QPushButton::clicked, [view]() {
        QApplication::clipboard()->setText(view->toPlainText());
    });

    dlg.exec();
}

// ============================================================
// 辅助
// ============================================================

void MainWindow::updateInfoBar(quint16 port)
{
    QString http = m_server->isListening()
        ? QString("HTTP:%1 ✓").arg(port)
        : QString("HTTP:%1 ✗").arg(port);
    m_infoBar->setText(
        QString(" %1  |  IP %2  |  组播 %3  |  保存 %4  |  日志 ~/.local/share/localsend-qt/localsend.log")
            .arg(http).arg(m_core->localIp())
            .arg(m_core->multicastSummary()).arg(m_core->saveDir()));
}

void MainWindow::onLog(const QString& msg)
{
    Logger::log(msg);
}

QIcon MainWindow::deviceIcon(const QString& deviceType) const
{
    static QIcon fallback(":/localsend/logo-32.png");
    QString t = deviceType.toLower();
    if (t == "mobile" || t == "phone")
        return QIcon::fromTheme("phone", QIcon::fromTheme("smartphone", fallback));
    if (t == "tablet")
        return QIcon::fromTheme("tablet", fallback);
    if (t == "desktop" || t == "computer")
        return QIcon::fromTheme("computer", QIcon::fromTheme("video-display", fallback));
    if (t == "web")
        return QIcon::fromTheme("applications-internet", fallback);
    if (t == "headless" || t == "server")
        return QIcon::fromTheme("network-server", fallback);
    return fallback;
}

QString MainWindow::formatSize(qint64 bytes) const
{
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024 * 1024 * 1024) return QString("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
    return QString("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
}
