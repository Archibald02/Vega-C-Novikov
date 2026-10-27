# Third-party components

The project links dynamically to Qt Core, Gui, Widgets, Network and WebSockets.
Qt 6.8.3 is available under LGPLv3 / GPLv3 or a commercial license, depending
on the component and distribution. This project does not use Qt Charts.
Qt source and notices: https://download.qt.io/official_releases/qt/6.8/6.8.3/
License information: https://www.qt.io/licensing/open-source-lgpl-obligations
When redistributing a packaged application, preserve Qt's license texts,
allow replacement of the dynamically linked Qt libraries and satisfy the applicable license.
The source and CI package include LGPLv3 and GPLv3 license texts in licenses/.
The CI additionally copies the Qt SDK Licenses folder when present.

Test framework doctest 2.4.12: MIT, https://github.com/doctest/doctest/tree/v2.4.12
Google Benchmark 1.9.4: Apache-2.0, https://github.com/google/benchmark/tree/v1.9.4
Those dependencies are fetched by CMake, not copied into this source archive.

Market-data access and redistribution are governed separately by the provider's terms.
No market-data license or brokerage account is provided with this source code.
The app is a read-only monitor: it contains no order-placement integration.
