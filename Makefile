CC := gcc
CFLAGS := -std=c11 -Wall -Wextra

BUILD_DIR := build
SIPHASH_DIR := third_party/siphash
LIB_CPPFLAGS := -Iinclude -Isrc/hash_table -I$(SIPHASH_DIR)
LIB_SOURCES := src/hash_table/hash_table.c src/hash_table/key_hash.c src/hash_table/random_seed.c $(SIPHASH_DIR)/siphash.c
LIB_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(LIB_SOURCES))
# 헤더별 정밀 의존성 대신, 라이브러리 헤더가 바뀌면 라이브러리 오브젝트 전체를 다시 빌드한다.
LIB_HEADERS := src/hash_table/bucket_tree.h include/measurement_clock.h include/hash_table.h src/hash_table/key_hash.h src/hash_table/random_seed.h $(SIPHASH_DIR)/siphash.h
HASH_TEST_HEADERS := src/hash_table/hash_table_test_access.h
# 단위 테스트는 할당 실패 주입을 위해 hash_table.c를 테스트 할당기로 따로 컴파일한다.
TEST_OBJECTS := $(BUILD_DIR)/obj/test/hash_table.o $(filter-out $(BUILD_DIR)/obj/src/hash_table/hash_table.o,$(LIB_OBJECTS))
HASH_TABLE_LIB := $(BUILD_DIR)/libhash_table.a

ifeq ($(OS),Windows_NT)
# sh.exe가 PATH에 있어도 항상 cmd로 레시피를 실행해 경로/명령 해석을 고정한다.
SHELL := cmd.exe
EXE := .exe
OS_LDLIBS := -lbcrypt
SERVER_LDLIBS := -lws2_32
SERVER_PLATFORM_CPPFLAGS := -DFD_SETSIZE=256
make_dir = if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
run_program = $(subst /,\,$(1))
remove_build = if exist "$(subst /,\,$(BUILD_DIR))" rmdir /S /Q "$(subst /,\,$(BUILD_DIR))"
else
EXE :=
OS_LDLIBS :=
make_dir = mkdir -p $(1)
run_program = ./$(1)
remove_build = rm -rf -- "$(BUILD_DIR)"
endif

UNIT_TEST := $(BUILD_DIR)/test_hash_table$(EXE)
EXAMPLE := $(BUILD_DIR)/basic_usage$(EXE)

.PHONY: all clean lib example run-example unit-test test

all: lib example $(UNIT_TEST)

lib: $(HASH_TABLE_LIB)

$(HASH_TABLE_LIB): $(LIB_OBJECTS)
	ar rcs $@ $^

$(BUILD_DIR)/obj/%.o: %.c $(LIB_HEADERS)
	@$(call make_dir,$(dir $@))
	$(CC) $(CPPFLAGS) $(LIB_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/obj/test/hash_table.o: src/hash_table/hash_table.c $(LIB_HEADERS) $(HASH_TEST_HEADERS)
	@$(call make_dir,$(dir $@))
	$(CC) $(CPPFLAGS) $(LIB_CPPFLAGS) $(CFLAGS) -DHASH_TABLE_TEST_ALLOCATOR -c $< -o $@

example: $(EXAMPLE)

$(EXAMPLE): examples/basic_usage.c $(HASH_TABLE_LIB)
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $< $(HASH_TABLE_LIB) $(LDFLAGS) $(OS_LDLIBS) $(LDLIBS) -o $@

run-example: $(EXAMPLE)
	$(call run_program,$(EXAMPLE))

$(UNIT_TEST): tests/test_hash_table.c tests/test_allocator.c tests/test_allocator.h include/hash_table.h src/hash_table/key_hash.h $(HASH_TEST_HEADERS) $(SIPHASH_DIR)/vectors.h $(TEST_OBJECTS)
	$(CC) $(CPPFLAGS) $(LIB_CPPFLAGS) -Itests $(CFLAGS) tests/test_hash_table.c tests/test_allocator.c $(TEST_OBJECTS) $(LDFLAGS) $(OS_LDLIBS) $(LDLIBS) -o $@

unit-test: $(UNIT_TEST)
	$(call run_program,$(UNIT_TEST))

test: unit-test

clean:
	$(remove_build)

# Windows HTTP 서버와 계층별 회귀 테스트. 해시 테이블 단독 타깃은 유지한다.
LLHTTP_DIR := third_party/llhttp
SERVER_CPPFLAGS := $(LIB_CPPFLAGS) -I$(LLHTTP_DIR)/include $(SERVER_PLATFORM_CPPFLAGS)
LLHTTP_SOURCES := $(LLHTTP_DIR)/src/llhttp.c $(LLHTTP_DIR)/src/http.c $(LLHTTP_DIR)/src/api.c
LLHTTP_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(LLHTTP_SOURCES))
STORE_SOURCES := src/store/store.c
PERSISTENCE_SOURCES := src/store/persistence.c src/store/persistence_file.c
HTTP_SOURCES := src/server/http_api.c src/server/redis_http.c
SERVER_SOURCES := src/server/main.c src/server/tcp_server.c src/server/redis_server.c $(STORE_SOURCES) $(HTTP_SOURCES) $(PERSISTENCE_SOURCES)
SERVER_HEADERS := include/measurement_clock.h include/server/redis_http.h include/store.h include/persistence.h src/store/persistence_file.h include/server/http_api.h include/server/tcp_server.h $(LLHTTP_DIR)/include/llhttp.h
SERVER := $(BUILD_DIR)/mini-redis$(EXE)
STORE_TEST := $(BUILD_DIR)/test_store$(EXE)
HTTP_TEST := $(BUILD_DIR)/test_http_api$(EXE)
TCP_TEST := $(BUILD_DIR)/test_tcp_progress$(EXE)
TIMEOUT_SERVER := $(BUILD_DIR)/mini-redis-timeout-test$(EXE)
PERSISTENCE_SERVER := $(BUILD_DIR)/mini-redis-persistence-test$(EXE)
TIMEOUT_FLAGS := -DTCP_RECEIVE_TIMEOUT_MS=700u -DTCP_IDLE_TIMEOUT_MS=1200u -DTCP_SEND_TIMEOUT_MS=700u
PYTHON ?= python

.PHONY: server run-server store-test http-test tcp-test server-unit-test functional-test timeout-test check

server: $(SERVER)

ifeq ($(OS),Windows_NT)
all: server
endif

# 생성된 외부 코드에서만 사용하지 않는 콜백 인자 경고를 제외한다.
$(BUILD_DIR)/obj/$(LLHTTP_DIR)/%.o: $(LLHTTP_DIR)/%.c $(LLHTTP_DIR)/include/llhttp.h
	@$(call make_dir,$(dir $@))
	$(CC) $(CPPFLAGS) -I$(LLHTTP_DIR)/include $(CFLAGS) -Wno-unused-parameter -c $< -o $@

$(SERVER): $(SERVER_SOURCES) $(SERVER_HEADERS) $(HASH_TABLE_LIB) $(LLHTTP_OBJECTS)
	@$(call make_dir,$(BUILD_DIR))
	$(CC) $(CPPFLAGS) $(SERVER_CPPFLAGS) -I$(BUILD_DIR)/server-metadata $(CFLAGS) $(SERVER_SOURCES) $(LLHTTP_OBJECTS) $(HASH_TABLE_LIB) $(LDFLAGS) $(OS_LDLIBS) $(SERVER_LDLIBS) $(LDLIBS) -o $@

run-server: $(SERVER)
	$(call run_program,$(SERVER))

$(STORE_TEST): tests/test_store.c tests/test_allocator.c tests/test_allocator.h $(STORE_SOURCES) include/store.h $(LIB_HEADERS) $(TEST_OBJECTS)
	$(CC) $(CPPFLAGS) $(LIB_CPPFLAGS) -Itests $(CFLAGS) -DSTORE_TEST_ALLOCATOR tests/test_store.c tests/test_allocator.c $(STORE_SOURCES) $(TEST_OBJECTS) $(LDFLAGS) $(OS_LDLIBS) $(LDLIBS) -o $@

$(HTTP_TEST): tests/test_http_api.c $(HTTP_SOURCES) $(STORE_SOURCES) $(PERSISTENCE_SOURCES) $(SERVER_HEADERS) $(HASH_TABLE_LIB) $(LLHTTP_OBJECTS)
	$(CC) $(CPPFLAGS) $(SERVER_CPPFLAGS) -I$(BUILD_DIR)/server-metadata $(CFLAGS) tests/test_http_api.c $(HTTP_SOURCES) $(STORE_SOURCES) $(PERSISTENCE_SOURCES) $(LLHTTP_OBJECTS) $(HASH_TABLE_LIB) $(LDFLAGS) $(OS_LDLIBS) $(LDLIBS) -o $@

$(TCP_TEST): tests/server/test_tcp_progress.c src/server/tcp_server.c $(SERVER_HEADERS)
	@$(call make_dir,$(BUILD_DIR))
	$(CC) $(CPPFLAGS) $(SERVER_CPPFLAGS) -I$(BUILD_DIR)/server-metadata $(CFLAGS) tests/server/test_tcp_progress.c $(LDFLAGS) -lws2_32 $(LDLIBS) -o $@

store-test: $(STORE_TEST)
	$(call run_program,$(STORE_TEST))

http-test: $(HTTP_TEST)
	$(call run_program,$(HTTP_TEST))

ifeq ($(OS),Windows_NT)
tcp-test: $(TCP_TEST)
	$(call run_program,$(TCP_TEST))
server-unit-test: tcp-test
endif

server-unit-test: store-test http-test

functional-test: $(SERVER)
	$(PYTHON) tests/server/test_functional.py --server $(SERVER)

$(TIMEOUT_SERVER): $(SERVER_SOURCES) $(SERVER_HEADERS) $(HASH_TABLE_LIB) $(LLHTTP_OBJECTS)
	$(CC) $(CPPFLAGS) $(SERVER_CPPFLAGS) -I$(BUILD_DIR)/timeout-metadata $(CFLAGS) $(TIMEOUT_FLAGS) $(SERVER_SOURCES) $(LLHTTP_OBJECTS) $(HASH_TABLE_LIB) $(LDFLAGS) $(OS_LDLIBS) $(SERVER_LDLIBS) $(LDLIBS) -o $@

timeout-test: $(TIMEOUT_SERVER)
	$(PYTHON) tests/server/test_timeouts.py --server $(TIMEOUT_SERVER)

.PHONY: persistence-test
$(PERSISTENCE_SERVER): $(SERVER_SOURCES) $(SERVER_HEADERS) $(HASH_TABLE_LIB) $(LLHTTP_OBJECTS) include/bucket_options.h
	$(CC) $(CPPFLAGS) $(SERVER_CPPFLAGS) -I$(BUILD_DIR)/server-metadata $(CFLAGS) -DPERSISTENCE_TEST_HOOKS $(SERVER_SOURCES) $(LLHTTP_OBJECTS) $(HASH_TABLE_LIB) $(LDFLAGS) $(OS_LDLIBS) $(SERVER_LDLIBS) $(LDLIBS) -o $@

persistence-test: $(PERSISTENCE_SERVER)
	$(PYTHON) tests/server/test_persistence.py --server $(PERSISTENCE_SERVER)

check: unit-test server-unit-test functional-test timeout-test persistence-test

# Native measurement executable uses the same build directory as the server.
BENCHMARK := $(BUILD_DIR)/mini-redis-benchmark$(EXE)
BENCH_CFLAGS ?= $(CFLAGS) -O2
.PHONY: benchmark native-test
benchmark: $(BENCHMARK)
$(BENCHMARK): tools/native_benchmark.c tools/native_benchmark_common.h include/measurement_clock.h $(STORE_SOURCES) include/store.h $(LIB_HEADERS) $(LIB_SOURCES)
	@$(call make_dir,$(BUILD_DIR))
	$(CC) $(CPPFLAGS) $(LIB_CPPFLAGS) $(BENCH_CFLAGS) -I$(BUILD_DIR)/native-metadata tools/native_benchmark.c $(STORE_SOURCES) $(LIB_SOURCES) $(LDFLAGS) $(OS_LDLIBS) $(LDLIBS) -o $@
native-test: benchmark
	$(PYTHON) tests/test_native_benchmark.py --exe $(BENCHMARK)

# Flags travel via Make's environment, never through shell quote interpolation.
export MEASUREMENT_CC := $(CC)
export MEASUREMENT_SERVER_FLAGS := $(CPPFLAGS) $(SERVER_CPPFLAGS) $(CFLAGS)
export MEASUREMENT_TIMEOUT_FLAGS := $(CPPFLAGS) $(SERVER_CPPFLAGS) $(CFLAGS) $(TIMEOUT_FLAGS)
export MEASUREMENT_TIMEOUT_SOURCE := Make generated; includes timeout-test defines; auxiliary llhttp/hash compilation uses normal server flags
export MEASUREMENT_NATIVE_FLAGS := $(CPPFLAGS) $(LIB_CPPFLAGS) $(BENCH_CFLAGS)
export MEASUREMENT_SERVER_SOURCE := Make generated; flags are primary source compilation; llhttp uses CPPFLAGS CFLAGS -Wno-unused-parameter; hash library uses CPPFLAGS LIB_CPPFLAGS CFLAGS; generated-header include excluded
export MEASUREMENT_NATIVE_SOURCE := Make generated; all native sources use reported flags; generated-header include excluded
SERVER_BUILD_INFO := $(BUILD_DIR)/server-metadata/build_info.h
TIMEOUT_BUILD_INFO := $(BUILD_DIR)/timeout-metadata/build_info.h
NATIVE_BUILD_INFO := $(BUILD_DIR)/native-metadata/build_info.h
.PHONY: FORCE_BUILD_INFO
FORCE_BUILD_INFO:
$(SERVER_BUILD_INFO): FORCE_BUILD_INFO tools/generate_build_info.py
	$(PYTHON) tools/generate_build_info.py $@ server
$(NATIVE_BUILD_INFO): FORCE_BUILD_INFO tools/generate_build_info.py
	$(PYTHON) tools/generate_build_info.py $@ native
$(TIMEOUT_BUILD_INFO): FORCE_BUILD_INFO tools/generate_build_info.py
	$(PYTHON) tools/generate_build_info.py $@ timeout
$(TIMEOUT_SERVER): $(TIMEOUT_BUILD_INFO)
$(SERVER) $(HTTP_TEST) $(PERSISTENCE_SERVER) $(LIB_OBJECTS) $(LLHTTP_OBJECTS): $(SERVER_BUILD_INFO)
$(BENCHMARK): $(NATIVE_BUILD_INFO)

BUCKET_TEST := $(BUILD_DIR)/test_buckets$(EXE)
.PHONY: bucket-test
$(BUCKET_TEST): tests/test_buckets.c tests/test_allocator.c $(STORE_SOURCES) $(LIB_HEADERS) $(TEST_OBJECTS)
	$(CC) $(CPPFLAGS) $(LIB_CPPFLAGS) -Itests $(CFLAGS) -DSTORE_TEST_ALLOCATOR tests/test_buckets.c tests/test_allocator.c $(STORE_SOURCES) $(TEST_OBJECTS) $(LDFLAGS) $(OS_LDLIBS) $(LDLIBS) -o $@
bucket-test: $(BUCKET_TEST)
	$(call run_program,$(BUCKET_TEST))

BUCKET_BENCHMARK := $(BUILD_DIR)/mini-redis-bucket-benchmark$(EXE)
.PHONY: bucket-benchmark
bucket-benchmark: $(BUCKET_BENCHMARK)
$(BUCKET_BENCHMARK): tools/bucket_benchmark.c include/bucket_options.h $(LIB_HEADERS) $(LIB_SOURCES) $(NATIVE_BUILD_INFO)
	@$(call make_dir,$(BUILD_DIR))
	$(CC) $(CPPFLAGS) $(LIB_CPPFLAGS) $(BENCH_CFLAGS) -I$(BUILD_DIR)/native-metadata tools/bucket_benchmark.c $(LIB_SOURCES) $(LDFLAGS) $(OS_LDLIBS) $(LDLIBS) -o $@
$(SERVER) $(TIMEOUT_SERVER): include/bucket_options.h

.PHONY: bucket-native-test
bucket-native-test: $(BUCKET_BENCHMARK) $(SERVER)
	$(PYTHON) tests/test_bucket_benchmark.py --exe $(BUCKET_BENCHMARK) --server $(SERVER)
check: bucket-test bucket-native-test

BUCKET_HARDENING_TEST := $(BUILD_DIR)/test_bucket_hardening$(EXE)
.PHONY: bucket-hardening-test
$(BUCKET_HARDENING_TEST): tests/test_bucket_hardening.c tests/test_allocator.c tests/test_allocator.h $(LIB_HEADERS) src/hash_table/hash_table.c $(filter-out $(BUILD_DIR)/obj/src/hash_table/hash_table.o,$(LIB_OBJECTS))
	@$(call make_dir,$(BUILD_DIR))
	$(CC) $(CPPFLAGS) $(LIB_CPPFLAGS) -Itests $(CFLAGS) tests/test_bucket_hardening.c tests/test_allocator.c $(filter-out $(BUILD_DIR)/obj/src/hash_table/hash_table.o,$(LIB_OBJECTS)) $(LDFLAGS) $(OS_LDLIBS) $(LDLIBS) -o $@
bucket-hardening-test: $(BUCKET_HARDENING_TEST)
	$(call run_program,$(BUCKET_HARDENING_TEST))
bucket-test: bucket-hardening-test
