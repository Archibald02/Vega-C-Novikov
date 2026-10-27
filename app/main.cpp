#include "main_window.hpp"
#include <QApplication>
#include <QDir>
#include <QTimer>
#include <cstdio>
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    QCoreApplication::setOrganizationName("QuantPriceLab"); QCoreApplication::setApplicationName("QuantPriceLive");
    const auto args=app.arguments(); const bool smoke=args.contains("--smoke-test"), moex=args.contains("--check-moex");
    live::MainWindow window(smoke || moex); window.show();
    if (smoke || moex) {
        if (smoke) window.start_demo_test(); else window.start_moex_test();
        QTimer::singleShot(smoke ? 1800 : 14000,&app,[&] {
            const bool ok=smoke ? window.has_demo_data() : window.has_any_data();
            const int index=args.indexOf("--screenshot");
            if (index>=0 && index+1<args.size()) window.grab().save(args[index+1]);
            std::printf("%s: %s\n",smoke ? "GUI smoke test" : "MOEX online check",ok ? "PASS" : "FAIL");
            app.exit(ok ? 0 : 1);
        });
    }
    return app.exec();
}
