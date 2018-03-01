all: src/main.cpp
	g++ -o main `llvm-config-4.0 --cxxflags --ldflags --system-libs --libs core` -std=c++14 src/main.cpp