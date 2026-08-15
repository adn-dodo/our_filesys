CC := gcc
AR := ar
CPPFLAGS := -Iinclude -Isrc
CFLAGS := -std=c11 -Wall -Wextra -Werror -pedantic -O2 -pthread

BUILD_DIR := build
LIBRARY := $(BUILD_DIR)/libuserfs.a

MODULES := userfs ufs_sync userfs_storage journal namespace metadata file_io mmap_io
LIB_SOURCES := $(addprefix src/,$(addsuffix .c,$(MODULES)))
LIB_OBJECTS := $(addprefix $(BUILD_DIR)/,$(addsuffix .o,$(MODULES)))

TEST_NAMES := test_integration test_storage_v2 test_journal test_metadata \
	test_links test_mmap test_permissions_namespace test_crash_recovery
TEST_BINS := $(addprefix $(BUILD_DIR)/,$(TEST_NAMES))

.PHONY: all test concurrency sanitize shell clean tree

all: $(LIBRARY)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c include/userfs.h include/ufs_internal.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(LIBRARY): $(LIB_OBJECTS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/test_%: tests/test_%.c $(LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIBRARY) -o $@

test: $(TEST_BINS)
	$(BUILD_DIR)/test_integration
	$(BUILD_DIR)/test_storage_v2
	$(BUILD_DIR)/test_journal
	$(BUILD_DIR)/test_metadata
	$(BUILD_DIR)/test_links
	$(BUILD_DIR)/test_mmap
	$(BUILD_DIR)/test_permissions_namespace
	$(BUILD_DIR)/test_crash_recovery

concurrency: $(BUILD_DIR)/test_threads $(BUILD_DIR)/test_processes
	$(BUILD_DIR)/test_threads
	$(BUILD_DIR)/test_processes

sanitize: | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g \
		-pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(LIB_SOURCES) tests/test_integration.c \
		-o $(BUILD_DIR)/test_integration_san
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/test_integration_san
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g \
		-pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(LIB_SOURCES) tests/test_links.c -o $(BUILD_DIR)/test_links_san
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/test_links_san
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g \
		-pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(LIB_SOURCES) tests/test_mmap.c -o $(BUILD_DIR)/test_mmap_san
	ASAN_OPTIONS=detect_leaks=0 $(BUILD_DIR)/test_mmap_san

$(BUILD_DIR)/userfs_shell: apps/userfs_shell.c $(LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIBRARY) -o $@

shell: $(BUILD_DIR)/userfs_shell
	$(BUILD_DIR)/userfs_shell

tree:
	find . -maxdepth 2 -type f ! -path './build/*' | sort

clean:
	rm -rf $(BUILD_DIR)
	rm -f integration.img storage_test.img journal_test.img metadata_test.img \
		links_test.img mmap_test.img mmap_shell.img \
		userfs_live.img shell_links.img test_permissions.img \
		test_crash_recovery.img test_threads.img test_processes.img
