CC := gcc
AR := ar
CFLAGS := -std=c11 -Wall -Wextra -Werror -pedantic -O2

LIB_SOURCES := userfs.c userfs_storage.c namespace.c file_io.c
LIB_OBJECTS := $(LIB_SOURCES:.c=.o)
LIBRARY := libuserfs.a
TEST := test_integration

.PHONY: all test demo shell sanitize trace clean

all: $(LIBRARY)

$(LIBRARY): $(LIB_OBJECTS)
	$(AR) rcs $@ $^

$(LIB_OBJECTS): userfs.h ufs_internal.h

$(TEST): test_integration.c $(LIBRARY)
	$(CC) $(CFLAGS) test_integration.c -L. -luserfs -o $@

test: $(TEST)
	./$(TEST)

demo:
	$(CC) $(CFLAGS) $(LIB_SOURCES) demo_userfs.c -o demo_userfs
	./demo_userfs

shell:
	$(CC) $(CFLAGS) $(LIB_SOURCES) userfs_shell.c -o userfs_shell
	./userfs_shell

sanitize:
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		$(LIB_SOURCES) test_integration.c -o test_integration_san
	ASAN_OPTIONS=detect_leaks=0 ./test_integration_san

trace:
	$(CC) $(CFLAGS) -DUFS_TRACE $(LIB_SOURCES) test_integration.c \
		-o test_integration_trace
	./test_integration_trace

clean:
	rm -f $(LIB_OBJECTS) test_integration.o $(LIBRARY) $(TEST) test_integration_san \
		test_integration_trace demo_userfs userfs_shell integration.img \
		userfs_demo.img userfs_live.img
