#include "main_window.hpp"
#include "instrument_catalog.hpp"
#include <QApplication>
#include <QTimer>
#include <cstdio>
int main(int argc,char**argv){
    QApplication app(argc,argv);QCoreApplication::setOrganizationName("QuantPriceLab");QCoreApplication::setApplicationName("QuantPriceLive");
    const auto args=app.arguments();const bool smoke=args.contains("--smoke-test"),moex=args.contains("--check-moex"),tools=args.contains("--tools-test"),stress=args.contains("--stress-test"),profile=args.contains("--profile");
    if(args.contains("--check-catalog")){
        live::InstrumentCatalog catalog;
        QObject::connect(&catalog,&live::InstrumentCatalog::updated,&app,[&]{std::printf("Catalog online check: %lld instruments\n",static_cast<long long>(catalog.instruments().size()));app.exit(catalog.instruments().isEmpty()?1:0);});
        QObject::connect(&catalog,&live::InstrumentCatalog::failed,&app,[&](const QString&){std::printf("Catalog online check: FAIL\n");app.exit(1);});
        catalog.load(true);QTimer::singleShot(20000,&app,[&]{app.exit(1);});return app.exec();
    }
    live::MainWindow window(smoke||moex||tools||stress||profile);window.show();
    if(smoke||moex||tools||stress||profile){
        if(moex)window.start_moex_test();else window.start_demo_test(stress||profile?20:4);
        if(stress)QTimer::singleShot(100,&app,[&]{const bool ok=window.stress_test();std::printf("Bounded history stress (500000 observations): %s\n",ok?"PASS":"FAIL");app.exit(ok?0:1);});
        else if(profile){auto* sample_timer=new QTimer(&app);QObject::connect(sample_timer,&QTimer::timeout,&app,[&]{std::printf("%s\n",window.performance_status().toUtf8().constData());});sample_timer->start(1000);QTimer::singleShot(15000,&app,[&]{std::printf("Profile finished; chart rebuilds: %llu\n",static_cast<unsigned long long>(window.chart_rebuilds()));app.exit(0);});}
        else QTimer::singleShot(smoke||tools?1800:14000,&app,[&]{const bool ok=moex?window.has_any_data():tools?window.exercise_tools_test():window.has_demo_data();
            QTimer::singleShot(tools?650:100,&app,[&,ok]{const auto rebuilds=window.chart_rebuilds();QTimer::singleShot(tools?650:0,&app,[&,ok,rebuilds]{const bool passed=ok&&(!tools||window.chart_rebuilds()==rebuilds);const int index=args.indexOf("--screenshot");if(index>=0&&index+1<args.size())window.grab().save(args[index+1]);std::printf("%s: %s\n",moex?"MOEX online check":tools?"GUI tools test":"GUI smoke test",passed?"PASS":"FAIL");app.exit(passed?0:1);});});
        });
    }
    return app.exec();
}
