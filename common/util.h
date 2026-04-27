#ifndef UTIL_H
#define UTIL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG16(addr) (*(volatile uint16_t *)(uintptr_t)(addr))
#define REG8(addr)  (*(volatile uint8_t *)(uintptr_t)(addr))

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

/* UART (PL011) */
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
int uart_getc_nonblock(void);   /* -1 if nothing pending */
int uart_getc(void);

/* Minimal printf: %s %c %d %u %x %X %p, with optional 0-padding width. */
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);

void hexdump_short(const uint8_t *p, size_t n);

/* PSCI SYSTEM_RESET through the HVC conduit QEMU provides on "virt". */
void system_reset(void) __attribute__((noreturn));

uint32_t crc32(uint32_t crc, const void *data, size_t len);

/* Milliseconds from the ARM generic timer (virtual counter). */
uint32_t timer_ms(void);

static inline void version_str(uint32_t v, char *out)
{
    /* version packed as 0x00MMmmpp */
    uint32_t parts[3] = { (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff };
    int k = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t x = parts[i];
        if (x >= 100) out[k++] = (char)('0' + x / 100);
        if (x >= 10)  out[k++] = (char)('0' + (x / 10) % 10);
        out[k++] = (char)('0' + x % 10);
        if (i < 2) out[k++] = '.';
    }
    out[k] = 0;
}

#endif
