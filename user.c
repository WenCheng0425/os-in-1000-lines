#include "user.h"
#include "common.h"
extern char __stack_top[];

int syscall(int sysno, int arg0, int arg1, int arg2) {
    // 1. 準備公事包 (把變數塞進指定的硬體暫存器)
    register int a0 __asm__("a0") = arg0;
    register int a1 __asm__("a1") = arg1;
    register int a2 __asm__("a2") = arg2;
    register int a3 __asm__("a3") = sysno; // a3 放的是「服務代碼」

    // 2. 按下撥號鍵 (觸發例外)
    __asm__ __volatile__("ecall"
                         : "=r"(a0) // 3. 接收結果 (OS 會把回傳值放在 a0)
                         : "r"(a0), "r"(a1), "r"(a2), "r"(a3) // 告訴編譯器我們輸入了這些暫存器
                         : "memory"); // 告訴編譯器這段組合語言可能會改變記憶體，不要亂做最佳化

    return a0; // 4. 把 OS 給的結果回傳給應用程式
}

void putchar(char ch) {
    // 幫應用程式填好表單，代打電話！
    // 參數：(服務代碼, 第一個參數, 第二個參數, 第三個參數)
    syscall(SYS_PUTCHAR, ch, 0, 0);
}

int getchar(void) {
    return syscall(SYS_GETCHAR, 0, 0, 0);
}

__attribute__((noreturn)) void exit(void) {
    syscall(SYS_EXIT, 0, 0, 0); 
    for (;;);
}

__attribute__((section(".text.start")))
__attribute__((naked))
void start(void) {
    __asm__ __volatile__(
        "mv sp, %[stack_top] \n"
        "call main           \n"
        "call exit           \n"
        :: [stack_top] "r" (__stack_top)
    );
}