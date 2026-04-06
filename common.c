#include "common.h"

void putchar(char ch);

void printf(const char *fmt, ...) {
    va_list vargs;
    va_start(vargs, fmt);

    while (*fmt) {
        if (*fmt == '%') {
            fmt++; // Skip '%'
            switch (*fmt) { // Read the next character
                case '\0': // '%' at the end of the format string
                    putchar('%');
                    goto end;
                case '%': // Print '%'
                    putchar('%');
                    break;
                case 's': { // Print a NULL-terminated string.
                    const char *s = va_arg(vargs, const char *);
                    while (*s) {
                        putchar(*s);
                        s++;
                    }
                    break;
                }
                case 'd': { // Print an integer in decimal.
                    int value = va_arg(vargs, int);
                    unsigned magnitude = value; // https://github.com/nuta/operating-system-in-1000-lines/issues/64
                    if (value < 0) {
                        putchar('-');
                        magnitude = -magnitude;
                    }

                    unsigned divisor = 1;
                    while (magnitude / divisor > 9)
                        divisor *= 10;

                    while (divisor > 0) {
                        putchar('0' + magnitude / divisor);
                        magnitude %= divisor;
                        divisor /= 10;
                    }

                    break;
                }
                case 'x': { // Print an integer in hexadecimal.
                    unsigned value = va_arg(vargs, unsigned);
                    for (int i = 7; i >= 0; i--) {
                        unsigned nibble = (value >> (i * 4)) & 0xf;
                        putchar("0123456789abcdef"[nibble]);
                    }
                    break;
                }
                case 'b': { // Print an integer in binary.
                    unsigned value = va_arg(vargs, unsigned);
                    for (int i = 31; i >= 0; i--) {
                        unsigned bit = (value >> i) & 1; 
                        putchar('0' + bit);              // 如果 bit 是 1，'0'+1 就會印出 '1'
                    }
                    break;
                }
            }
        } else {
            putchar(*fmt);
        }

        fmt++;
    }

end:
    va_end(vargs);
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *) dst;
    const uint8_t *s = (const uint8_t *) src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *buf, char c, size_t n) {
    uint8_t *p = (uint8_t *) buf;
    while (n--)
        *p++ = c;
    return buf;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while (*src)           // 只要還沒讀到字串結尾的 '\0' (數值為 0)，就繼續跑
        *d++ = *src++;     // 邊複製、邊把指標往後推 (上一階段學過的極致寫法)
    *d = '\0';             // 煞車！手動在目的地補上結尾符號
    return dst;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {       // 只要兩個字串都還沒走到結尾 '\0'
        if (*s1 != *s2)        // 只要發現有一個字元不一樣
            break;             // 立刻中斷迴圈，準備判刑！
        s1++;                  // 一樣的話，就繼續檢查下一個字元
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2; // 判決出爐
}