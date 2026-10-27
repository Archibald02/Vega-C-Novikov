# Quant Price Live

Нативное приложение на **C++17 и Qt 6** для мониторинга российских акций:
таблица котировок, график, SMA, разброс доходностей и настройки подключения.
Основной сценарий не использует CSV. Данные поступают из интернета асинхронно.

**Документация Doxygen:** https://archibald02.github.io/Vega-C-Novikov/

Репозиторий: https://github.com/Archibald02/Vega-C-Novikov

## Источники

| Режим | Доступ | Характер данных |
|---|---|---|
| АЛОР | Боевой аккаунт и Refresh token | WebSocket-подписки на котировки MOEX/TQBR |
| MOEX ISS | Без ключа | Публичные данные с задержкой, HTTP раз в 5 секунд |
| ДЕМО | Без сети и аккаунта | Явно помеченные синтетические цены |

По FAQ АЛОР использование OpenAPI бесплатно. Для боевого потока нужен именно
торговый аккаунт, не только учётная запись разработчика. Условия обслуживания
счёта проверяются отдельно. Тестовый контур АЛОР не является источником реальных котировок
и в этой версии не подключается. Проект не размещает торговые поручения.

## Возможности

- До 20 тикеров TQBR: например, SBER, GAZP, LKOH, YDEX. Доступность зависит от источника.
- Последняя цена, bid/ask, изменение дня, время данных и время с последнего ответа.
- График выбранной строки и до 2 000 принятых точек на инструмент.
- SMA и выборочное стандартное отклонение доходностей по скользящему окну.
- O(1) работы на обновление расчётов; перерисовка до 4 раз в секунду.
- Асинхронный ввод-вывод, тайм-ауты HTTP, reconnect WebSocket, heartbeat и обновление авторизации.
- Сохранение тикеров и настроек. Токен и котировки на диск не сохраняются.

Нет предварительной загрузки истории: график заполняется с момента подключения.
Первая цена появляется после ответа источника; мгновенность и задержка сети не гарантируются.
Подписка АЛОР передаёт агрегированные обновления котировок, не каждую сделку.
Параметр «Поток» — интервал выдачи сообщений, а не полная задержка от биржи до экрана.

## Быстрый запуск Windows без установки компилятора

1. Откройте вкладку Actions этого репозитория.
2. Дождитесь успешного workflow в Actions.
3. В Artifacts скачайте **QuantPriceLive-Windows-x64**.
4. Полностью распакуйте ZIP, оставив DLL и подпапки рядом с EXE.
5. Запустите **quant_price_live.exe**.

На чистом компьютере может потребоваться официальный Microsoft Visual C++ Redistributable x64:
https://aka.ms/vs/17/release/vc_redist.x64.exe

Windows EXE создаётся в вашем CI. В архиве исходников нет заранее собранного Windows EXE.
Пошаговая настройка репозитория и приложения — в START_HERE.html / GUIDE_WINDOWS_RU.md.

## Локальная сборка Windows

Нужны Git, CMake 3.20+, MSVC 2022 и Qt 6.8.3 **MSVC 2022 64-bit** с модулем Qt WebSockets.
Qt можно поставить через aqtinstall, подробности в гайде. В Developer PowerShell for VS 2022:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build build --config Release --parallel 2
$env:PATH = "C:\Qt\6.8.3\msvc2022_64\bin;" + $env:PATH
ctest --test-dir build -C Release --output-on-failure
.\build\Release\quant_price_live.exe
.\build\Release\quant_benchmark.exe --benchmark_min_time=0.1s
```

CMake сам скачает doctest 2.4.12 и Google Benchmark 1.9.4. Нужен интернет при первой конфигурации.
Для папки запуска, независимой от Qt в PATH, используйте windeployqt (см. гайд).

## Linux

С установленными Qt 6.5+ Widgets/Network/WebSockets и dev-пакетами OpenGL:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/qt/gcc_64
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/quant_price_live
./build/quant_benchmark --benchmark_min_time=0.1s
```

Для одной численной библиотеки без Qt добавьте `-DBUILD_GUI=OFF`.

## Тесты и CI/CD

CI проверяет Release-сборку в Windows и Linux, doctest для формул и окон,
разбор сообщений API и автономный GUI smoke test. Сетевые запросы и токены для тестов не нужны.
Benchmark измеряет пакетные функции и отдельное обновление скользящего окна;
8 результатов видны в логах, JSON — в artifacts. Единицы benchmark указаны в таблице.
Doxygen генерирует HTML и публикует его на Pages после успешной сборки.
Windows CI упаковывает приложение вместе с Qt DLL и проверяет запуск пакета.

## Структура

- `app/` — окно, график, асинхронные источники, разбор JSON.
- `include/quant/` — численные API и RollingSeries.
- `src/` — пакетные численные функции для тестов и benchmark.
- `tests/` — doctest и входные сообщения без секретов.
- `benchmarks/` — Google Benchmark.
- `docs/` — формулы и решения реализации.
- `.github/workflows/ci.yml` — весь CI/CD и сборка Windows-пакета.
- `AGENTS.md`, `CLAUDE.md` — правила ИИ-агенту.

## Метрики

SMA во время прогрева использует имеющиеся цены; карточка показывает N / размер окна.
σ — стандартное отклонение доходностей между принятыми обновлениями, в процентах.
Это **не годовая волатильность**: интервалы между сообщениями не фиксированы.
Указанное время АЛОР относится к последней сделке; время ISS — к снимку ответа сервера.
Старые ответы отбрасываются, одинаковые цена+время не дублируются в статистике.
Bid/ask могут обновляться без новой точки последней цены.

Токен вводится только в локальное окно или через переменную окружения ALOR_REFRESH_TOKEN.
Его не надо добавлять в исходники, GitHub Secrets для CI или присылать в чат.
Функции отправки заявок и операций со счётом в программе отсутствуют.

## Официальные источники

- АЛОР, API бесплатно и условия доступа: https://www.alorbroker.ru/faq
- АЛОР, боевой доступ: https://alor.dev/docs/api/quick-start/prod-env
- АЛОР, подписки: https://alor.dev/docs/api/websocket/data-subscriptions/QuotesSubscribe
- АЛОР, авторизация: https://alor.dev/docs/api/access/authorization/access-token
- MOEX ISS: https://www.moex.com/a8531
- Qt WebSockets: https://doc.qt.io/qt-6/qwebsocket.html
- Qt deployment: https://doc.qt.io/qt-6/windows-deployment.html

Условия распространения зависимостей — в THIRD_PARTY.md и licenses/.
