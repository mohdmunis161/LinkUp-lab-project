CXX = g++
CXXFLAGS = -std=c++17 -Wall
LDFLAGS = -lssl -lcrypto -lpthread -lmysqlclient

SRCS = linkup_main.cpp linkup_channels.cpp linkup_chat.cpp linkup_file_transfer.cpp db_utils.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = linkup_main

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) linkup_main_test
