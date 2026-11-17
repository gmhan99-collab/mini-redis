#ifndef MEASUREMENT_CLOCK_H
#define MEASUREMENT_CLOCK_H
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#define MEASUREMENT_CLOCK_NAME "QueryPerformanceCounter"
static inline uint64_t measurement_ticks(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)counter.QuadPart;
}
static inline double measurement_ms(uint64_t start, uint64_t end)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return (double)(end - start) * 1000.0 / (double)frequency.QuadPart;
}
#else
#include <time.h>
#define MEASUREMENT_CLOCK_NAME "clock_gettime(CLOCK_MONOTONIC)"
static inline uint64_t measurement_ticks(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
static inline double measurement_ms(uint64_t start, uint64_t end)
{
    return (double)(end - start) / 1000000.0;
}
#endif
#endif
