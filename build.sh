#!/bin/sh
devtool_dir=${_DEVTOOL:-/var/devtools}
if [ ! -d "$devtool_dir" ]; then
  echo Devtools are not initialized at "$devtool_dir"
  devtool_dir=${_DEVTOOL:-~/devtools}
  if [ "$_DEVTOOL" = "" ] && [ ! -d "$devtool_dir" ]; then
    echo Creating Devtools at "$devtool_dir"
    mkdir $devtool_dir
  fi
  if [ ! -d "$devtool_dir" ]; then
    echo Devtools are not initialized at "$devtool_dir"
    exit 1
  fi
  echo Using Devtools at "$devtool_dir"
fi

echo Building jam and setting it up at "$devtool_dir"

# force non-empty $_DEVTOOL, jamfiles will fail otherwise
export _DEVTOOL=${_DEVTOOL:-$devtool_dir}
export OSX_CPU_TYPE=`uname -m`

cd jam_src
mkdir _output
c_opt='-pipe -x c -c -MD -Wno-error -Wno-trigraphs -Wno-multichar -Wno-parentheses -DNDEBUG=1 -D_GNU_SOURCE -pthread'
gcc $c_opt -o _output/builtins.o builtins.c
gcc $c_opt -o _output/command.o command.c
gcc $c_opt -o _output/compile.o compile.c
gcc $c_opt -o _output/expand.o expand.c
gcc $c_opt -o _output/execdmc.o execdmc.c
gcc $c_opt -o _output/fileunix.o fileunix.c
gcc $c_opt -o _output/glob.o glob.c
gcc $c_opt -o _output/hash.o hash.c
gcc $c_opt -o _output/headers.o headers.c
gcc $c_opt -o _output/jam.o jam.c
gcc $c_opt -o _output/jambase.o jambase.c
gcc $c_opt -o _output/jamgram.o jamgram.c
gcc $c_opt -o _output/lists.o lists.c
gcc $c_opt -o _output/make.o make.c
gcc $c_opt -o _output/make1.o make1.c
gcc $c_opt -o _output/newstr.o newstr.c
gcc $c_opt -o _output/option.o option.c
gcc $c_opt -o _output/parse.o parse.c
gcc $c_opt -o _output/pathunix.o pathunix.c
gcc $c_opt -o _output/regexp.o regexp.c
gcc $c_opt -o _output/rules.o rules.c
gcc $c_opt -o _output/scan.o scan.c
gcc $c_opt -o _output/search.o search.c
gcc $c_opt -o _output/variable.o variable.c
gcc $c_opt -o _output/timestamp.o timestamp.c
gcc $c_opt -o _output/outFilter.o outFilter.c
g++ -pipe _output/builtins.o _output/command.o _output/compile.o _output/expand.o _output/execdmc.o _output/fileunix.o _output/glob.o _output/hash.o _output/headers.o _output/jam.o _output/jambase.o _output/jamgram.o _output/lists.o _output/make.o _output/make1.o _output/newstr.o _output/option.o _output/parse.o _output/pathunix.o _output/regexp.o _output/rules.o _output/scan.o _output/search.o _output/variable.o _output/timestamp.o _output/outFilter.o -Wl,-lpthread -o ../jam_0
rm -rf _output
cd ..
rm ./jam
./jam_0 -sRoot=. -f jam_src/jamfile -a nocare

if [ -d "$devtool_dir" ]; then
  sudo cp jam $devtool_dir/jam
  sudo chmod 777 $devtool_dir/jam
fi
sudo cp jam /usr/local/bin/jam
sudo chmod 777 /usr/local/bin/jam

echo Jam set up at "$devtool_dir"
