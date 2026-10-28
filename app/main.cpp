#include "main_window.hpp"
#include "instrument_catalog.hpp"
#include <QApplication>
#include <QTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <memory>
#include <cstdio>
int main(int argc,char**argv){
    QApplication app(argc,argv);QCoreApplication::setOrganizationName("QuantPriceLab");QCoreApplication::setApplicationName("QuantPriceLive");QCoreApplication::setApplicationVersion("6.1.0");
    const auto args=app.arguments();const bool online_lab=args.contains("--check-lab");const bool lab=args.contains("--lab-test");const bool soak=args.contains("--soak-test");const bool visuals=args.contains("--visuals-test"),privacy=args.contains("--privacy-test");
    const bool smoke=args.contains("--smoke-test"),moex=args.contains("--check-moex"),tools=args.contains("--tools-test"),stress=args.contains("--stress-test"),profile=args.contains("--profile");
    if(args.contains("--check-catalog")){
        live::InstrumentCatalog catalog;
        QObject::connect(&catalog,&live::InstrumentCatalog::updated,&app,[&]{std::printf("Catalog online check: %lld instruments\n",static_cast<long long>(catalog.instruments().size()));app.exit(catalog.instruments().isEmpty()?1:0);});
        QObject::connect(&catalog,&live::InstrumentCatalog::failed,&app,[&](const QString&){std::printf("Catalog online check: FAIL\n");app.exit(1);});
        catalog.load(true);QTimer::singleShot(20000,&app,[&]{app.exit(1);});return app.exec();
    }
    if(args.contains("--check-long-history")) {
        live::MarketHistory history;
        QObject::connect(&history,&live::MarketHistory::loaded,&app,[&](const QString&,int,const QVector<live::Candle>& bars,bool){std::printf("Paginated public history: %lld bars; %s\n",static_cast<long long>(bars.size()),bars.size()>500?"PASS":"FAIL");app.exit(bars.size()>500?0:1);});
        QObject::connect(&history,&live::MarketHistory::failed,&app,[&](const QString& e){std::printf("Paginated public history: FAIL (%s)\n",e.toUtf8().constData());app.exit(1);});
        history.load_range("SBER",QDate(2020,1,1),QDate::currentDate(),24);QTimer::singleShot(60000,&app,[&]{app.exit(1);});return app.exec();
    }
    std::unique_ptr<QTemporaryDir> private_settings;
    if(privacy){qputenv("ALOR_REFRESH_TOKEN","ENVIRONMENT_CANARY_MUST_NOT_BE_IMPORTED");private_settings=std::make_unique<QTemporaryDir>();if(!private_settings->isValid())return 1;QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,private_settings->path());}
    if(args.contains("--check-history")){live::MarketHistory history;QObject::connect(&history,&live::MarketHistory::loaded,&app,[&](const QString&,int,const QVector<live::Candle>& bars,bool){std::printf("Public candles online check: %lld bars\n",static_cast<long long>(bars.size()));app.exit(bars.isEmpty()?1:0);});QObject::connect(&history,&live::MarketHistory::failed,&app,[&](const QString&){std::printf("Public candles online check: FAIL\n");app.exit(1);});history.load("SBER",0);QTimer::singleShot(20000,&app,[&]{app.exit(1);});return app.exec();}
    live::MainWindow window(smoke||moex||tools||stress||profile||visuals||soak||lab||online_lab);window.show();
    if(online_lab){window.start_lab_online_test();auto* check=new QTimer(&app);QObject::connect(check,&QTimer::timeout,&app,[&]{if(window.lab_online_ready()){std::printf("Public laboratory integration: PASS\n");app.exit(0);}});check->start(250);QTimer::singleShot(40000,&app,[&]{std::printf("Public laboratory integration: FAIL\n");app.exit(1);});}
    if(smoke||moex||tools||stress||profile||visuals||privacy||soak||lab){
        if(soak)window.start_soak_test();else if(moex)window.start_moex_test();else if(!privacy)window.start_demo_test(stress||profile?20:4);
        if(stress)QTimer::singleShot(100,&app,[&]{const bool ok=window.stress_test();std::printf("Bounded history stress (500000 observations): %s\n",ok?"PASS":"FAIL");app.exit(ok?0:1);});
        else if(profile||soak){auto* sample_timer=new QTimer(&app);QObject::connect(sample_timer,&QTimer::timeout,&app,[&]{std::printf("%s\n",window.performance_status().toUtf8().constData());std::fflush(stdout);});sample_timer->start(soak?5000:1000);QTimer::singleShot(soak?150000:15000,&app,[&]{const bool ok=!soak||window.soak_ready();std::printf("%s finished; chart rebuilds: %llu; %s\n",soak?"150-second soak":"Profile",static_cast<unsigned long long>(window.chart_rebuilds()),ok?"PASS":"FAIL");app.exit(ok?0:1);});}
        else QTimer::singleShot(smoke||tools||visuals||privacy||lab?1800:14000,&app,[&]{const bool ok=moex?window.has_any_data():tools?window.exercise_tools_test():visuals?window.exercise_visuals_test():privacy?window.exercise_privacy_test():lab?window.exercise_lab_test():window.has_demo_data();
            QTimer::singleShot(tools?650:100,&app,[&,ok]{const auto rebuilds=window.chart_rebuilds();if(lab){const int view=args.indexOf("--lab-view");if(view>=0&&view+1<args.size())window.select_lab_test_view(args[view+1].toInt());}if(visuals){const int view=args.indexOf("--view");if(view>=0&&view+1<args.size())window.select_visual_test(args[view+1].toInt());}QTimer::singleShot(tools?650:0,&app,[&,ok,rebuilds]{const bool passed=ok&&(!tools||window.chart_rebuilds()==rebuilds);const int index=args.indexOf("--screenshot");if(index>=0&&index+1<args.size())window.grab().save(args[index+1]);std::printf("%s: %s\n",moex?"MOEX online check":tools?"GUI tools test":visuals?"GUI visuals test":privacy?"GUI privacy/lifecycle test":lab?"GUI laboratory test":"GUI smoke test",passed?"PASS":"FAIL");app.exit(passed?0:1);});});
        });
    }
    return app.exec();
}
