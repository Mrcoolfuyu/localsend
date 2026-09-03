#include "mainwindow.h"
#include "logger.h"

#include <DApplication>
#include <DWidgetUtil>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QSslSocket>
#include <QCoreApplication>
#include <QTimer>
#include <QPixmap>
#include <QStringList>

int main(int argc, char* argv[])
{
    // 1) 最早安装日志：此后所有 qDebug/qWarning 都会落盘
    //    日志固定位于 ~/.local/share/localsend-qt/localsend.log
    Logger::install("0.3.4");

    Dtk::Widget::DApplication a(argc, argv);

    // LocalSend 仅用于局域网通信，必须强制直连，禁止走系统/全局 SOCKS 代理。
    // 否则 QTcpServer::listen() 会把监听请求发给 SOCKS5 代理，代理不支持 BIND 命令，
    // 报 "socksv5 command not supported"，导致 53317 及所有回退端口都无法监听。
    QNetworkProxyFactory::setUseSystemConfiguration(false);
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));

    a.setOrganizationName("localsend");
    a.setApplicationName("LocalSend");
    a.setApplicationDisplayName("LocalSend");
    a.setApplicationVersion("0.3.4");
    // DTK「关于」对话框的主页默认显示 www.chinauos.com，必须显式覆盖为项目主页
    a.setApplicationHomePage("https://github.com/Mrcoolfuyu/");
    a.setWindowIcon(QIcon(":/localsend/logo-512.png"));
    a.setProductIcon(QIcon(":/localsend/logo-512.png"));
    a.loadTranslator();   // 自动加载 DTK/qt 翻译

    Logger::log(QString("[启动] 命令行      : %1").arg(a.arguments().join(" ")));
    Logger::log(QString("[GUI] 代理已强制设为 NoProxy；SSL 支持=%1")
                    .arg(QSslSocket::supportsSsl() ? "是" : "否"));

    MainWindow w;
    w.show();
    Dtk::Widget::moveToCenter(&w);

    // 调试参数：无显示器环境（Xvfb / CI）下验证界面渲染
    //   --screenshot <png路径> [--shot-delay <毫秒，默认 5000>] [--page <0接收|1发送|2设置>]
    const QStringList args = a.arguments();
    QString shotPath;
    int shotDelay = 5000;
    int page = -1;
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == "--screenshot" && i + 1 < args.size())
            shotPath = args.at(++i);
        else if (args.at(i) == "--shot-delay" && i + 1 < args.size())
            shotDelay = args.at(++i).toInt();
        else if (args.at(i) == "--page" && i + 1 < args.size())
            page = args.at(++i).toInt();
    }
    if (page >= 0 && page <= 2)
        w.showPage(page);
    if (!shotPath.isEmpty()) {
        Logger::log(QString("[GUI] 调试截图模式：%1 ms 后保存到 %2").arg(shotDelay).arg(shotPath));
        QTimer::singleShot(shotDelay, [&w, shotPath]() {
            QPixmap px = w.grab();
            bool saved = px.save(shotPath);
            Logger::log(QString("[GUI] 截图保存 %1：%2 (%3x%4)")
                            .arg(shotPath).arg(saved ? "成功" : "失败")
                            .arg(px.width()).arg(px.height()));
            // 不用 qApp 宏：DTK 把它重定义为 Dtk::Widget::DApplication*，
            // 在全局命名空间下未 using 该命名空间时会编译失败。
            QCoreApplication::quit();
        });
    }

    return a.exec();
}
