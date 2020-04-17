JL_SHARE = $(shell julia -e 'print(joinpath(Sys.BINDIR, Base.DATAROOTDIR, "julia"))')
CFLAGS   += $(shell $(JL_SHARE)/julia-config.jl --cflags)
CXXFLAGS += $(shell $(JL_SHARE)/julia-config.jl --cflags)
LDFLAGS  += $(shell $(JL_SHARE)/julia-config.jl --ldflags)
LDLIBS   += $(shell $(JL_SHARE)/julia-config.jl --ldlibs)
JFLAGS=$(CXXFLAGS) $(LDFLAGS) $(LDLIBS)

CC=gcc
CXX=g++

.PHONY: all clean

all: test

clean:
	rm -f *.so test

%.so: %.cpp
	$(CXX) -shared -ggdb -O0 -o $@ $(JFLAGS) -fPIC $<

test: test.c julia-amp.so
	$(CC) -std=c99 -Wall -O0 -lpthread -ldl -ggdb $< -o $@
