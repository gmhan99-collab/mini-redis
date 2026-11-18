#ifndef TEST_ALLOCATOR_H
#define TEST_ALLOCATOR_H

#include <stddef.h>

/*
 * 단위 테스트 전용 할당기. HASH_TABLE_TEST_ALLOCATOR로 빌드한 해시 테이블이 이 함수들을 호출한다.
 * 살아 있는 포인터를 주소별로 기록해, 추적하지 않은 주소의 해제와 이중 해제를 오류로 센다.
 * 한계: 해제된 주소를 malloc이 다시 돌려준 뒤의 이중 해제는 새 할당의 해제와 구분할 수 없다.
 */
void *test_allocator_malloc(size_t size);
void test_allocator_free(void *ptr);

/* 지금부터 nth번째(1부터) malloc 호출 하나만 NULL을 반환하게 한다. 0이면 주입을 끈다. */
void test_allocator_fail_nth(size_t nth);

/* 아직 해제되지 않은 추적 중인 할당 수. */
long test_allocator_live_count(void);

/* 지금까지 발생한 잘못된 해제(미추적 주소, 이중 해제)와 추적 용량 초과의 누적 횟수. */
long test_allocator_error_count(void);

#endif
