all: src/main.cpp
	g++ -o main -I/usr/include/llvm-4.0 -std=c++14 src/main.cpp