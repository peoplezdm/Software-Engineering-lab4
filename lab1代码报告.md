<h1 align="center"> 代码实现说明 </h1>


# 功能概述
本代码实现了最小可执行内核和启动流程，在Qemu模拟器上运行64位RISC-V计算机。

# 代码功能实现


## 内核启动中的程序入口操作

### 实现的代码
内核启动中的程序入口操作主要在kern/init/entry.S中，这是一段RISC-V汇编代码，如下为对其代码的解释：

    #include <mmu.h>
    #include <memlayout.h>
以上代码用于包含头文件。mmu.h定义了与内存管理单元（MMU）相关的常量，例如页表项的标志位、页大小等，memlayout.h头文件通常定义了内存布局的常量，例如内核起始地址、设备地址、栈地址等。

        .section .text,"ax",%progbits
.section是指令，告诉汇编器接下来的内容属于哪个段。

.text段表明是只读的可执行代码段。

"ax"是个标志，"a"代表该段在程序加载时需要被分配到内存中,"x"表示该段包含可执行指令。

%progbits 是段类型，即“程序定义内容”，即这个段中存放的是程序自己定义的代码或数据。

        .globl kern_entry
声明符号kern_entry为全局符号，使链接器与其它目标文件可见，并可作为 ELF 的入口点。

    kern_entry:
标签kern_entry，表示内核入口的地址。系统启动后会把控制权交到这里。

        la sp, bootstacktop
la = "load address"，顾名思义，用于加载地址。它把标签 bootstacktop 代表的地址加载到栈指针sp里，sp寄存器指向当前函数调用栈的顶部。

这句话的效果就是将堆栈指针初始化为 bootstacktop，也就是把栈指向预分配栈区域的最高地址。后继的代码会按照调用约定使用 sp，所以需要先建立一个安全可用的内核栈。


        tail kern_init
tail 是一个尾调用的伪指令：它会把控制权转移到 kern_init，并且不会保存返回地址，也就是不会再返回到 kern_entry。它会实现直接无返回地跳到kern_init，把 kern_entry 的栈帧“替换”为 kern_init 的执行环境。

    .section .data
切换到 .data 段，后面定义的数据会被放在可写数据段中。

        # .align 2^12
这是个注释，写明是按 2^12 对齐（4096 字节，即一页）。

        .align PGSHIFT
.align PGSHIFT 使用宏 PGSHIFT（通常定义为 12）来做对齐；.align N 一般按 2^N 字节对齐，因此 .align 12就是按照4096字节对齐）。

        .global bootstack
声明全局符号bootstack，表示栈底。

    bootstack:
        .space KSTACKSIZE

在汇编输出中预留 KSTACKSIZE个 字节（全为零），用于作为内核的初始栈空间。KSTACKSIZE 源自 memlayout.h 的宏定义，是2*4096个字节，即2个页。

        .global bootstacktop
    bootstacktop:

定义 bootstacktop 标签，通常放在栈空间的末尾（高地址）。la sp, bootstacktop 正是把 sp 设置为这里。

bootstacktop 没有紧随数据后面的空间指令，因为它就是栈空间的结束位置标签。

内核启动的总体流程大体如下：

加电复位 → CPU从0x1000进入MROM → 跳转到0x80000000(OpenSBI) → OpenSBI初始化并加载内核到0x80200000 → 跳转到entry.S → 调用kern_init() → 输出信息 → 结束


我们都知道c代码运行中栈的重要性，所以，内核启动必须实现给 CPU 一个安全的栈，让 C 函数能正常执行。

指令 la sp, bootstacktop 实现了初始化内核的栈指针寄存器，目的是为后续的函数调用和局部变量分配提供合法的栈空间。结合上边的代码阅读，我可以知道系统为这个栈分配了两个页的空间大小，所以这个代码实现了让 CPU 从这块 8KB 的内存最高处开始作为栈，逐渐向下使用。

tail kern_init实现了无条件跳转到函数kern_init，并且不再返回。也就是永久的把cpu控制权转移给kern_init。在启动汇编（kern_entry）阶段，我们只能执行非常简单的汇编指令，做少量硬件初始化。复杂的操作（比如内存检测、页表建立、中断初始化、设备注册等）都必须用C来完成。这个代码的目的就是结束kern_entry阶段，把控制权交给 C 语言内核。而 kern_init() 就是整个 C 内核的入口函数。

总而言之， kern/init/entry.S的作用就是为CPU分配安全的栈空间，然后跳转到用C语言实现的内核里。






