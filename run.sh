#!/bin/bash
set -xue

QEMU=qemu-system-riscv32

# 1. 編譯器與環境設定
CC=clang
CFLAGS="-std=c11 -O2 -g3 -Wall -Wextra --target=riscv32-unknown-elf -fuse-ld=lld -fno-stack-protector -ffreestanding -nostdlib"

# 因為是用 WSL/Linux，直接呼叫 llvm-objcopy 就可以了！
OBJCOPY=llvm-objcopy

# ==========================================
# 2. 編譯應用程式 (Userland)
# ==========================================
# 把 shell.c 等程式碼編譯成執行檔 (shell.elf)
$CC $CFLAGS -Wl,-Tuser.ld -Wl,-Map=shell.map -o shell.elf shell.c user.c common.c

# 把 ELF 檔扒皮，變成純二進位檔 (shell.bin)
$OBJCOPY --set-section-flags .bss=alloc,contents -O binary shell.elf shell.bin

# 把純二進位檔包裝成 C 語言可以讀取的陣列物件檔 (shell.bin.o)
$OBJCOPY -Ibinary -Oelf32-littleriscv shell.bin shell.bin.o

# ==========================================
# 3. 編譯作業系統核心 (Kernel)
# ==========================================
# 把剛剛做好的應用程式陣列 (shell.bin.o) 一起塞進核心肚子裡編譯！
$CC $CFLAGS -Wl,-Tkernel.ld -Wl,-Map=kernel.map -o kernel.elf \
    kernel.c common.c shell.bin.o

# ==========================================
# 4. 啟動虛擬機
# ==========================================
$QEMU -machine virt -bios default -nographic -serial mon:stdio --no-reboot \
    -kernel kernel.elf