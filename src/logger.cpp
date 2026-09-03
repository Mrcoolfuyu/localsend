#include "logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>
#include <QtGlobal>
#include <QSslSocket>

static QMutex g_mutex;
static QString g_path;
static QFile* g_file = nullptr;
static bool g_installed = false;

static const qint64 MAX_SIZE = 2 * 1024 * 1024;   // 2MB 后轮转
static const int    MAX_KEEP = 3;

static void rotateIfNeeded()
{
    QFileInfo fi(g_path);
    if (!fi.exists() || fi.size() < MAX_SIZE) return;

    // localsend.log.3 -> 丢弃；.2 -> .3；.1 -> .2；当前 -> .1
    QFile::remove(g_path + QString(".%1").arg(MAX_KEEP));
    for (int i = MAX_KEEP - 1; i >= 1; --i) {
        QString from = (i == 1) ? g_path : g_path + QString(".%1").arg(i);
        QString to   = g_path + QString(".%1").arg(i + 1);
        QFile::remove(to);
        QFile::rename(from, to);
    }
}

// 内部实现：调用前必须已持有 g_mutex
static void writeLineLocked(const QString& line)
{
    if (g_path.isEmpty()) return;

    if (!g_file) {
        rotateIfNeeded();
        g_file = new QFile(g_path);
        if (!g_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            delete g_file; g_file = nullptr;
            return;
        }
    }
    if (!g_file->isOpen()) return;

    QTextStream ts(g_file);
    ts.setCodec("UTF-8");
    ts << line << "\n";
    ts.flush();
    g_file->flush();
}

static void writeLine(const QString& line)
{
    QMutexLocker lk(&g_mutex);
    writeLineLocked(line);
}

static void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    static const char* kLevel[] = { "DBG ", "WARN", "ERR ", "FATAL", "INFO" };
    QString lv = (type >= 0 && type <= 4) ? kLevel[type] : "????";

    QString where;
    if (ctx.function && *ctx.function) {
        where = QString(ctx.function);
        int p = where.indexOf('(');
        if (p > 0) where = where.left(p);
        // 只保留最后一段（类名::函数名）
        int sp = where.lastIndexOf("::");
        if (sp > 0) where = where.mid(sp + 2);
        if (where.length() > 40) where = where.left(40);
        where = " [" + where + "]";
    }

    QString line = QString("[%1] %2%3  %4")
                       .arg(QDateTime::currentDateTime().toString("MM-dd hh:mm:ss.zzz"))
                       .arg(lv).arg(where).arg(msg);

    writeLine(line);

    // 同时输出到 stderr，方便终端里排查
    QTextStream err(stderr);
    err << line << "\n";
    err.flush();
}

void Logger::install(const QString& appName)
{
    {
        QMutexLocker lk(&g_mutex);
        if (g_installed) return;

        QString dir = QDir::homePath() + "/.local/share/localsend-qt";
        QDir().mkpath(dir);
        g_path = dir + "/localsend.log";

        qInstallMessageHandler(messageHandler);
        g_installed = true;
    }

    // 注意：写入横幅时不能再持有 g_mutex —— writeLine 会自行加锁，
    // 而 QMutex 是非递归的，重复加锁会立即自死锁（表现为进程卡死/静默退出）。
    // 启动横幅：每次进程启动都能在日志里定位
    writeLine("");
    writeLine("=========================================================");
    writeLine(QString("[%1] LocalSend 启动")
                  .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz")));
    writeLine(QString("  版本        : %1").arg(appName));
    writeLine(QString("  进程 PID    : %1").arg(QCoreApplication::applicationPid()));
    // 注意：命令行参数由调用方在 QApplication 创建后补记 —— install() 通常早于
    // QApplication 构造，此时调 QCoreApplication::arguments() 会产生告警。
    writeLine(QString("  Qt 版本     : %1").arg(qVersion()));
    writeLine(QString("  SSL 支持    : %1").arg(QSslSocket::supportsSsl() ? "是" : "否"));
    if (QSslSocket::supportsSsl()) {
        writeLine(QString("  SSL 后端    : %1")
                      .arg(QSslSocket::sslLibraryVersionString()));
    } else {
        writeLine("  ！警告：Qt 无 SSL 支持，将无法连接 https 对端，只能走 http 回退");
    }
    writeLine(QString("  用户名/HOME : %1 / %2").arg(qgetenv("USER").constData()).arg(QDir::homePath()));
    writeLine("=========================================================");
}

void Logger::log(const QString& msg)
{
    writeLine(QString("[%1] INFO   %2")
                  .arg(QDateTime::currentDateTime().toString("MM-dd hh:mm:ss.zzz"))
                  .arg(msg));
}

QString Logger::filePath()
{
    QMutexLocker lk(&g_mutex);
    if (g_path.isEmpty())
        g_path = QDir::homePath() + "/.local/share/localsend-qt/localsend.log";
    return g_path;
}

QString Logger::dirPath()
{
    return QFileInfo(filePath()).absolutePath();
}

QStringList Logger::tail(int lines)
{
    QMutexLocker lk(&g_mutex);
    QFile f(g_path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QStringList();

    // 简单读取全部后取尾部（日志最多 2MB，开销可接受）
    QTextStream ts(&f);
    ts.setCodec("UTF-8");
    QStringList all = ts.readAll().split('\n');
    if (all.size() > lines) all = all.mid(all.size() - lines);
    return all;
}

void Logger::clear()
{
    QMutexLocker lk(&g_mutex);
    if (g_file) { g_file->close(); delete g_file; g_file = nullptr; }
    QFile::remove(g_path);
}
