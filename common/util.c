#include "util.h"
#include "board.h"

/* ---- libc bits (the compiler may emit calls to memcpy/memset itself) ---- */

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if ((((uintptr_t)d | (uintptr_t)s | n) & 3) == 0) {
        uint32_t *dw = dst;
        const uint32_t *sw = src;
        for (size_t i = 0; i < n / 4; i++)
            dw[i] = sw[i];
        return dst;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] - y[i];
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (uint8_t)*a - (uint8_t)*b;
}

/* ---- PL011 ---- */

#define UART_DR   (UART0_BASE + 0x00)
#define UART_FR   (UART0_BASE + 0x18)
#define UART_LCRH (UART0_BASE + 0x2c)
#define UART_CR   (UART0_BASE + 0x30)
#define UART_IMSC (UART0_BASE + 0x38)
#define FR_RXFE   (1u << 4)
#define FR_TXFF   (1u << 5)

void uart_init(void)
{
    REG32(UART_CR) = 0;
    REG32(UART_IMSC) = 0;
    REG32(UART_LCRH) = (3u << 5) | (1u << 4);      /* 8N1, FIFOs on */
    REG32(UART_CR) = (1u << 0) | (1u << 8) | (1u << 9); /* UARTEN | TXE | RXE */
}

void uart_putc(char c)
{
    if (c == '\n')
        uart_putc('\r');
    while (REG32(UART_FR) & FR_TXFF)
        ;
    REG32(UART_DR) = (uint8_t)c;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

int uart_getc_nonblock(void)
{
    if (REG32(UART_FR) & FR_RXFE)
        return -1;
    return (int)(REG32(UART_DR) & 0xff);
}

int uart_getc(void)
{
    int c;
    while ((c = uart_getc_nonblock()) < 0)
        ;
    return c;
}

/* ---- printf ---- */

static void put_num(uint32_t v, unsigned base, int width, char pad, int upper)
{
    char buf[12];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do {
        buf[n++] = digits[v % base];
        v /= base;
    } while (v);
    while (n < width--)
        uart_putc(pad);
    while (n)
        uart_putc(buf[--n]);
}

void kvprintf(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            uart_putc(*fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0, left = 0;
        if (*fmt == '-') {
            left = 1;
            fmt++;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len = (int)strlen(s);
            if (!left)
                while (len < width--)
                    uart_putc(' ');
            uart_puts(s);
            if (left)
                while (len < width--)
                    uart_putc(' ');
            break;
        }
        case 'c': uart_putc((char)va_arg(ap, int)); break;
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) {
                uart_putc('-');
                v = -v;
            }
            put_num((uint32_t)v, 10, width, pad, 0);
            break;
        }
        case 'u': put_num(va_arg(ap, uint32_t), 10, width, pad, 0); break;
        case 'x': put_num(va_arg(ap, uint32_t), 16, width, pad, 0); break;
        case 'X': put_num(va_arg(ap, uint32_t), 16, width, pad, 1); break;
        case 'p': uart_puts("0x"); put_num(va_arg(ap, uint32_t), 16, 8, '0', 0); break;
        case '%': uart_putc('%'); break;
        default:  uart_putc('%'); uart_putc(*fmt); break;
        }
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

void hexdump_short(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        kprintf("%02x", p[i]);
}

/* ---- reset ---- */

void system_reset(void)
{
    register uint32_t r0 __asm__("r0") = 0x84000009u;   /* PSCI_SYSTEM_RESET */
    __asm__ volatile(".arch_extension virt\n\thvc #0" : : "r"(r0) : "memory");
    for (;;)
        __asm__ volatile("wfi");
}

/* ---- generic timer ---- */

uint32_t timer_ms(void)
{
    uint32_t lo, hi, freq;
    __asm__ volatile("isb; mrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));   /* CNTVCT */
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(freq));             /* CNTFRQ */
    uint64_t cnt = ((uint64_t)hi << 32) | lo;
    return (uint32_t)(cnt / (freq / 1000u));
}

/* ---- CRC-32 (IEEE, reflected) ---- */

uint32_t crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
