<h1 align="center"> 代码实现说明 </h1>


# 功能概述

本实现围绕 uCore 的文件系统栈展开，核心包括 VFS 抽象层、SFS（Simple FS）以及设备文件的统一访问路径。在此基础上完成两类关键能力：

- **文件访问**：通过 VFS/SFS 完成文件 `open/read/write/seek` 的端到端链路，SFS 侧的关键落点是 `sfs_io_nolock()`。
- **程序装载**：基于文件系统实现 `exec` 装载（从磁盘读取 ELF 并构建新的用户地址空间），关键落点是 `load_icode()`。

# 代码功能实现

## 文件系统分层与 open 链路

### open 的处理路径

当应用程序调用 `open()` 系统调用打开一个文件时,整个处理流程如下:

**1.1 用户层调用 (user/libs/file.c)**
- 应用程序调用 `open(path, flags)` 函数
- 该函数通过系统调用进入内核

**1.2 内核文件层处理 (kern/fs/file.c)**
- `file_open()` 函数处理文件打开请求
  - 根据 `open_flags` 确定文件的可读/可写属性
  - 调用 `fd_array_alloc()` 在进程的文件描述符表中分配一个空闲的文件描述符
  - 调用 VFS 层的 `vfs_open()` 打开文件,获取 inode
  - 设置文件位置 `pos`,如果是追加模式则定位到文件末尾
  - 将 inode 和读写权限信息保存到文件描述符中
  - 调用 `fd_array_open()` 标记文件为打开状态
  - 返回文件描述符 fd

**1.3 VFS层处理 (kern/fs/vfs/vfsfile.c)**
- `vfs_open()` 函数是 VFS 层的核心打开函数
  - 检查 `open_flags` 的合法性
  - 调用 `vfs_lookup(path, &node)` 查找文件路径对应的 inode
  - 如果文件不存在且设置了 `O_CREAT` 标志:
    - 调用 `vfs_lookup_parent()` 获取父目录的 inode
    - 调用 `vop_create()` 在父目录中创建新文件
  - 如果文件已存在且设置了 `O_EXCL` 标志,返回错误
  - 调用 `vop_open()` 打开 inode(文件系统相关操作)
  - 增加 inode 的打开计数 `vop_open_inc()`
  - 如果设置了 `O_TRUNC` 或是新创建的文件,调用 `vop_truncate()` 清空文件
  - 返回 inode 指针

**1.4 SFS文件系统层处理 (kern/fs/sfs/sfs_inode.c)**
- `sfs_lookup()` 负责在目录中解析路径并找到目标文件
  - `vfs_lookup()` 最终会调用到 `sfs_lookup()`/`sfs_lookup_once()`，在目录项中按文件名查找 inode 号
  - 找到 inode 号后通过 `sfs_load_inode()` 把磁盘 inode 加载到内存中，得到抽象层 inode
- `sfs_openfile()` 在你的代码里是一个“占位”实现（注释标注为 no use），直接返回 0
  - 因此 **真正的 open 语义检查主要发生在 VFS 层**（`vfs_open()` 对 O_CREAT/O_TRUNC/O_EXCL 等标志做统一处理）
  - 目录打开会走 `sfs_opendir()`，它会检查只读等限制

整个流程体现了 VFS 的分层设计思想:
- **用户层**: 提供统一的文件操作接口
- **文件描述符层**: 管理进程的打开文件表
- **VFS抽象层**: 提供文件系统无关的统一接口
- **具体文件系统层(SFS)**: 实现具体的磁盘操作

## 文件读写：`sfs_io_nolock()`

`sfs_io_nolock()` 是 SFS 的核心读写入口：根据文件内 `offset` 把“字节级 I/O”拆解为对若干磁盘块的读写，并在需要时通过块映射拿到对应物理块号。

### 参数说明
- `sfs`: SFS文件系统结构
- `sin`: 内存中的 SFS inode
- `buf`: 读写数据的缓冲区
- `offset`: 文件内的偏移量
- `alenp`: 指向需要读写长度的指针,函数返回时存储实际读写的长度
- `write`: 布尔值,0表示读,1表示写

### 实现思路

文件数据在磁盘上以块(block)为单位存储,每块大小为 `SFS_BLKSIZE` (通常为4KB)。读写操作需要处理三种情况:

**(1) 第一个块(可能非对齐)**
如果 `offset` 不是块大小的整数倍,需要从块的中间位置开始读写:
```c
if (blkoff != 0) {
    // 计算第一个块需要读写的大小
    size = (nblks > 0) ? (SFS_BLKSIZE - blkoff) : (endpos - offset);
    // 获取该逻辑块对应的物理块号
    if ((ret = sfs_bmap_load_nolock(sfs, sin, blkno, &ino)) != 0) {
        goto out;
    }
    // 读写该块的指定偏移和长度
    if ((ret = sfs_buf_op(sfs, buffer, size, ino, blkoff)) != 0) {
        goto out;
    }
    alen += size;
    buffer += size;
    blkno++;
    nblks--;
}
```

**(2) 中间的对齐块（以及可能的最后一个不足块）**
我的实现里为了逻辑统一，没有使用 `sfs_block_op` 做“多块批量读写”，而是循环逐块处理，并在每次循环里用 `remaining` 计算这一块需要读/写的字节数（可能是 `SFS_BLKSIZE`，也可能是最后一块的不足部分），然后用 `sfs_buf_op(..., offset=0)` 从块首地址开始读写：
```c
while (nblks > 0) {
    // 获取逻辑块对应的物理块号
    if ((ret = sfs_bmap_load_nolock(sfs, sin, blkno, &ino)) != 0) {
        goto out;
    }
    
    // 计算当前块需要读写的大小
    size_t remaining = endpos - (offset + alen);
    size = (remaining >= SFS_BLKSIZE) ? SFS_BLKSIZE : remaining;
    
    // 从块起始位置读写（这里统一使用 sfs_buf_op）
    if ((ret = sfs_buf_op(sfs, buffer, size, ino, 0)) != 0) {
        goto out;
    }
    
    alen += size;
    buffer += size;
    blkno++;
    nblks--;
}
```

同时，在函数前半部分我还做了必要的**边界与读语义裁剪**：

- 若 `offset < 0`、`offset >= SFS_MAX_FILE_SIZE`、或 `offset > endpos` 直接返回 `-E_INVAL`。
- 对读操作：若 `offset >= din->size` 直接返回 0；并把 `endpos` 裁剪到 `din->size`，保证不会读出文件尾。

### 关键函数说明

- `sfs_bmap_load_nolock(sfs, sin, index, &ino)`: 
  - 根据文件 inode 和逻辑块号 `index`,获取对应的物理磁盘块号 `ino`
  - 如果需要,会分配新的数据块
  
- `sfs_buf_op(sfs, buf, len, blkno, offset)`:
  - 对单个磁盘块进行非对齐的读写操作
  - 根据是否为写操作,指向 `sfs_rbuf` 或 `sfs_wbuf`
  - 支持从块的任意偏移位置读写指定长度的数据
  
- `sfs_block_op(sfs, buf, blkno, nblks)`:
  - 对连续的完整磁盘块进行读写
  - 根据是否为写操作,指向 `sfs_rblock` 或 `sfs_wblock`
  - 效率高于 `sfs_buf_op`,适用于完整块操作

### 与提示实现的差异（为何可行）

框架提示中给了 `sfs_block_op`（对应 `sfs_rblock/sfs_wblock`）用于“整块 + 连续多块”的高效读写；但我的实现为了简化处理路径，**对齐块仍然逐块调用 `sfs_buf_op`**。

这样做的结果是：

- **正确性**：不受影响。因为 `sfs_buf_op` 本身就支持对一个块内任意偏移/长度的读写，块对齐时用它读写整块也完全成立。
- **性能**：会略差一些（无法利用连续块批量 I/O）。在当前场景下可接受。

### 工作流程小结

1. 计算起始块号 `blkno` 和需要操作的块数 `nblks`
2. 计算第一个块内的偏移 `blkoff`
3. 如果第一个块非对齐(`blkoff != 0`),读写第一个块的部分数据
4. 循环处理中间的块,每次读写一个块
5. 更新已读写的数据长度 `alen`
6. 如果是写操作，更新 `sin->din->size` 并置 `sin->dirty=1`（对应代码里 `offset + alen > sin->din->size` 的更新逻辑），保证后续 `fsync` 能把 inode 元数据刷回磁盘


## 基于文件系统的程序装载：`load_icode()`
该实现把 `exec` 的装载来源切换为“文件系统中的 ELF 文件”：通过 `sysfile_open/read/seek/fstat` 读取程序文件，再按 ELF Program Header 建立 VMA、分配物理页并填充 TEXT/DATA/BSS，最后构造用户栈并设置 trapframe 进入用户态。

### 实现说明

我在 `kern/process/proc.c` 中的实现关键点如下（与下面粘贴的代码逐段对应）：

1. **load_icode_read：把文件 fd 变成“可随机读的字节流”**
    - 先用 `sysfile_seek(fd, offset, LSEEK_SET)` 定位到文件偏移。
    - 再用 `sysfile_read(fd, buf, len)` 读取指定长度。
    - 若实际读取字节数不等于 `len`，则认为读取失败（返回错误码或 `-1`），避免“读半截 ELF 头/段内容”导致后续解析越界。

2. **建立新的 mm 与页目录（并要求 old mm 已被回收）**
    - 一开始检查 `current->mm`，若不为空直接 `panic`：这对应 exec 语义——应先回收旧用户地址空间，再创建全新的地址空间。
    - `mm_create()` 创建新的 `mm_struct`，`setup_pgdir(mm)` 创建并初始化新的页目录。

3. **读取 ELF 头 + 文件大小并做越界检查**
    - `load_icode_read(fd, &elf, sizeof(elfhdr), 0)` 读取 ELF Header，并校验 `elf.e_magic == ELF_MAGIC`。
    - 通过 `sysfile_fstat(fd, &stat)` 获取 `file_size`，随后检查 `elf.e_phoff + ph_size <= file_size`，防止 program header 越界。
    - `kmalloc(ph_size)` 分配 program header 数组并读入。

4. **逐个装载 PT_LOAD 段：mm_map 建立 VMA，pgdir_alloc_page 分配页并拷贝内容**
    - 对每个 `ph_iter->p_type == ELF_PT_LOAD`：
      - 校验 `p_filesz <= p_memsz`，以及 `p_offset + p_filesz <= file_size`。
      - 用 `p_flags` 生成 `vm_flags`（VM_READ/VM_WRITE/VM_EXEC）与页表权限 `perm`（PTE_R/W/X + PTE_U/PTE_V）。
      - `mm_map(mm, p_va, p_memsz, vm_flags, NULL)` 建立段对应的 VMA。
      - 以页为单位循环：`pgdir_alloc_page(mm->pgdir, la, perm)` 分配页，计算页内偏移 `off` 与本次拷贝 `size`，再用 `load_icode_read` 把文件内容复制到 `page2kva(page)+off`。

5. **BSS 清零（memsz 超过 filesz 的部分）**
    - 先处理最后一个“已分配的页”里剩余的未覆盖区域：`memset(page2kva(page)+off, 0, size)`。
    - 若还需要更多页，则继续 `pgdir_alloc_page` 分配新页，并对对应范围清零。

6. **建立用户栈并布置 argc/argv（关键是地址从高到低）**
    - 先 `mm_map(mm, USTACKTOP-USTACKSIZE, USTACKSIZE, VM_READ|VM_WRITE|VM_STACK, NULL)` 建立栈 VMA。
    - 再固定分配 4 个栈页：`pgdir_alloc_page(mm->pgdir, USTACKTOP - i*PGSIZE, PTE_USER)`，`i=1..4`。
    - 栈布局（高 -> 低）：`字符串区 | argv[argc]=NULL | argv[] 指针数组 | argc`。
      - 先从 `USTACKTOP` 向下拷贝每个字符串，并记录每个字符串的用户栈地址到 `argv_array[i]`。
      - `ROUNDDOWN(stacktop, sizeof(uintptr_t))` 做指针对齐。
      - 压入 argv 指针数组（含 NULL 终止），得到 `argv_ptr`。
      - 最后压入 `argc`。

7. **切换页表并设置 trapframe，让用户态从入口点运行**
    - `current->mm = mm; current->pgdir = PADDR(mm->pgdir); lsatp(current->pgdir)` 切换到新页表。
    - 设置用户态寄存器：`sp=stacktop`，`a0=argc`，`a1=argv_ptr`，`epc=elf.e_entry`。
    - `status` 清除 SPP/SIE 并设置 SPIE，保证 `sret` 返回用户态时中断使能状态正确。

8. **失败清理：按阶段 goto 回收资源，避免泄漏**
    - 段/栈建立失败：`exit_mmap(mm)` 回收 vma 与页。
    - 随后 `put_pgdir(mm)` 回收页目录。
    - 最后 `mm_destroy(mm)` 释放 mm。
    - 临时 program header 内存 `ph` 在成功或失败路径都 `kfree(ph)`。

下面给出 `load_icode()` 的关键片段与对应解释（与上面说明一一对应）：

#### 代码分段讲解（逐段对应 `load_icode()`）

为了更容易对照阅读，我把你贴出的 `load_icode()` 按功能拆成 8 段。每段都给出“摘录代码 + 解释要点”，你可以一边看下面的完整代码一边对照本小节。

**第 1 段：前置条件检查 + 创建 mm + 建立页目录**

作用：保证 exec 语义是“重建地址空间”。因此旧 `mm` 必须为空，然后为新程序创建 `mm` 和页目录。

```c
if (current->mm != NULL) {
    panic("load_icode: current->mm must be empty.\n");
}
int ret = -E_NO_MEM;
struct mm_struct *mm;
if ((mm = mm_create()) == NULL) {
    goto bad_mm;
}
if (setup_pgdir(mm) != 0) {
    goto bad_pgdir_cleanup_mm;
}
```

**第 2 段：读取 ELF 头 + 魔数校验 + 获取文件大小**

作用：确认目标文件是合法 ELF，并拿到 `file_size` 给后续的边界检查使用。

```c
struct elfhdr elf;
if ((ret = load_icode_read(fd, &elf, sizeof(struct elfhdr), 0)) != 0) {
    goto bad_load_cleanup_pgdir;
}
if (elf.e_magic != ELF_MAGIC) {
    ret = -E_INVAL_ELF;
    goto bad_elf_cleanup_pgdir;
}
struct stat stat;
if (sysfile_fstat(fd, &stat) != 0) {
    ret = -E_INVAL;
    goto bad_stat_cleanup_pgdir;
}
size_t file_size = stat.st_size;
```

**第 3 段：读取 Program Header 表（带越界检查）**

作用：在分配并读取 PH 表之前，先验证 `e_phoff + ph_size` 不越界，防止读到文件末尾之外。

```c
struct proghdr *ph = NULL;
size_t ph_size = elf.e_phnum * sizeof(struct proghdr);
if (elf.e_phoff + ph_size > file_size) {
    ret = -E_INVAL_ELF;
    goto bad_ph_size_cleanup_pgdir;
}
if ((ph = kmalloc(ph_size)) == NULL) {
    ret = -E_NO_MEM;
    goto bad_ph_cleanup_pgdir;
}
if ((ret = load_icode_read(fd, ph, ph_size, elf.e_phoff)) != 0) {
    goto bad_ph_cleanup_pgdir;
}
```

**第 4 段：遍历 PT_LOAD 段 + 建 VMA + 计算页权限**

作用：只装载 `ELF_PT_LOAD` 段；用 `p_flags` 同时计算：

- `vm_flags`：VMA 的权限（VM_READ/VM_WRITE/VM_EXEC）
- `perm`：页表项权限（PTE_R/W/X + PTE_U + PTE_V）

同时检查 `p_filesz <= p_memsz`、`p_offset + p_filesz <= file_size` 防止伪造 ELF。

```c
if (ph_iter->p_filesz > ph_iter->p_memsz) {
    ret = -E_INVAL_ELF;
    goto bad_cleanup_mmap;
}
if (ph_iter->p_offset + ph_iter->p_filesz > file_size) {
    ret = -E_INVAL_ELF;
    goto bad_cleanup_mmap;
}
uint32_t vm_flags = 0, perm = PTE_U | PTE_V;
if (ph_iter->p_flags & ELF_PF_X) vm_flags |= VM_EXEC;
if (ph_iter->p_flags & ELF_PF_W) vm_flags |= VM_WRITE;
if (ph_iter->p_flags & ELF_PF_R) vm_flags |= VM_READ;
if (vm_flags & VM_READ) perm |= PTE_R;
if (vm_flags & VM_WRITE) perm |= (PTE_W | PTE_R);
if (vm_flags & VM_EXEC) perm |= PTE_X;
if ((ret = mm_map(mm, ph_iter->p_va, ph_iter->p_memsz, vm_flags, NULL)) != 0) {
    goto bad_cleanup_mmap;
}
```

**第 5 段：按页分配并复制文件内容（TEXT/DATA）**

作用：以页粒度分配并填充内容。你这里处理了“段起始地址不页对齐”的情况：

- `la = ROUNDDOWN(start, PGSIZE)` 让 `la` 对齐到页
- `off = start - la` 得到页内偏移

```c
uintptr_t start = ph_iter->p_va, end, la = ROUNDDOWN(start, PGSIZE);
end = ph_iter->p_va + ph_iter->p_filesz;
while (start < end) {
    if ((page = pgdir_alloc_page(mm->pgdir, la, perm)) == NULL) {
        goto bad_cleanup_mmap;
    }
    off = start - la, size = PGSIZE - off, la += PGSIZE;
    if (end < la) {
        size -= la - end;
    }
    if ((ret = load_icode_read(fd, page2kva(page) + off, size,
                              ph_iter->p_offset + (start - ph_iter->p_va))) != 0) {
        goto bad_cleanup_mmap;
    }
    start += size;
}
```

**第 6 段：BSS 清零（memsz 比 filesz 多出来的部分）**

作用：把 `p_memsz - p_filesz` 对应的内存区域清零。

你这里分两步做：

1) 先清零“最后一个已分配页里剩余部分”；
2) 若仍未覆盖到 `p_memsz`，继续分配新页并清零。

```c
end = ph_iter->p_va + ph_iter->p_memsz;
if (start < la) {
    if (start == end) {
        continue;
    }
    off = start - la, size = PGSIZE - off;
    if (end < la) {
        size -= la - end;
    }
    memset(page2kva(page) + off, 0, size);
    start += size;
}
while (start < end) {
    if ((page = pgdir_alloc_page(mm->pgdir, la, perm)) == NULL) {
        goto bad_cleanup_mmap;
    }
    off = start - la, size = PGSIZE - off, la += PGSIZE;
    if (end < la) {
        size -= la - end;
    }
    memset(page2kva(page) + off, 0, size);
    start += size;
}
```

**第 7 段：映射用户栈 + 组织 argc/argv + 设置 trapframe**

作用：为用户态准备 ABI 约定的入口环境。

- `mm_map` 建立用户栈 VMA
- 分配 4 页栈页
- 从高地址向低地址复制字符串，再放 argv 指针数组，最后压入 argc
- `lsatp` 切换到新页表，设置 `sp/a0/a1/epc/status`

```c
uint32_t vm_flags = VM_READ | VM_WRITE | VM_STACK;
if ((ret = mm_map(mm, USTACKTOP - USTACKSIZE, USTACKSIZE, vm_flags, NULL)) != 0) {
    goto bad_cleanup_mmap;
}
for (int i = 1; i <= 4; i++) {
    if (pgdir_alloc_page(mm->pgdir, USTACKTOP - i * PGSIZE, PTE_USER) == NULL) {
        goto bad_cleanup_mmap;
    }
}
// ... 复制字符串/构造 argv/压入 argc ...
current->mm = mm;
current->pgdir = PADDR(mm->pgdir);
lsatp(current->pgdir);
current->tf->gpr.sp = stacktop;
current->tf->gpr.a0 = argc;
current->tf->gpr.a1 = argv_ptr;
current->tf->epc = elf.e_entry;
current->tf->status = (read_csr(sstatus) & ~(SSTATUS_SPP | SSTATUS_SIE)) | SSTATUS_SPIE;
```

**第 8 段：goto 失败清理（避免内存/页表泄漏）**

作用：任何一步失败都能回收已分配的资源。

- `exit_mmap(mm)`：回收已建立的 vma 与页面
- `put_pgdir(mm)`：回收页目录
- `mm_destroy(mm)`：释放 mm
- `ph` 在失败路径释放

```c
bad_cleanup_mmap:
    exit_mmap(mm);
bad_ph_cleanup_pgdir:
    if (ph != NULL) {
        kfree(ph);
    }
bad_load_cleanup_pgdir:
    put_pgdir(mm);
bad_pgdir_cleanup_mm:
    mm_destroy(mm);
bad_mm:
    goto out;
```

（完整 `load_icode()` 代码较长，这里仅保留关键片段与实现思路，避免在说明文档中重复贴出全部源码。）


## 挑战实现代码：匿名管道 pipe（设计方案）

目标：在 ucore 中加入匿名管道 `pipe()`，实现“一个内核缓冲区 + 两端文件描述符”的半双工通信。

#### 1. 需要新增/扩展的数据结构

参考现有的“文件 = inode + file descriptor + vop_read/vop_write”分层结构，我倾向于把 pipe 作为一种新的 inode 类型（或设备类 inode），让其复用 `sysfile_read/sysfile_write` 路径。

可以新增：

```c
// pipe 的核心状态
typedef struct pipe {
    // 环形缓冲区
    char *buf;
    size_t cap;
    size_t rpos;
    size_t wpos;
    size_t used;

    // 端点引用计数：用于 close 语义
    int readers;
    int writers;

    // 并发控制
    semaphore_t mutex;        // 保护 used/rpos/wpos/readers/writers
    semaphore_t can_read;     // 用于阻塞读：used==0 且 writers>0
    semaphore_t can_write;    // 用于阻塞写：used==cap 且 readers>0
} pipe_t;

// inode 的具体信息（示意：需要扩展 inode union / inode_type）
struct pipe_inode {
    pipe_t *pipe;
    bool is_read_end;
    bool is_write_end;
};
```

同步互斥要点：
- **互斥**：对环形队列的读写位置与计数更新必须在 `mutex` 保护下完成。
- **同步**：
  - 读：若 `used==0` 且仍存在写端（`writers>0`），读者应睡眠/阻塞等待 `can_read`；若 `writers==0`，读返回 0（EOF）。
  - 写：若 `used==cap` 且仍存在读端（`readers>0`），写者应阻塞等待 `can_write`；若 `readers==0`，写返回错误（类 UNIX 的 EPIPE）。

#### 2. 需要提供的接口（语义说明）

**系统调用层**
- `int sys_pipe(int pipefd[2]);`
  - 创建 pipe，返回两个 fd：`pipefd[0]` 只读，`pipefd[1]` 只写

**VFS/文件层对接**
为了复用现有 `sysfile_read/sysfile_write/sysfile_close`，pipe 需要提供类似 inode 的操作：
- `pipe_open/pipe_close/pipe_read/pipe_write`
  - `pipe_read`/`pipe_write` 语义如上（阻塞与EOF/EPIPE）
  - `pipe_close`：减少对应端点计数（readers/writers），并唤醒对端（防止永久睡眠）

#### 3. 与现有代码结构的结合点

- 当前实现的 `do_execve()`/`load_icode()` 已经通过 `sysfile_open/read/seek` 走通了“fd -> 文件系统 -> vop_read”路径。
- pipe 机制可以复用同一条路径，只需让 `sysfile_read/sysfile_write` 能识别并调用到 pipe 对应的 inode_ops（或设备 ops）。



## 挑战实现代码：链接机制（设计方案）

SFS 已经具备实现链接机制的“数据基础”：
- `struct sfs_disk_inode` 含 `nlinks` 字段（硬链接计数）
- `struct sfs_disk_entry` 目录项里存放 `ino`（多个目录项可指向同一 inode）
- `sfs.h` 已定义 `SFS_TYPE_LINK`（为软链接预留）
- VFS 层接口 `vfs_link/vfs_unlink/vfs_symlink/vfs_readlink` 已声明，但在你的 `vfsfile.c` 中目前大多返回 `-E_UNIMP`

#### 1. 硬链接（hard link）设计

**语义**
- `link(old, new)`：在 `new` 的父目录中新建一个目录项，目录项的 `ino` 指向 `old` 对应的 inode，并将 inode 的 `nlinks++`
- `unlink(path)`：删除目录项并将 inode 的 `nlinks--`；当 `nlinks==0` 且 inode 没有被打开（可结合 VFS 的 open_count/ref_count）时回收数据块与 inode

**需要的接口（建议）**
- VFS：
  - `int vfs_link(char *old_path, char *new_path);`
  - `int vfs_unlink(char *path);`
- SFS（文件系统内部建议拆分为若干原子步骤，均需要加锁保护目录 inode）：
  - 查找 `old_path` 得到目标 inode（需 `vfs_lookup`）
  - `vfs_lookup_parent(new_path, &dir, &name)` 找到新目录与文件名
  - 在 `dir` 中分配空目录项（复用类似 `sfs_dirent_search_nolock` 的扫描思路寻找空槽位）
  - 写入目录项（name、ino）并 `nlinks++`

**同步互斥要点**
- 目录内容更新必须串行化：对目录 inode 加锁（你的 SFS inode 已有 `semaphore_t sem`，可用 `lock_sin/unlock_sin` 思路）。
- 防止并发 `link/unlink` 造成 `nlinks` 不一致：更新 inode 的 `nlinks` 与目录项写入应被视为一个事务（失败回滚）。

#### 2. 软链接（symbolic link）设计

**语义**
- `symlink(target, linkpath)`：创建一个类型为 `SFS_TYPE_LINK` 的“链接文件”，其内容存放目标路径字符串 `target`
- `readlink(linkpath, buf)`：读取软链接文件内容（即 target 路径）
- 路径解析时：若遇到 `SFS_TYPE_LINK`，可选择“自动跟随”（像 `open`/`exec`）或“不跟随”（像 `lstat`），这需要在 VFS 的 lookup/open 语义中区分

**需要的数据结构/接口（建议）**

```c
// 软链接不一定需要新的 on-disk 结构：复用普通文件的数据块存储 target 字符串即可
// 关键是 inode->type == SFS_TYPE_LINK

int vfs_symlink(char *target, char *linkpath);
int vfs_readlink(char *linkpath, struct iobuf *iob);
```

实现方式要点：
- `vfs_symlink`：
  - 通过 `vfs_lookup_parent` 找到父目录
  - 创建一个 inode（类型 `SFS_TYPE_LINK`，`size=strlen(target)`）
  - 调用文件写入路径（最终落到 `sfs_io_nolock`）把 target 字符串写入数据块
- `vfs_readlink`：
  - 查找 linkpath 对应 inode
  - 读出 inode 内容到用户缓冲

同步互斥：
- 创建软链接需要对父目录与新 inode 的元数据更新加锁
- 读链接内容属于读操作，但仍需遵守 inode 级别的读写互斥（你的 SFS inode 已用 `sem` 做读写保护）

