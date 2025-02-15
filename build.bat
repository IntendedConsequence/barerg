@echo off
set CLANG="c:\program files\llvm\bin\clang-cl.exe"
set CommonCompilerFlags=/Od
set CommonCompilerFlags=%CommonCompilerFlags% /Zi
set CommonLinkerFlags=/incremental:no

%CLANG% %CommonCompilerFlags% barerg.cpp /link %CommonLinkerFlags% kernel32.lib
