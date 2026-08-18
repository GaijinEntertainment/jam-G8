@echo off

if not defined VSINSTALLDIR (
  echo.
  echo ERROR: Visual Studio environment not detected. Please run 'vcvarsall.bat' and try again.
  echo        "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" ^&^& build.vc.cmd
  exit /b 1
)

if not exist jam_0.exe (
pushd jam_src
echo "Building jam_0.exe first (since it is absent)..."
cl -nologo -W0 -O2 -DNT -DWIN32 builtins.c command.c compile.c expand.c execdmc.c filent.c glob.c hash.c headers.c jam.c jambase.c jamgram.c lists.c make.c make1.c newstr.c option.c parse.c pathunix.c  regexp.c rules.c scan.c search.c variable.c timestamp.c outFilter.c changedPaths.c fastre.c depcache.c strrules.c statecache.c prof.c user32.lib /Fe../jam_0.exe
del *.obj
popd
)
echo Building jam
jam_0 -sRoot=. -sOutDir=. -f jam_src/jamfile -a
