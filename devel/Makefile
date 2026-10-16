.PHONY: all format clean

CCACHE := $(shell which ccache)
CCFLAGS ?= -g -O2 -fsanitize=address -fsanitize=undefined -Wall
COVFLAGS ?= $(CCFLAGS) --coverage -fno-omit-frame-pointer -DDEBUG -DAPBUF_INIT=1

SRCS := $(wildcard test_*.c)
OBJS := $(SRCS:.c=.o)
TESTS := $(SRCS:.c=)

# The upnp.c translation unit is built once, without coverage
# instrumentation (the coverage report only covers darkhttpd.c).
UPNP_OBJ := upnp.o

all: $(TESTS) darkhttpd darkhttpd_no_debug darkhttpd_no_ipv6
	./run-tests

format:
	clang-format -i test_parse_field.c

$(OBJS): %.o: %.c ../darkhttpd.c ../upnp.h
	$(CCACHE) $(CC) $(CCFLAGS) -DDEBUG -c $<

$(UPNP_OBJ): ../upnp.c ../upnp.h
	$(CCACHE) $(CC) $(CCFLAGS) -c ../upnp.c -o $@

# Linking is uncacheable.
$(TESTS): %: %.o $(UPNP_OBJ)
	$(CC) $(CCFLAGS) -o $@ $< $(UPNP_OBJ)

# Debug build with coverage instrumentation.
darkhttpd: ../darkhttpd.c $(UPNP_OBJ) ../upnp.h
	$(CCACHE) $(CC) $(COVFLAGS) -c ../darkhttpd.c -o darkhttpd.o
	$(CC) $(COVFLAGS) darkhttpd.o $(UPNP_OBJ) -o darkhttpd

# Test building without -DDEBUG
darkhttpd_no_debug: ../darkhttpd.c $(UPNP_OBJ) ../upnp.h
	$(CCACHE) $(CC) $(CCFLAGS) -c ../darkhttpd.c -o darkhttpd_no_debug.o
	$(CC) $(CCFLAGS) -o $@ darkhttpd_no_debug.o $(UPNP_OBJ)

# Test building with -DNO_IPV6
darkhttpd_no_ipv6: ../darkhttpd.c $(UPNP_OBJ) ../upnp.h
	$(CCACHE) $(CC) $(CCFLAGS) -DNO_IPV6 -c ../darkhttpd.c -o darkhttpd_no_ipv6.o
	$(CC) $(CCFLAGS) -o $@ darkhttpd_no_ipv6.o $(UPNP_OBJ)

clean:
	rm -f 	*.gcov \
		test.out.log \
		test.out.stderr \
		test.out.stdout \
		test_logging.log \
		test_xff.log \
		$(OBJS) $(TESTS) \
		darkhttpd darkhttpd.o \
		darkhttpd_no_debug darkhttpd_no_debug.o \
		darkhttpd_no_ipv6 darkhttpd_no_ipv6.o \
		darkhttpd.gcda darkhttpd.gcno \
		upnp.o upnp.gcda upnp.gcno \
		fuzz_darkhttpd.o fuzz_upnp.o \
		fuzz_make_safe_uri \
		fuzz_socket \
		fuzz_parse_request
	rm -rf tmp.fuzz tmp.httpd.tests
