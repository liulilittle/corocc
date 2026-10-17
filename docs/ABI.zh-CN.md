# ABI — 二进制接口契约

> [English](ABI.md) | 中文

本文档是 corocc 的应用二进制接口(ABI)契约:已编译的消费方可以依赖什么、
每个完整公开类型的布局如何被钉死、以及哪些源码改动与之兼容。逐函数契约见
各 API 参考([coro.zh-CN.md](api/coro.zh-CN.md)、
[coro_l0.zh-CN.md](api/coro_l0.zh-CN.md)、
[coro_rt.zh-CN.md](api/coro_rt.zh-CN.md)、
[coro_sched.zh-CN.md](api/coro_sched.zh-CN.md)、
[coro_scope.zh-CN.md](api/coro_scope.zh-CN.md)、
[coro_cpp.zh-CN.md](api/coro_cpp.zh-CN.md)、
[coro_rt_driver.zh-CN.md](api/coro_rt_driver.zh-CN.md));本页是跨面的二进制契约与实测
布局表。

## ABI 覆盖范围

| 面 | 本文档钉死的内容 |
| --- | --- |
| 公开函数 | 名称、签名、调用约定、返回类型(`extern "C"`)。 |
| 公开枚举 | `coro_status`、`coro_state`、`coro_driver_flags`:数值冻结。 |
| 完整公开结构体 | `coro_stack`、`coro_ctx`、`coro_attr_t`、`coro_allocator_t`、`coro_rt_mutex`、`coro_rt_cond`、`coro_rt_sem`、`coro_rt_rwlock`、`coro_rt_channel`、`coro_rt_scope_t`、`coro_rt_driver_ops`:`sizeof`、对齐、字段偏移。 |
| 不透明句柄 | `coro_t`、`coro_rt_t`、`coro_sched_t`、`coro_rt_driver_t`:仅指针宽度——其背后的布局为私有,可自由变化。 |
| 符号可见性 | `COROCC_API` 宏(`coro_status.h`):公开头中唯一的平台条件,只影响符号装饰,绝不影响布局。 |
| 常量 | `CORO_DEFAULT_STACK_SIZE`、`CORO_LOCAL_SLOTS`、`CORO_TOKEN_NONE`、`CORO_TOKEN_DEAD`、`CORO_F_NONE`。 |

未列出的都是实现细节。特别地,公开头中**没有任何影响布局的条件编译**:
`include/corocc/` 里唯一的 `_WIN32` 条件就是 `coro_status.h` 中的
`COROCC_API` 宏。

运行时驱动层(`coro_rt_driver.h`)是**纯新增**:它引入完整结构体
`coro_rt_driver_ops`(其首字段 `struct_size` 为 vtable 的前向兼容扩展打标)、
枚举 `coro_driver_flags`、不透明句柄 `coro_rt_driver_t` 与若干新函数;而
`coro_rt.h` 的面与内置后端不变。未挂驱动时运行时逐位走内置路径。

## 稳定性承诺

在同一 ABI 主版本线内(当前为 **1.x**):

- **冻结:** 所有既有公开符号(函数名与签名)、所有既有枚举值、所有完整结构体
  的 `sizeof`、对齐与字段偏移。
- **语义冻结:** 每个状态码与每个入口的文档化含义;完整结构体的字段**顺序**。
- **允许新增:** 新函数、新的不透明句柄类型、新的完整类型,以及**追加在既有
  成员之后**(绝不插入)的新枚举成员。`coro_status` 只在尾部增长,因此对旧
  数值的比较含义永不改变。
- **1.x 内禁止:** 重排、增删或改变既有完整结构体的字段尺寸;改动既有枚举值;
  改动函数签名、返回类型或调用约定;删除或重命名符号。

无法遵守上述规则的改动属于 **2.0** 变更:ABI 主版本即包主版本,2.x 是全新的
平行库——新旧二进制不在同一 ABI 组件内混用。

跨组件混用:与任何 C 库一样,同一程序内的所有翻译单元必须对其编译所用头版本
保持一致。链接同一主版本线内两个不同头版本的二进制在构造上即有缺陷;在 1.x
内,源码兼容意味着"重新编译总是足够的"。

## 指针模型与布局

所有完整公开结构体只由 `void*` 指针、`size_t`、`uint64_t` 与 32 位枚举构成。
这恰好产生如下几种布局:

- **LP64**(Linux 与 macOS、多数 Unix 64 位):指针、`size_t`、`long` 均为
  8 字节。macOS x86_64 与 Apple Silicon arm64 均为 LP64,故下方 64 位布局表
  原样适用(与 LLP64 同理,原因相同:公开字段不使用 `long`)。
- **LLP64**(64 位 Windows,x64 与 ARM64):指针与 `size_t` 为 8 字节,`long`
  为 4 字节——
  公开字段不使用 `long`,因此 **LLP64 与 LP64 布局完全一致**。Windows ARM64
  同为 LLP64,遵循同一张 64 位表(CI 的 windows-arm64 任务在 ARM64 主机上
  运行全量套件)。
- **ILP32,`uint64_t` 8 字节对齐**(ARM EABI、MIPS o32、Win32 MSVC):指针与
  `size_t` 为 4 字节;`uint64_t` 为 8 字节、对齐 8。已在 armhf、mips o32 与
  Windows x86 实测;其余 32 位 ARM/MIPS/riscv 目标遵循同一规则。MSVC x86
  默认(`/Zp8`)保持 `__int64` 8 字节对齐,因此 Win32 归入本表而非 4 字节表。
- **ILP32,`uint64_t` 4 字节对齐**(i386 SysV、Android x86):指针与 `size_t`
  为 4 字节;`uint64_t` 为 8 字节、对齐 4。适用于 i386 目标与 Android x86
  ABI。四个纯指针结构体
  (`coro_stack`、`coro_ctx`、`coro_attr_t`、`coro_allocator_t`)与另一种
  ILP32 变体一致;六个含 `uint64_t` 的结构体更小。

下方表格是 ABI 探针在全部本机可用平台上的**实测**值,彼此一致:

| 站点 | 指针模型 | 结果 |
| --- | --- | --- |
| Windows x64,MSVC(`cl`,VS2022 17.x,`/W4 /WX`) | LLP64 | 与 LP64 表一致 |
| Windows x86(Win32),MSVC(`cl`,VS2022 17.x,`/W4 /WX`) | ILP32,`uint64_t` 8 字节对齐 | 与第一张 ILP32 表一致 |
| Linux x86-64,GCC 13 | LP64 | 基线 |
| Linux aarch64,GCC 13 / `qemu-aarch64` | LP64 | 一致 |
| Linux riscv64,GCC 13 / `qemu-riscv64` | LP64 | 一致 |
| Linux ppc64le,GCC 13 / `qemu-ppc64le` | LP64 | 一致 |
| Linux s390x(大端),GCC 13 / `qemu-s390x` | LP64 | 一致 |
| Linux armhf(armv7 ABI),GCC 13 / `qemu-arm` | ILP32,`uint64_t` 8 字节对齐 | 与第一张 ILP32 表一致 |
| Linux mips(o32,大端),GCC / `qemu-mips` | ILP32,`uint64_t` 8 字节对齐 | 与 armhf 一致 |
| Linux i386(x86 32 位 SysV),GCC 13 | ILP32,`uint64_t` 4 字节对齐 | 与 i386 表一致 |
| Android NDK clang 18(r27d),`armeabi-v7a` | ILP32,`uint64_t` 8 字节对齐 | 与第一张 ILP32 表一致(record-layout dump) |
| Android NDK clang 18(r27d),`x86` | ILP32,`uint64_t` 4 字节对齐 | 与 i386 表一致(record-layout dump) |

字节序不改变任何偏移:上述布局规则与端序无关;但二进制仍然只能与**相同**
ABI(架构、端序、位宽、ABI 变体与 C 库)互换。上表中的大端站点是为了证明
这一论断,而不是让二进制可互换。

### 64 位布局(LP64 与 LLP64 完全一致)

| 类型 | `sizeof` | 对齐 | 字段偏移 |
| --- | --- | --- | --- |
| `coro_stack` | 40 | 8 | `base` 0,`total` 8,`lo` 16,`usable` 24,`guard_bytes` 32 |
| `coro_ctx` | 48 | 8 | `fctx` 0,`stack` 8,`entry` 16,`arg` 24,`back` 32,`asan_fake` 40 |
| `coro_attr_t` | 32 | 8 | `stack_size` 0,`name` 8,`flags` 16,`allocator` 24 |
| `coro_allocator_t` | 32 | 8 | `alloc_stack` 0,`free_stack` 8,`alloc_handle` 16,`free_handle` 24 |
| `coro_rt_mutex_t` | 16 | 8 | `locked` 0,`waitq` 8 |
| `coro_rt_cond_t` | 8 | 8 | `waitq` 0 |
| `coro_rt_sem_t` | 16 | 8 | `count` 0,`waitq` 8 |
| `coro_rt_rwlock_t` | 24 | 8 | `state` 0,`rd_waitq` 8,`wr_waitq` 16 |
| `coro_rt_channel_t` | 64 | 8 | `slots` 0,`cap` 8,`len` 16,`head` 24,`limit` 32,`closed` 40,`sendq` 48,`recvq` 56 |
| `coro_rt_scope_t` | 88 | 8 | `state` 0,`rt` 8,`children` 16,`nchildren` 24,`cap` 32,`live` 40,`status` 48,`waitq` 56,`first_done` 64,`first_value` 72,`timer_fired` 80 |

### 32 位布局(ILP32)

纯指针结构体在两种 ILP32 变体上完全一致:

| 类型 | `sizeof` | 对齐 | 字段偏移 |
| --- | --- | --- | --- |
| `coro_stack` | 20 | 4 | `base` 0,`total` 4,`lo` 8,`usable` 12,`guard_bytes` 16 |
| `coro_ctx` | 24 | 4 | `fctx` 0,`stack` 4,`entry` 8,`arg` 12,`back` 16,`asan_fake` 20 |
| `coro_attr_t` | 16 | 4 | `stack_size` 0,`name` 4,`flags` 8,`allocator` 12 |
| `coro_allocator_t` | 16 | 4 | `alloc_stack` 0,`free_stack` 4,`alloc_handle` 8,`free_handle` 12 |

含 `uint64_t` 的结构体,**`uint64_t` 8 字节对齐**(实测:armhf、mips o32):

| 类型 | `sizeof` | 对齐 | 字段偏移 |
| --- | --- | --- | --- |
| `coro_rt_mutex_t` | 16 | 8 | `locked` 0,`waitq` 8 |
| `coro_rt_cond_t` | 4 | 4 | `waitq` 0 |
| `coro_rt_sem_t` | 16 | 8 | `count` 0,`waitq` 8 |
| `coro_rt_rwlock_t` | 16 | 8 | `state` 0,`rd_waitq` 8,`wr_waitq` 12 |
| `coro_rt_channel_t` | 40 | 8 | `slots` 0,`cap` 4,`len` 8,`head` 12,`limit` 16,`closed` 24,`sendq` 32,`recvq` 36 |
| `coro_rt_scope_t` | 72 | 8 | `state` 0,`rt` 8,`children` 12,`nchildren` 16,`cap` 20,`live` 24,`status` 32,`waitq` 40,`first_done` 48,`first_value` 56,`timer_fired` 64 |

含 `uint64_t` 的结构体,**`uint64_t` 4 字节对齐**(实测:i386 SysV):

| 类型 | `sizeof` | 对齐 | 字段偏移 |
| --- | --- | --- | --- |
| `coro_rt_mutex_t` | 12 | 4 | `locked` 0,`waitq` 8 |
| `coro_rt_cond_t` | 4 | 4 | `waitq` 0 |
| `coro_rt_sem_t` | 12 | 4 | `count` 0,`waitq` 8 |
| `coro_rt_rwlock_t` | 16 | 4 | `state` 0,`rd_waitq` 8,`wr_waitq` 12 |
| `coro_rt_channel_t` | 36 | 4 | `slots` 0,`cap` 4,`len` 8,`head` 12,`limit` 16,`closed` 20,`sendq` 28,`recvq` 32 |
| `coro_rt_scope_t` | 64 | 4 | `state` 0,`rt` 8,`children` 12,`nchildren` 16,`cap` 20,`live` 24,`status` 28,`waitq` 36,`first_done` 40,`first_value` 48,`timer_fired` 56 |

## 枚举

`coro_status`(全部受支持 ABI 上均为 4 字节;数值冻结):

| 值 | 数值 | 返回场景(摘要;完整表见 [coro_l0.zh-CN.md](api/coro_l0.zh-CN.md)) |
| --- | --- | --- |
| `CORO_OK` | 0 | 成功。 |
| `CORO_EINVAL` | 1 | 参数非法:NULL、未初始化、尺寸不可用、槽键越界。 |
| `CORO_ENOMEM` | 2 | 分配失败(句柄、栈映射、ring 增长、worker 启动)。 |
| `CORO_EBUSY` | 3 | 目标忙:运行中争抢销毁、通道已满、对象未排空、子协程存活。 |
| `CORO_ESTATE` | 4 | 状态错误:无匹配持有的解锁、对取消中/已排空 scope 的 spawn、重复 start。 |
| `CORO_ECANCELED` | 5 | 在安全点观察到取消;`wait_for` 期限到达。 |
| `CORO_EDESTROYED` | 6 | 目标已结束/已回收;投递给已退出线程的唤醒。 |
| `CORO_ESTALE` | 7 | 凭证过期;或"必须在运行时协程内调用"。 |
| `CORO_EAGAIN` | 8 | 暂时性状态;`coro_join` 对活目标的非阻塞答复。 |
| `CORO_ECLOSED` | 9 | 通道已关闭(已缓冲项先排空)。 |

`coro_state`(4 字节;数值冻结):`CORO_NEW` 0、`CORO_READY` 1、
`CORO_RUNNING` 2、`CORO_SUSPENDED` 3、`CORO_DEAD` 4。

`coro_ctx_init` 是唯一**不**返回 `coro_status` 的入口:它返回 `int`(成功 0,
参数非法时负 `CORO_EINVAL`)。这是冻结签名的一部分。

## 结构体传值

只有 `coro_attr_default()` **按值**返回结构体(`coro_attr_t`,64 位上 32 字节/
32 位上 16 字节);其余边界全部使用指针或标量。按值返回遵循平台标准的结构体
返回规则(这一尺寸的结构体走内存返回),因此在同一平台混用编译器无需特殊处理。

## 符号可见性与调用约定

- 所有公开函数都声明于 `extern "C"` 块内(以 C++ 编译时)并使用平台默认 C
  调用约定;公开面没有变参函数、没有 `__stdcall`/`__fastcall` 装饰、没有名字
  修饰。
- `COROCC_API` 是公开头中唯一的平台条件:
  - Windows:库自身以 `COROCC_BUILDING` 编译时是 `__declspec(dllexport)`,
    消费方为 `__declspec(dllimport)`。**静态库构建为自身与其消费方定义
    `COROCC_BUILDING=1`**(CMake target 已如此设置),从而不残留任何 `__imp_`
    间接。
  - ELF/Mach-O:`__attribute__((visibility("default")))`,库以
    `-fvisibility=hidden` 编译时仍导出公开符号。
- vendored 上下文切换汇编导出重命名后的符号(`corocc_asm_make`、
  `corocc_asm_jump`),不属于公开面。

## 32 位链接接口注记(atomic)

在工具链不提供内联 64 位 compare-and-swap 的 32 位构建上,库的少量操作依赖
`libatomic`。预构建 CMake 包**仅在构建宽度探针未发现内联 64 位 CAS 时**把
`atomic` 追加到 `corocc::corocc` 的链接接口(与 `cmake/arch_select.cmake`
一致)。消费预编译归档无需手动改动;此处记录是因为它是这些归档链接 ABI 的
一部分。

## 版本策略

- ABI 主版本与包主版本一致(当前 `1.x`)。
- 1.x 内冻结:布局、枚举值、签名、语义(见"稳定性承诺")。
- 预构建 CMake 包经 `corocc-config-version.cmake` 报告版本;
  `find_package(corocc 1.0 CONFIG)` 接受任何兼容的 1.x。
- **不承诺**跨主版本的二进制兼容。

## Android NDK 与交叉工具链注记

- **Android NDK(r26+)。** 经 `scripts/toolchains/android-ndk.cmake` 提供
  编译链接支持:该文件选择工具链与目标 ABI(`ANDROID_ABI`:`arm64-v8a`、
  `armeabi-v7a`、`x86_64`、`x86`),Android API level 24,静态 C++ STL。
  CI 对全部四个 ABI 做编译+链接验证;设备上运行不在模拟器矩阵内。布局
  (经 NDK clang record-layout dump 实测):`arm64-v8a` 与 `x86_64` 为 LP64
  (64 位表);`armeabi-v7a` 为 ILP32、`uint64_t` 8 字节对齐(第一张 ILP32
  表);`x86` 为 ILP32、`uint64_t` 4 字节对齐(i386 表)——Android x86 归入
  4 字节表,与 Win32 MSVC x86 的 8 字节对齐不同。
- **macOS(Mach-O)。** macOS x86_64 与 arm64 的原生构建使用 vendored Mach-O
  后端(`x86_64_sysv_macho_gas.S`、`arm64_aapcs_macho_gas.S`);两者都是
  LP64,遵循 64 位布局表。公开头保持与 ELF 相同的 `extern "C"` 表面与
  `visibility("default")` 拼写;仅有的 Darwin 平台调整都在内部(条件变量
  实时时钟期限与 `MAP_ANON`)。Mach-O 产物中的符号带常规 C 下划线前缀
  (`_corocc_asm_make`),这是该平台的标准翻译,不改变文档所述 C 名称。
- **交叉工具链。** 交叉构建使用 `scripts/toolchains/` 下的工具链文件(每个
  ABI 一个;ISA 清单见 [platforms.zh-CN.md](platforms.zh-CN.md)),面向模拟器
  运行做静态链接,并按 ISA 在 `qemu-user` 下执行。不同目标产出的二进制**不可
  互换**:每个三元组都是独立的 ABI 包(每三元组的 `libcorocc.a`,预构建布局
  见 [building.zh-CN.md](building.zh-CN.md))。
- **s390x 专用编译开关。** s390x 工具链使用 `-fno-stack-protector`:gcc 的
  s390x TLS 访问会把线程指针物化到被调用者保存寄存器,而跨线程的上下文切换
  会恢复过期的值,栈保护金丝雀检查恰好会读它。这是 s390x ABI 包的工具链级
  细节,不是源码级改动。
- **静态链接纪律。** 预构建交叉归档为静态(`libcorocc.a`);运行时线程支持
  来自平台(CMake 包中的 `Threads::Threads`)。

## 如何自行验证布局

上表仅凭公开头即可复现:用任一受支持编译器编译一个单文件检查并运行。在 64
位平台上,以下程序必须打印 `PASS`(同一组断言被编译进库自身在全部列出的平台
上的验证运行):

```c
#include <corocc/coro.h>
#include <corocc/coro_ctx.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_scope.h>
#include <corocc/coro_stack.h>
#include <corocc/coro_status.h>
#include <stddef.h>
#include <stdio.h>

#define SA(name, cond) typedef char sa_##name[(cond) ? 1 : -1]
SA(ctx_sz, sizeof(coro_ctx) == 48)
SA(ctx_fctx, offsetof(coro_ctx, fctx) == 0)
SA(ctx_asan, offsetof(coro_ctx, asan_fake) == 40)
SA(stack_sz, sizeof(coro_stack) == 40)
SA(attr_sz, sizeof(coro_attr_t) == 32)
SA(chan_sz, sizeof(coro_rt_channel_t) == 64)
SA(scope_sz, sizeof(coro_rt_scope_t) == 88)
SA(status_ok0, CORO_OK == 0)
SA(status_closed9, CORO_ECLOSED == 9)

int main(void) {
    printf("sizeof(coro_ctx)=%u sizeof(coro_stack)=%u sizeof(coro_attr_t)=%u\n",
           (unsigned)sizeof(coro_ctx), (unsigned)sizeof(coro_stack),
           (unsigned)sizeof(coro_attr_t));
    printf("PASS\n");
    return 0;
}
```

```sh
cc -std=c99 -Wall -Wextra -Werror -Iinclude -o abi_check abi_check.c
./abi_check
```

32 位目标改用与你的 ABI 变体对应的 ILP32 表(例如两种变体下
`sizeof(coro_ctx) == 24` 都成立;`sizeof(coro_rt_channel_t)` 在 `uint64_t`
8 字节对齐时为 40、4 字节对齐时为 36),程序其余不变。大端目标运行同一份
源码;偏移与尺寸不依赖字节序。
