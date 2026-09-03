#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QStringList>

// 轻量文件日志模块：
//  - Logger::install() 安装 Qt 消息处理器，把所有 qDebug/qWarning/qCritical 落盘
//  - Logger::log() 供业务代码主动写入（也被 LocalSend::logMessage 转发）
//  - 日志固定写在 ~/.local/share/localsend-qt/localsend.log，超过 2MB 自动轮转 3 份
class Logger
{
public:
    // 安装消息处理器并写入启动横幅。可重复调用（幂等）。
    static void install(const QString& appName = "LocalSend (UOS)");

    // 主动写一条日志
    static void log(const QString& msg);

    // 日志文件绝对路径
    static QString filePath();

    // 日志所在目录
    static QString dirPath();

    // 读取最后 n 行（供 GUI "查看日志" 对话框使用）
    static QStringList tail(int lines = 400);

    // 清空当前日志
    static void clear();
};

#endif // LOGGER_H
