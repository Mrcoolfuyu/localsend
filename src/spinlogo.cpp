#include "spinlogo.h"

#include <QPainter>
#include <QPaintEvent>

SpinLogo::SpinLogo(QWidget* parent)
    : QWidget(parent)
{
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &SpinLogo::onTick);
}

void SpinLogo::start()
{
    if (!m_timer->isActive()) m_timer->start(40);   // 25 fps
}

void SpinLogo::stop()
{
    m_timer->stop();
}

void SpinLogo::onTick()
{
    m_angle = (m_angle + 6) % 360;   // 约 2.5 秒一圈
    update();
}

void SpinLogo::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int side = qMin(width(), height());
    if (side <= 0) return;
    p.translate(width() / 2.0, height() / 2.0);
    p.rotate(m_angle);

    // 官方 LocalSend 主色调（深青绿）
    QColor teal(16, 124, 110);

    // 8 段圆弧，带渐隐尾迹（越靠后越淡），旋转时呈现流动感
    double rOut = side * 0.36;
    double penW = side * 0.115;
    QRectF arcRect(-rOut, -rOut, rOut * 2, rOut * 2);
    for (int i = 0; i < 8; ++i) {
        p.save();
        p.rotate(i * 45.0);
        int alpha = 255 - i * 30;
        if (alpha < 50) alpha = 50;
        QColor c = teal;
        c.setAlpha(alpha);
        QPen pen(c, penW);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        // 每段短弧：起始 12° 处画 20°
        p.drawArc(arcRect, 12 * 16, 20 * 16);
        p.restore();
    }

    // 中心实心圆点
    p.setPen(Qt::NoPen);
    p.setBrush(teal);
    double rCore = side * 0.155;
    p.drawEllipse(QPointF(0, 0), rCore, rCore);
}
