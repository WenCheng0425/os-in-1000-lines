#include "kernel.h"
#include "common.h"

// 來自 linker script (kernel.ld) 的符號，定義了記憶體的邊界
extern char __bss[], __bss_end[], __stack_top[];
extern char __free_ram[], __free_ram_end[];
extern char __kernel_base[]; 

// 來自 shell.bin.o 的符號，這是我們「焊」進核心的應用程式資料
extern char _binary_shell_bin_start[], _binary_shell_bin_size[];

// ==========================================
// 全域變數區 (Global States)
// ==========================================
struct process procs[PROCS_MAX]; // 行程控制表 (所有的行程都存在這)
struct process *current_proc;    // 指向目前正在執行的行程
struct process *idle_proc;       // 指向閒置行程 (當沒事做時跑這個)

// ✨ 新增：Virtio 磁碟相關的全域變數
struct virtio_virtq *blk_request_vq;
struct virtio_blk_req *blk_req;
paddr_t blk_req_paddr;
uint64_t blk_capacity;

// ==========================================
// 1. 底層硬體操作 (SBI & Memory)
// ==========================================

// 呼叫 OpenSBI (透過 ecall)，這是 OS 與底層韌體溝通的管道
struct sbiret sbi_call(long arg0, long arg1, long arg2, long arg3, long arg4,
                       long arg5, long fid, long eid) {
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a3 __asm__("a3") = arg3;
    register long a4 __asm__("a4") = arg4;
    register long a5 __asm__("a5") = arg5;
    register long a6 __asm__("a6") = fid;
    register long a7 __asm__("a7") = eid;

    __asm__ __volatile__("ecall"
                         : "=r"(a0), "=r"(a1)
                         : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5),
                           "r"(a6), "r"(a7)
                         : "memory");
    return (struct sbiret){.error = a0, .value = a1};
}

void putchar(char ch) {
    sbi_call(ch, 0, 0, 0, 0, 0, 0, 1 /* Console Putchar */);
}

long getchar(void) {
    struct sbiret ret = sbi_call(0, 0, 0, 0, 0, 0, 0, 2 /* Console Getchar */);
    return ret.error;
}

// 記憶體分配器：一次發放一頁 (4096 bytes)
paddr_t alloc_pages(uint32_t n) {
    static paddr_t next_paddr = (paddr_t) __free_ram;
    paddr_t paddr = next_paddr;
    next_paddr += n * PAGE_SIZE;

    if (next_paddr > (paddr_t) __free_ram_end)
        PANIC("out of memory");

    memset((void *) paddr, 0, n * PAGE_SIZE);
    return paddr;
}

// 建立並寫入分頁表 (Page Table) 的對照紀錄
// table1: 第一層分頁表, vaddr: 虛擬位址, paddr: 物理位址, flags: 權限
void map_page(uint32_t *table1, uint32_t vaddr, paddr_t paddr, uint32_t flags) {
    if (!is_aligned(vaddr, PAGE_SIZE)) PANIC("unaligned vaddr %x", vaddr);
    if (!is_aligned(paddr, PAGE_SIZE)) PANIC("unaligned paddr %x", paddr);

    // [第一層] 取得 VPN1
    uint32_t vpn1 = (vaddr >> 22) & 0x3ff;
    if ((table1[vpn1] & PAGE_V) == 0) {
        // 如果第二層表還不存在，就生出一頁來當第二層表
        uint32_t pt_paddr = alloc_pages(1);
        table1[vpn1] = ((pt_paddr / PAGE_SIZE) << 10) | PAGE_V;
    }

    // [第二層] 取得 VPN0 並填入物理位址
    uint32_t vpn0 = (vaddr >> 12) & 0x3ff;
    uint32_t *table0 = (uint32_t *) ((table1[vpn1] >> 10) * PAGE_SIZE);
    table0[vpn0] = ((paddr / PAGE_SIZE) << 10) | flags | PAGE_V;
}

// ==========================================
// 2. 行程管理與切換 (Context Switch)
// ==========================================

// 保存目前行程的存摺 (暫存器)，並換成下一個行程的存摺
__attribute__((naked)) void switch_context(uint32_t *prev_sp, uint32_t *next_sp) {
    __asm__ __volatile__(
        "addi sp, sp, -13 * 4\n" // 在堆疊上挖 13 格空間
        "sw ra,  0  * 4(sp)\n"   // 存下所有「被呼叫者保存」暫存器
        "sw s0,  1  * 4(sp)\n"
        "sw s1,  2  * 4(sp)\n"
        "sw s2,  3  * 4(sp)\n"
        "sw s3,  4  * 4(sp)\n"
        "sw s4,  5  * 4(sp)\n"
        "sw s5,  6  * 4(sp)\n"
        "sw s6,  7  * 4(sp)\n"
        "sw s7,  8  * 4(sp)\n"
        "sw s8,  9  * 4(sp)\n"
        "sw s9,  10 * 4(sp)\n"
        "sw s10, 11 * 4(sp)\n"
        "sw s11, 12 * 4(sp)\n"

        "sw sp, (a0)\n"         // *prev_sp = 目前的 sp
        "lw sp, (a1)\n"         // sp = *next_sp (換成別人的堆疊了！)

        "lw ra,  0  * 4(sp)\n"   // 讀回別人的暫存器
        "lw s0,  1  * 4(sp)\n"
        "lw s1,  2  * 4(sp)\n"
        "lw s2,  3  * 4(sp)\n"
        "lw s3,  4  * 4(sp)\n"
        "lw s4,  5  * 4(sp)\n"
        "lw s5,  6  * 4(sp)\n"
        "lw s6,  7  * 4(sp)\n"
        "lw s7,  8  * 4(sp)\n"
        "lw s8,  9  * 4(sp)\n"
        "lw s9,  10 * 4(sp)\n"
        "lw s10, 11 * 4(sp)\n"
        "lw s11, 12 * 4(sp)\n"
        "addi sp, sp, 13 * 4\n" 
        "ret\n"
    );
}

// 降級跳轉：從核心模式 (S-Mode) 跳進使用者模式 (U-Mode)
__attribute__((naked)) void user_entry(void) {
    __asm__ __volatile__(
        "csrw sepc, %[sepc]\n"       // 設定出院後的第一站 (USER_BASE)
        "csrw sstatus, %[sstatus]\n" // 設定權限：啟動中斷，回到 U-Mode
        "sret\n"                     // 執行出院！
        : : [sepc] "r" (USER_BASE), [sstatus] "r" (SSTATUS_SPIE)
    );
}

// 核心工廠：建立一個新的行程
struct process *create_process(const void *image, size_t image_size) {
    struct process *proc = NULL;
    int i;
    for (i = 0; i < PROCS_MAX; i++) {
        if (procs[i].state == PROC_UNUSED) {
            proc = &procs[i];
            break;
        }
    }
    if (!proc) PANIC("no free process slots");

    // 初始化核心堆疊與暫存器
    uint32_t *sp = (uint32_t *) &proc->stack[sizeof(proc->stack)];
    for (int j = 0; j < 12; j++) *--sp = 0; // s0-s11 = 0
    *--sp = (uint32_t) user_entry;         // 第一次切換過來時會跳到這裡

    // 為行程配一副全新的「VR 眼鏡」(分頁表)
    uint32_t *page_table = (uint32_t *) alloc_pages(1);

    // [映射區 A] 核心本身與 RAM：讓核心在處理這個行程時也能存取自己
    for (paddr_t paddr = (paddr_t) __kernel_base;
         paddr < (paddr_t) __free_ram_end; paddr += PAGE_SIZE) {
        map_page(page_table, paddr, paddr, PAGE_R | PAGE_W | PAGE_X); 
    }

    // [映射區 B] 磁碟裝置：讓核心可以讀寫 Virtio-blk 暫存器 (✨ 新增)
    map_page(page_table, VIRTIO_BLK_PADDR, VIRTIO_BLK_PADDR, PAGE_R | PAGE_W);

    // [映射區 C] 使用者程式：把應用程式搬進去，並標記為「使用者可存取 (PAGE_U)」
    for (uint32_t off = 0; off < image_size; off += PAGE_SIZE) {
        paddr_t page = alloc_pages(1);
        size_t remaining = image_size - off;
        size_t copy_size = PAGE_SIZE <= remaining ? PAGE_SIZE : remaining;
        memcpy((void *) page, (const uint8_t *)image + off, copy_size);
        map_page(page_table, USER_BASE + off, page,
                 PAGE_U | PAGE_R | PAGE_W | PAGE_X);
    }

    proc->pid = i + 1;
    proc->state = PROC_RUNNABLE;
    proc->sp = (uint32_t) sp;
    proc->page_table = page_table;
    return proc;
}

// 禮讓機制：交出 CPU 控制權給下一個行程
void yield(void) {
    struct process *next = idle_proc;
    for (int i = 0; i < PROCS_MAX; i++) {
        struct process *proc = &procs[(current_proc->pid + i) % PROCS_MAX];
        if (proc->state == PROC_RUNNABLE && proc->pid > 0) {
            next = proc;
            break;
        }
    }
    if (next == current_proc) return;

    struct process *prev = current_proc;
    current_proc = next;

    // 更換 VR 眼鏡 (satp) 並準備好病床 (sscratch)
    __asm__ __volatile__(
        "sfence.vma\n"               // 清空管線
        "csrw satp, %[satp]\n"       // 切換分頁表
        "sfence.vma\n"               // 清空舊地圖快取 (TLB)
        "csrw sscratch, %[sscratch]\n" // 設定核心堆疊指標，Trap 時用
        :
        : [satp] "r" (SATP_SV32 | ((uint32_t) next->page_table / PAGE_SIZE)),
          [sscratch] "r" ((uint32_t) &next->stack[sizeof(next->stack)])
    );
    
    switch_context(&prev->sp, &next->sp);
}

// ==========================================
// 3. Disk I/O 區 (Virtio-blk)
// ==========================================

uint32_t virtio_reg_read32(unsigned offset) {
    return *((volatile uint32_t *) (VIRTIO_BLK_PADDR + offset));
}

uint64_t virtio_reg_read64(unsigned offset) {
    return *((volatile uint64_t *) (VIRTIO_BLK_PADDR + offset));
}

void virtio_reg_write32(unsigned offset, uint32_t value) {
    *((volatile uint32_t *) (VIRTIO_BLK_PADDR + offset)) = value;
}

void virtio_reg_fetch_and_or32(unsigned offset, uint32_t value) {
    virtio_reg_write32(offset, virtio_reg_read32(offset) | value);
}

// 初始化 Virtqueue (Virtio 的通訊佇列)
struct virtio_virtq *virtq_init(unsigned index) {
    paddr_t virtq_paddr = alloc_pages(align_up(sizeof(struct virtio_virtq), PAGE_SIZE) / PAGE_SIZE);
    struct virtio_virtq *vq = (struct virtio_virtq *) virtq_paddr;
    vq->queue_index = index;
    vq->used_index = (volatile uint16_t *) &vq->used.index;

    // 1. 選擇佇列：寫入索引（第一個佇列為 0）
    virtio_reg_write32(VIRTIO_REG_QUEUE_SEL, index);
    // 2. 指定佇列大小：寫入要使用的描述項數量
    virtio_reg_write32(VIRTIO_REG_QUEUE_NUM, VIRTQ_ENTRY_NUM);
    // 3. 寫入佇列的頁框編號 (PFN)
    // 裝置會利用這個位址來進行 DMA 存取
    virtio_reg_write32(VIRTIO_REG_QUEUE_PFN, virtq_paddr / PAGE_SIZE);

    return vq;
}

void virtio_blk_init(void) {
    if (virtio_reg_read32(VIRTIO_REG_MAGIC) != 0x74726976)
        PANIC("virtio: invalid magic value");
    if (virtio_reg_read32(VIRTIO_REG_VERSION) != 1)
        PANIC("virtio: invalid version");
    if (virtio_reg_read32(VIRTIO_REG_DEVICE_ID) != VIRTIO_DEVICE_BLK)
        PANIC("virtio: invalid device id");

    // 1. 重設裝置
    virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, 0);
    // 2. 設定 ACKNOWLEDGE 狀態位元：已發現裝置
    virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACK);
    // 3. 設定 DRIVER 狀態位元：知道如何使用此裝置
    virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_DRIVER);
    // 設定頁面大小：使用 4KB 頁面
    virtio_reg_write32(VIRTIO_REG_PAGE_SIZE, PAGE_SIZE);
    
    // 初始化磁碟讀寫請求用的佇列 (需實作 virtq_init)
    blk_request_vq = virtq_init(0);
    
    // 6. 設定 DRIVER_OK 狀態位元：現在可以使用裝置了
    virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_DRIVER_OK);

    // 取得磁碟容量
    blk_capacity = virtio_reg_read64(VIRTIO_REG_DEVICE_CONFIG + 0) * SECTOR_SIZE;
    printf("virtio-blk: capacity is %d bytes\n", (int)blk_capacity);

    // 為磁碟請求配置空間
    blk_req_paddr = alloc_pages(align_up(sizeof(*blk_req), PAGE_SIZE) / PAGE_SIZE);
    blk_req = (struct virtio_blk_req *) blk_req_paddr;
}

// 通知裝置有新的請求
void virtq_kick(struct virtio_virtq *vq, int desc_index) {
    vq->avail.ring[vq->avail.index % VIRTQ_ENTRY_NUM] = desc_index;
    vq->avail.index++;
    __sync_synchronize();
    virtio_reg_write32(VIRTIO_REG_QUEUE_NOTIFY, vq->queue_index);
    vq->last_used_index++;
}

// 檢查裝置是否正在處理請求
bool virtq_is_busy(struct virtio_virtq *vq) {
    return vq->last_used_index != *vq->used_index;
}

// 讀取或寫入磁碟磁區
void read_write_disk(void *buf, unsigned sector, int is_write) {
    if (sector >= blk_capacity / SECTOR_SIZE) {
        printf("virtio: tried to read/write sector=%d, but capacity is %d\n",
               sector, (int)(blk_capacity / SECTOR_SIZE));
        return;
    }

    // 1. 依照 Virtio 規格建立請求內容
    blk_req->sector = sector;
    blk_req->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    if (is_write)
        memcpy(blk_req->data, buf, SECTOR_SIZE);

    // 2. 建立描述元鏈 (Descriptor Chain)，我們固定用前 3 個描述元
    struct virtio_virtq *vq = blk_request_vq;
    
    // [描述元 0] 請求標頭：包含類型與磁區號 (對裝置來說是唯讀)
    vq->descs[0].addr = blk_req_paddr;
    vq->descs[0].len = sizeof(uint32_t) * 2 + sizeof(uint64_t);
    vq->descs[0].flags = VIRTQ_DESC_F_NEXT;
    vq->descs[0].next = 1;

    // [描述元 1] 資料區塊：實際要讀寫的資料 (讀取時對裝置來說是可寫)
    vq->descs[1].addr = blk_req_paddr + offsetof(struct virtio_blk_req, data);
    vq->descs[1].len = SECTOR_SIZE;
    vq->descs[1].flags = VIRTQ_DESC_F_NEXT | (is_write ? 0 : VIRTQ_DESC_F_WRITE);
    vq->descs[1].next = 2;

    // [描述元 2] 狀態位元：裝置處理完後會寫入結果 (對裝置來說是可寫)
    vq->descs[2].addr = blk_req_paddr + offsetof(struct virtio_blk_req, status);
    vq->descs[2].len = sizeof(uint8_t);
    vq->descs[2].flags = VIRTQ_DESC_F_WRITE;

    // 3. 通知裝置處理請求
    virtq_kick(vq, 0);

    // 4. 忙碌等待 (Polling) 直到裝置處理完畢
    while (virtq_is_busy(vq))
        ;

    // 5. 檢查處理結果
    if (blk_req->status != 0) {
        printf("virtio: warn: failed to read/write sector=%d status=%d\n",
               sector, blk_req->status);
        return;
    }

    // 如果是讀取操作，把資料從共享區域搬回目的地 buffer
    if (!is_write)
        memcpy(buf, blk_req->data, SECTOR_SIZE);
}

// ==========================================
// 4. 例外與中斷處理 (Trap / System Call)
// ==========================================

__attribute__((naked)) __attribute__((aligned(4)))
void kernel_entry(void) {
    __asm__ __volatile__(
        "csrrw sp, sscratch, sp\n" // 案發現場：交換 User SP 與 Kernel SP
        "addi sp, sp, -4 * 31\n"   // 在核心堆疊開 31 格存暫存器
        "sw ra,  4 * 0(sp)\n"
        // ... (省略中間 sw 指令以節省篇幅) ...
        "sw s11, 4 * 29(sp)\n"

        "csrr a0, sscratch\n"      // 讀回案發現場的 User SP
        "sw a0, 4 * 30(sp)\n"      // 存入最後一格

        "mv a0, sp\n"              // 把整張病歷表 (trap_frame) 傳給醫生
        "call handle_trap\n"

        // --- 準備出院 ---
        "lw ra,  4 * 0(sp)\n"
        // ... (省略中間 lw 指令) ...
        "lw s11, 4 * 29(sp)\n"

        "addi sp, sp, 4 * 31\n"    // 移回核心堆疊頂端
        "csrrw sp, sscratch, sp\n" // 換回 User SP，sscratch 收回 Kernel SP
        "sret\n"
    );
}

void handle_syscall(struct trap_frame *f) {
    switch (f->a3) {
        case SYS_PUTCHAR:
            putchar(f->a0);
            break;
        case SYS_GETCHAR:
            while (1) {
                long ch = getchar();
                if (ch >= 0) { f->a0 = ch; break; }
                yield();
            }
            break;      
        case SYS_EXIT:
            printf("process %d exited\n", current_proc->pid);
            current_proc->state = PROC_EXITED;
            yield();
            PANIC("unreachable");  
        default:
            PANIC("unexpected syscall a3=%x\n", f->a3);
    }
}

void handle_trap(struct trap_frame *f) {
    uint32_t scause = READ_CSR(scause);
    uint32_t user_pc = READ_CSR(sepc);
    if (scause == SCAUSE_ECALL) {
        handle_syscall(f);
        user_pc += 4; // 系統呼叫後要回到下一行指令
    } else {
        PANIC("unexpected trap scause=%x, stval=%x, sepc=%x\n", 
               scause, READ_CSR(stval), user_pc);
    }
    WRITE_CSR(sepc, user_pc);
}

// ==========================================
// 5. 啟動區 (Main & Boot)
// ==========================================

void kernel_main(void) {
    memset(__bss, 0, (size_t) __bss_end - (size_t) __bss);
    printf("\n\nOS is booting...\n");

    WRITE_CSR(stvec, (uint32_t) kernel_entry); 

    // ✨ 新增：初始化磁碟裝置
    virtio_blk_init();

    // 實際測試磁碟 I/O
    char buf[SECTOR_SIZE];
    read_write_disk(buf, 0, false /* 從磁碟讀取 */);
    printf("first sector (read): %s\n", buf);

    strcpy(buf, "hello from kernel!!!\n");
    read_write_disk(buf, 0, true /* 寫入磁碟 */);

    // 再次讀取確認
    read_write_disk(buf, 0, false);
    printf("first sector (verify): %s\n", buf);

    // 初始化 idle 行程與真正的應用程式
    idle_proc = create_process(NULL, 0); 
    idle_proc->pid = 0;
    current_proc = idle_proc;

    create_process(_binary_shell_bin_start, (size_t) _binary_shell_bin_size);

    yield(); // 開始排程！
    PANIC("switched to idle process");
}

__attribute__((section(".text.boot"))) __attribute__((naked))
void boot(void) {
    __asm__ __volatile__(
        "mv sp, %[stack_top]\n"
        "j kernel_main\n"
        : : [stack_top] "r" (__stack_top)
    );
}