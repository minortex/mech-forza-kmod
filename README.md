# mech-forza-kmod

`mechrevo-ec.c` 是只面向 **MECHREVO GX4HRXL** 的轻量 ACPI EC 驱动。它不包含
`uniwill-laptop` 中与本机无关的 dGPU、lightbar、电池扩展、多机型 descriptor 等代码。
本仓库从 [`mech-forza-control`](https://github.com/minortex/mech-forza-control) 独立出来，
只包含内核模块、UAPI 头文件和 Arch Linux DKMS 打包文件。Python CLI 与驱动可以分别发布、
安装和维护。


驱动通过固件设备 `INOU0000` 的 `ECRR`/`ECRW` 方法访问 12 位 EC XRAM，而不是映射
`/dev/mem`。Python 继续负责风扇表、TCC、设置项等业务语义；内核负责 ACPI 调用、串行化、
生命周期和最基本的输入检查。

## 安全边界

- 只在 DMI 同时匹配 `MECHREVO` 和 `GX4HRXL` 时 probe。
- 只绑定 ACPI ID `INOU0000`，并要求存在 `ECRR`、`ECRW`。
- `/dev/mechrevo-ec` 权限为 `0600`，同一时间只允许一个用户进程打开。
- ioctl 数据固定为 4 字节，只接受地址 `0x0000..0x0FFF` 和单字节值。
- 所有 EC 操作共用一个 mutex；`UPDATE_BITS` 在同一临界区内完成原子读改写。
- 每次 ACPI EC 访问后等待 6 ms，与 OEM/参考驱动的时序一致。
- 不在内核复制业务寄存器白名单。寄存器语义和值域仍由
  [`ec-register-map.md`](https://github.com/minortex/mech-forza-control/blob/master/docs/ec-register-map.md) 和 Python 控制层维护；EC 固件的 H2RAM ACL 是最终硬件边界。

这比允许任意进程映射物理内存安全得多，但 **root 对 EC 的写操作仍可能造成异常风扇、功耗或
充电行为**，不能把设备节点开放给不受信任用户。

## ApExistFlag 生命周期

驱动管理 `XRAM[0x0741].bit0`，Python 不需要依靠某条业务命令“顺便”设置它：

1. `probe`：设置 bit0，并立即回读验证；失败则拒绝加载。
2. `resume`：首先重新设置并回读验证，再恢复 suspend 前保存的模式和键盘背光寄存器。
3. `remove`：devm 清理动作清除 bit0，并回读验证。
4. `shutdown`：主动清除 bit0，并回读验证。

这解决了该 volatile 位在睡眠/固件重置后丢失，而 regcache 无法自动恢复的问题。

## 内核原生接口

加载成功后提供：

- `/dev/mechrevo-ec`：Python 使用的 READ、WRITE、UPDATE_BITS ioctl。
- `platform_profile`：`low-power` / `balanced` / `performance`，分别对应 Office (`0xA0`)、
  Gaming (`0x00`) 和 Turbo (`0x10`)；切换时保留 `0x0751.bit6` FanBoost，不改风扇表、TCC
  或其他设置。
- LED class：`mechrevo::kbd_backlight`。
- hwmon：CPU 温度、两个风扇 RPM、两个风扇只读 PWM。
- WMI/input：性能档位按键、键盘背光按键和硬件亮度变化通知。

副风扇 RPM 使用 GX4HRXL 的 `high=0x046C`、`low=0x046B`。

## 构建

需要当前运行内核对应的 headers：

```bash
make
```

Makefile 会在内核由 Clang 构建时自动使用 `LLVM=1`。产物为：

```text
mechrevo-ec.ko
```

清理构建产物：

```bash
make clean
```

### Arch Linux / DKMS 软件包

在仓库目录构建并安装：

```bash
makepkg -si
```

`PKGBUILD` 会把最小驱动源码安装到
`/usr/src/mechrevo-ec-<pkgver>/`，并依赖 Arch 的标准 DKMS pacman hook。安装或升级软件包后，
DKMS 会自动为所有已安装且具备对应 headers 的内核构建并安装模块；后续内核升级也会自动重建。
请先安装当前内核对应的 headers（例如官方 `linux` 内核对应 `linux-headers`，其他内核使用各自的
headers 包）。

DKMS 自动构建和安装模块，**不会自动加载模块或抢占 `INOU0000`**。首次加载以及与
`uniwill_laptop` 的冲突处理仍应由用户手动完成。

## 加载前的冲突检查

`INOU0000` 同一时间只能由一个 platform driver 绑定。若 `uniwill_laptop` 已经绑定，新驱动不会
同时接管它。可以先只检查，不修改系统状态：

```bash
readlink /sys/bus/acpi/devices/INOU0000:*/driver
lsmod | grep -E 'uniwill|mechrevo'
```

卸载旧模块和加载新模块会直接影响真实硬件，应由用户在确认后手动执行。本项目的自动测试和
构建流程不会运行 `rmmod`、`modprobe` 或 `insmod`。

## Python 后端选择

[mech-forza-control](https://github.com/minortex/mech-forza-control) 在 Linux 上默认只使用该内核桥接：

```bash
sudo uv run mfc mode status
```

如果 `/dev/mechrevo-ec` 不存在或权限不足，程序会明确报错，**不会静默退回 `/dev/mem`**。
兼容后端必须显式选择：

```bash
sudo MFC_EC_BACKEND=acpi-call uv run mfc mode status
sudo MFC_EC_BACKEND=devmem uv run mfc mode status   # 不安全，仅兼容/调试
sudo MFC_EC_BACKEND=auto uv run mfc mode status     # kernel -> acpi-call -> devmem
```

建议正常使用始终保持默认的 `kernel` 后端。
