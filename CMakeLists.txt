cmake_minimum_required(VERSION 3.10)
project(localsend-qt VERSION 0.3.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 兼容 UOS 20 自带的 Qt5（5.11/5.15）。如系统为 Qt6，可改为 Qt6 组件。
find_package(Qt5 COMPONENTS Widgets Network REQUIRED)

# 公共源：协议核心 + HTTP 服务 + 日志
set(LS_COMMON_SRC
    src/localsend.cpp
    src/httpserver.cpp
    src/logger.cpp
)

# 协议无界面自测：始终构建，仅依赖 Qt5::Network，可在无 DTK 的环境（如 WSL）验证协议链路
add_executable(localsend-selftest src/selftest.cpp ${LS_COMMON_SRC})
target_link_libraries(localsend-selftest PRIVATE Qt5::Widgets Qt5::Network)
target_include_directories(localsend-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 集成测试：验证真实 UDP 组播发现（announcement/register）+ 传输，无需 DTK
add_executable(localsend-itest src/itest.cpp ${LS_COMMON_SRC})
target_link_libraries(localsend-itest PRIVATE Qt5::Widgets Qt5::Network)
target_include_directories(localsend-itest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# HTTPS 专项测试：连接自签名证书的 https 对端（模拟官方 LocalSend 的 https 模式）
# 配套测试服务端：tools/fake_https_peer.py（普通 TLS）/
#               tools/fake_https_mtls.py（强制要求客户端证书，复现官方行为）
add_executable(localsend-ssltest src/ssltest.cpp ${LS_COMMON_SRC}
    resources/localsend.qrc)
target_link_libraries(localsend-ssltest PRIVATE Qt5::Widgets Qt5::Network)
target_include_directories(localsend-ssltest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# GUI 目标：DTK 原生界面，需统信 DTK 开发包（UOS 自带）。
# 无 DTK 的环境（如普通 Ubuntu/WSL）用 -DBUILD_GUI=OFF 仅构建 selftest。
option(BUILD_GUI "Build the DTK GUI (requires UOS DTK dev packages)" ON)
if(BUILD_GUI)
    find_package(Dtk REQUIRED COMPONENTS Widget)
    add_executable(localsend-qt
        src/main.cpp
        src/mainwindow.cpp
        src/spinlogo.cpp
        ${LS_COMMON_SRC}
        resources/localsend.qrc
    )
    target_link_libraries(localsend-qt PRIVATE Qt5::Widgets Qt5::Network Dtk::Widget)
    target_include_directories(localsend-qt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    # 安装：二进制 + 官方 LOGO + 桌面入口
    install(TARGETS localsend-qt RUNTIME DESTINATION bin)
    install(FILES resources/icons/logo-512.png
            DESTINATION share/pixmaps RENAME localsend-qt.png)
endif()
