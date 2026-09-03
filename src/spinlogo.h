#ifndef SPINLOGO_H
#define SPINLOGO_H

#include <QWidget>
#include <QTimer>

// 旋转动态 Logo：模仿官方 LocalSend 接收页的圆环动画。
// 自绘 8 段圆弧 + 中心圆点，带渐隐尾迹，持续旋转（无外部资源依赖）。
class SpinLogo : public QWidget
{
    Q_OBJECT
public:
    explicit SpinLogo(QWidget* parent = nullptr);

    void start();
    void stop();

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onTick();

private:
    QTimer* m_timer = nullptr;
    int m_angle = 0;
};

#endif // SPINLOGO_H
