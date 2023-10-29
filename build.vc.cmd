@echo off

if not defined VSINSTALLDIR (
  echo.
  echo ERROR: Visual Studio environment not detected. Please run 'vcvarsall.bat' and try again.
  exit /b 1
)

pushd jam_src
cl -nologo -W0 -O2 -DNT -DWIN32 builtins.c command.c compile.c expand.c execdmc.c filent.c glob.c hash.c headers.c jam.c jambase.c jamgram.c lists.c make.c make1.c newstr.c option.c parse.c pathunix.c  regexp.c rules.c scan.c search.c variable.c timestamp.c outFilter.c user32.lib /Fe../jam_0.exe
del *.obj
popd
jam_0 -sRoot=. -f jam_src/Jamfile clean_all
jam_0 -sRoot=. -f jam_src/Jamfile
