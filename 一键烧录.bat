@echo off
chcp 65001 >nul
title 屿光智能床头灯 - 一键烧录
setlocal

set "CLI="
for /d %%p in ("C:\ST\STM32CubeIDE*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_*") do (
  if exist "%%p\tools\bin\STM32_Programmer_CLI.exe" set "CLI=%%p\tools\bin\STM32_Programmer_CLI.exe"
)
if not defined CLI (
  for /d %%p in ("C:\ST\STM32CubeCLT*\STM32CubeProgrammer\bin") do (
    if exist "%%p\STM32_Programmer_CLI.exe" set "CLI=%%p\STM32_Programmer_CLI.exe"
  )
)
if not defined CLI (
  echo [错误] 未找到 STM32_Programmer_CLI.exe，请确认已安装 STM32CubeIDE
  pause
  exit /b 1
)

set "ELF=%~dp0Debug\SMART_LAMP.elf"
if not exist "%ELF%" set "ELF=%~dp0Release\SMART_LAMP.elf"
if not exist "%ELF%" (
  echo [错误] 未找到固件，请先在 CubeIDE 里按 Ctrl+B 编译一次
  pause
  exit /b 1
)

echo 正在烧录 %ELF% ...
"%CLI%" -c port=SWD -w "%ELF%" 0x08000000 -v
if errorlevel 1 (
  echo [错误] 烧录失败：请检查 ST-Link 与板子接线 SWDIO/SWCLK/GND/3.3V
  pause
  exit /b 1
)
"%CLI%" -c port=SWD -rst
echo.
echo 烧录成功，板子已复位运行！
timeout /t 5
exit /b 0
