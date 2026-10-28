@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"
echo Quant Price Live 6.1.0 — учебное приложение, без реальных заявок
if not exist "quant_price_live.exe" (
  echo Распакуйте весь ZIP в отдельную папку. Файл quant_price_live.exe не найден.
  pause
  exit /b 1
)
if not exist "Qt6Core.dll" (
  echo Не хватает файлов Qt. Повторно распакуйте весь проверенный Windows-пакет.
  pause
  exit /b 1
)
if not exist "platforms\qwindows.dll" (
  echo Не найден platforms\qwindows.dll. Сохраните структуру папок при распаковке.
  pause
  exit /b 1
)
if not exist "%SystemRoot%\System32\vcruntime140.dll" if not exist "vcruntime140.dll" (
  echo Не найден Microsoft Visual C++ Runtime x64.
  echo Официальная установка: https://aka.ms/vs/17/release/vc_redist.x64.exe
  echo После установки запустите этот файл ещё раз. Никакие пароли приложению не нужны.
  pause
  exit /b 1
)
start "Quant Price Live" "%~dp0quant_price_live.exe"
exit /b 0
