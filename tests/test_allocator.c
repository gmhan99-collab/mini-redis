#include "test_allocator.h"

#include <stdio.h>
#include <stdlib.h>

/* ?⑥쐞 ?뚯뒪?몄뿉???숈떆???댁븘 ?덈뒗 ?좊떦 ?섎낫???됰꼮?섎㈃ 異⑸텇?섎떎. */
#define TEST_ALLOCATOR_CAPACITY 4096

static void *live_pointers[TEST_ALLOCATOR_CAPACITY];
static long live_count = 0;
static long error_count = 0;
static size_t calls_until_failure = 0;

static void **find_slot(const void *ptr)
{
    for (size_t i = 0; i < TEST_ALLOCATOR_CAPACITY; i++) {
        if (live_pointers[i] == ptr) {
            return &live_pointers[i];
        }
    }
    return NULL;
}

static int is_injected_failure(void)
{
    if (calls_until_failure == 0) {
        return 0;
    }
    calls_until_failure--;
    return calls_until_failure == 0;
}

void *test_allocator_malloc(size_t size)
{
    if (is_injected_failure()) {
        return NULL;
    }
    void **slot = find_slot(NULL);
    if (slot == NULL) {
        fprintf(stderr, "  test_allocator: capacity %d exceeded\n", TEST_ALLOCATOR_CAPACITY);
        error_count++;
        return NULL;
    }
    void *ptr = malloc(size);
    if (ptr != NULL) {
        *slot = ptr;
        live_count++;
    }
    return ptr;
}

/* 異붿쟻?섏? ?딆? 二쇱냼???ㅼ젣 free瑜??몄텧?섏? ?딅뒗?? ?섎せ???ъ씤?곕? free?섎㈃ ?뚯뒪???먯껜媛 源⑥쭏 ???덈떎. */
void test_allocator_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    void **slot = find_slot(ptr);
    if (slot == NULL) {
        fprintf(stderr, "  test_allocator: free of untracked or already freed pointer %p\n", ptr);
        error_count++;
        return;
    }
    *slot = NULL;
    live_count--;
    free(ptr);
}

void test_allocator_fail_nth(size_t nth)
{
    calls_until_failure = nth;
}

long test_allocator_live_count(void)
{
    return live_count;
}

long test_allocator_error_count(void)
{
    return error_count;
}

