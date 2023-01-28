@echo off
rem build.vc.cmd %GDEVTOOL%\vc2008 %GDEVTOOL%\win.sdk.71

if "%2" == "" (
  echo usage: build.vc.cmd VC_DIR WIN_SDK_DIR
  exit /B 1
)
if "%1" == "" (
  echo usage: build.vc.cmd VC_DIR WIN_SDK_DIR
  exit /B 1
)
set PATH=%1\bin;%2\bin;%PATH%
set INCLUDE=%1\include;%2\include
set LIB=%1\lib;%2\lib

pushd jam_src
cl -nologo -W0 -O2 -DNT -DWIN32 builtins.c command.c compile.c expand.c execdmc.c filent.c glob.c hash.c headers.c jam.c jambase.c jamgram.c lists.c make.c make1.c newstr.c option.c parse.c pathunix.c  regexp.c rules.c scan.c search.c variable.c timestamp.c outFilter.c user32.lib /Fe../jam_0.exe
del *.obj
popd
jam_0 -sRoot=. -f jam_src/Jamfile clean_all
jam_0 -sRoot=. -f jam_src/Jamfile
