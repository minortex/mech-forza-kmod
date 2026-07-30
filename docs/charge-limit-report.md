# 充电限制内核模块完善调研报告

> 当前实现状态（2026-07-30）：本报告最初用于规划从 upper-only 到 charge-window 的升级。
> 代码现已在 BAT0 暴露 `charge_control_start_threshold` 和
> `charge_control_end_threshold`，写入时按 `0x07D0` 后 `0x07B9` 的顺序初始化 phase，
> 并在 `/dev/mechrevo-ec` raw ioctl 触及 `0x07D0`/`0x07B9` 后发送 BAT0 变更通知。
> v2.2 固件中的 lower 持久化由 EC 自己通过 `0x0773` 与 `0x077E/0x077F` 维护；内核只提供
> 标准 start/end 阈值接口，不直接写 shadow 或提交握手。

## 1. 调研范围

本报告基于以下两处控制层实现做对照分析：

- `/home/texsd/codes/mech-forza-control/docs/cli-reference.md`
- `/home/texsd/codes/mech-forza-control/src/battery.py`

同时结合当前 `mech-forza-kmod` 仓库中的 `mechrevo-ec.c` 现状，以及本机已加载模块后的 sysfs 暴露结果，评估内核模块应如何完善“充电限制”能力。

---

## 2. 结论摘要

当前内核模块的充电限制能力**只实现了“上限阈值”**，即只把 `XRAM[0x07B9]` 映射为标准 `power_supply` 的 `charge_control_end_threshold`。

而控制层 (`battery.py` + CLI 文档) 已经明确支持一套更完整的 **上下限窗口 / hysteresis(FlexiCharge)** 语义：

- 上限寄存器：`XRAM[0x07B9]`
- 下限寄存器：`XRAM[0x07D0]`
- 两个寄存器的 bit7 不是简单“保留位”，而是被控制层当作**充电相位状态位**使用：
  - `0x07B9.bit7`：stop / inhibit charge
  - `0x07D0.bit7`：cycle-active
- 设置窗口时要求：
  - 先写下限，再写上限
  - 根据当前 RSOC 初始化 phase
  - 同一窗口重复设置时需要尽量保留已有 active phase

因此，**内核模块的主要缺口不是“不会写 0x07B9”，而是没有把 0x07D0 和 phase 初始化逻辑一起内核化**。如果只保留现在的 end-threshold 单点接口，内核 ABI 会长期落后于控制层能力。

**建议的一期目标**：

1. 在 `power_supply` 扩展里新增 `POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD`
2. 内核内部新增“窗口语义”读写辅助函数，同时管理 `0x07B9` / `0x07D0`
3. 按 `battery.py` 的规则初始化/保留 bit7 phase
4. 继续保留 `charge_control_end_threshold` 兼容现有用户空间
5. 在文档中明确说明：下限窗口需要兼容 EC 固件；原厂固件上寄存器写入可能成功，但行为未必生效

**建议的二期目标**：

6. 对 `/dev/mechrevo-ec` 直接写相关寄存器时补 battery notify，使 BAT0 sysfs 观察者能收到变更通知
7. 视测试结果决定是否补充 stock 3 档模式 (`0x07A6` bits 5:4) 的内核接口
8. 若实测 suspend/resume 后窗口寄存器会丢失，再补阈值缓存与恢复

---

## 3. 当前内核模块实现现状

### 3.1 只实现了上限阈值

`mechrevo-ec.c` 当前充电限制实现集中在：

- `mechrevo_charge_limit_get()`
- `mechrevo_charge_limit_set()`
- `POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD`

现状特征：

1. 只定义了一个寄存器地址：`EC_ADDR_BATTERY_CHARGE_LIMIT = 0x07b9`
2. 只暴露一个标准属性：`CHARGE_CONTROL_END_THRESHOLD`
3. 读的时候把 `0x07B9[6:0]` 解码为 1-100，其中 raw=0 映射为 100
4. 写的时候只更新 `0x07B9[6:0]`，并保留 bit7 的当前值
5. 完全不处理 `0x07D0`

也就是说，当前内核模块把“充电限制”理解成了**单一 upper limit**，而不是**带迟滞的 charge window**。

### 3.2 当前 power_supply ABI 是 upper-only

本机实际 sysfs 观察结果：

- 存在：`/sys/class/power_supply/BAT0/charge_control_end_threshold`
- 不存在：`/sys/class/power_supply/BAT0/charge_control_start_threshold`

这和源码完全一致：当前 BAT0 只暴露上限，没有下限窗口。

### 3.3 当前实现对 bit7 的处理过于保守

当前内核代码对 `0x07B9` 的写入使用 `update_bits(mask=0x7f)`，等价于：

- 只写 low 7 bits
- bit7 保留原值

这在“只支持 upper-only”时勉强成立，但一旦要支持控制层文档中的 FlexiCharge 语义，就不够了，因为 bit7 需要被**主动初始化**，而不是一律保留。

---

## 4. 控制层已经定义出的真实语义

以下内容直接来自 `cli-reference.md` 与 `battery.py`。

### 4.1 两个寄存器一起定义充电窗口

CLI 文档明确写了：

- `0x07B9[6:0]`：上限百分比
- `0x07B9.bit7`：停止/抑制充电标志
- `0x07D0[6:0]`：下限百分比
- `0x07D0.bit7`：当前是否处于充电周期中的 `cycle-active` 标志

这和当前内核的 upper-only 模型明显不同。

### 4.2 写入顺序有要求：先 low，后 high

CLI 文档注明：设置窗口时应当

1. 先写 `0x07D0`
2. 再写 `0x07B9`

`battery.py` 里的 `_write_limits()` 也正是这样实现的。说明这不是文档随手写写，而是控制层已经把它当作真实硬件语义。

### 4.3 phase 初始化不是“保留位”，而是有规则的状态机

`battery.py` 中 `_choose_cycle_active()` 与 `cmd_set()` 的行为可以总结为：

- 如果 `RSOC <= down`：初始化为 active cycle
- 如果 `RSOC >= up`：初始化为 stopped / inhibited
- 如果 `down < RSOC < up`：
  - 若是重新设置同一窗口且旧状态已经 active，则保留 active
  - 否则默认进入 hold（不充，直到掉到 down）

最终编码方式为：

- active cycle：
  - `low_raw = down | 0x80`
  - `high_raw = up`
- hold / inhibited：
  - `low_raw = down`
  - `high_raw = up | 0x80`

这说明 bit7 是**状态位**，不是单纯的“保留位”。

### 4.4 上限模式与窗口模式是两套不同语义

控制层支持三种模式：

1. `--disable`
   - `0x07D0 = 0`
   - `0x07B9 = 0`
   - 恢复 unrestricted / stock upper setting

2. 仅 `-u <up>`（上限模式）
   - `0x07D0 = 0`
   - `0x07B9 = up | stop_bit`
   - `up=0` 等价于 unrestricted / 100%

3. `-d <down> -u <up>`（窗口模式）
   - 同时配置两个寄存器
   - 使用 hysteresis phase 逻辑

当前内核只覆盖了第 2 种的一部分。

---

## 5. 当前实现与控制层之间的差距

### 5.1 缺少 start threshold 标准接口

Linux `power_supply.h` 在当前内核头里已经定义了：

- `POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD`
- `POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD`
- `POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR`

但当前模块只实现了 end threshold，没有 start threshold。

这是最直接、最应该补上的缺口。

### 5.2 缺少 window-aware 的 set/get 逻辑

当前 `mechrevo_charge_limit_set()` 的抽象层次太低，它只会：

- 校验 `1..100`
- raw 0 表示 100%
- 改 `0x07B9[6:0]`

它完全不知道：

- 当前是否启用了下限窗口
- 该不该写 `0x07D0`
- 当前 RSOC 是多少
- bit7 应该初始化成 active 还是 inhibit
- 同一窗口重设时是否要保留 phase

所以它不适合作为未来窗口模式的核心 helper，必须重构。

### 5.3 缺少对固件兼容性的分层表达

CLI 文档和 `battery.py` 明确区分了两件事：

- **upper limit 支持**：可能需要额外 enable（w568 脚本 / 特定 BIOS 选项）
- **lower hysteresis 支持**：需要兼容 EC firmware；stock firmware 可能“写入成功但行为无变化”

当前内核模块没有把这种差异体现在接口设计或日志里。结果会是：

- 用户能写 start/end
- 但在某些固件上只看到寄存器变了，充电行为不变
- 内核 ABI 看起来“成功支持了窗口”，实际行为却未必成立

这不是不能做，而是必须在文档/日志/错误模型里讲清楚。

### 5.4 缺少 battery notify 机制

如果通过 `power_supply` 自身写 `charge_control_end_threshold`，用户再次读取时能看到新值；
但如果控制层继续通过 `/dev/mechrevo-ec` 直接写 `0x07B9/0x07D0`，当前内核并不会主动发 BAT0 变更通知。

结果是：

- `cat` sysfs 会读到新值，因为它是实时读硬件
- 但依赖 `power_supply_changed()`/uevent 的监听者不一定能及时获知变化

这会造成“控制层已经改了，桌面电源管理器却没动静”的体验差。

---

## 6. 推荐实现方案

## 6.1 一期：把 upper-only 升级为标准 start/end window

这是最值得做的一期。

### 目标

让 BAT0 同时暴露：

- `charge_control_start_threshold`
- `charge_control_end_threshold`

并让它们真正对应：

- start -> `0x07D0`
- end -> `0x07B9`

### 设计原则

1. **保持现有 ABI 兼容**
   - `charge_control_end_threshold` 继续存在
   - raw `0x07B9[6:0] == 0` 仍对外表现为 `100`

2. **新增标准 start threshold**
   - `0x07D0[6:0] == 0` 对外表现为 `0`，表示下限窗口未启用

3. **写入时统一走 window helper**
   - 不能再让 end setter 只改一个寄存器
   - start/end 任一 setter 都应该：
     - 读取当前另一侧值
     - 读取当前 RSOC
     - 在一个 `io_lock` 临界区里重算 low/high raw
     - 先写 low，再写 high

4. **phase 逻辑跟控制层保持一致**
   - 直接复刻 `battery.py` 里的 `_choose_cycle_active()` 规则
   - 这样 Python CLI 和内核 sysfs 不会出现两套不同语义

### 推荐内部抽象

建议新增一个内部结构，例如：

```c
struct mechrevo_charge_window_state {
    u8 rsoc;
    u8 low_raw;
    u8 high_raw;
    u8 low;
    u8 high;
    bool cycle_active;
    bool stopped;
};
```

建议新增 helper：

- `mechrevo_charge_window_read_unlocked()`
- `mechrevo_charge_window_validate()`
- `mechrevo_charge_window_choose_phase()`
- `mechrevo_charge_window_write_unlocked()`

之后：

- `END_THRESHOLD` setter 只负责把“用户想要的新 high”交给 window helper
- `START_THRESHOLD` setter 只负责把“用户想要的新 low”交给 window helper

### 推荐的对外语义

#### 读

- `end_threshold`
  - `0x07B9[6:0] == 0` -> `100`
  - 否则返回 `1..100`

- `start_threshold`
  - `0x07D0[6:0] == 0` -> `0`
  - 否则返回 `1..95`

#### 写

- 写 `end=100`
  - 等价于 high raw 置 0
  - 如果 `start=0`，表示 unrestricted upper-only
  - 如果 `start>0`，**建议拒绝**，与 `battery.py` 保持一致（窗口模式下 up 不允许 100）

- 写 `start=0`
  - 禁用 lower hysteresis
  - 保留/重算 upper-only stop bit

- 写 `start>0`
  - 需要当前 `end` 在 `2..99`
  - 且 `start < end`

- 写 `end<current_start`
  - 拒绝 `-EINVAL`

这样虽然 sysfs 的 start/end 是两次独立写，但每次都能得到一致状态，不会留下“只改了一半寄存器”的问题。

---

## 6.2 一期实现时需要特别注意的细节

### A. bit7 不应继续被视为“永远保留”

如果继续沿用当前 `update_bits(mask=0x7f)` 的做法：

- upper-only 模式还能凑合
- window 模式一定不完整

因为 bit7 必须按 phase 明确写入。

### B. 需要在同一把 mutex 里完成 read-modify-write

控制层已经通过事务和固定写序保证一致性；内核层更应该这样做。

推荐做法：

- 在 `io_lock` 内读取：RSOC / old low / old high
- 算出 new low / new high
- 先写 low，再写 high
- 立即回读验证

### C. 建议在成功 set 后调用 `power_supply_changed(battery)`

这样：

- sysfs watcher
- 桌面电源管理器
- 监听 BAT0 变化的用户态

都能更快感知到阈值变化。

### D. 错误语义要保守

推荐：

- 参数非法：`-EINVAL`
- 回读不一致：`-EIO`
- 硬件/固件不支持：优先 `-EOPNOTSUPP`（若能检测）

不要因为寄存器“写成功”就假定行为一定生效。

---

## 6.3 二期：补充 stock 3 档模式支持

虽然本次重点是 `cli-reference.md` 与 `battery.py` 中的 upper/lower threshold，但同仓库逆向文档还揭示了另一条旧路径：

- `XRAM[0x07A6]` bits [5:4]
  - `00` -> High / 100%
  - `01` -> Middle / ~80%
  - `10` -> Low / ~60%

这条路径更像厂商原生“三档电池保护模式”。

### 为什么它值得考虑

因为从控制层文档看：

- custom upper threshold 可能需要先 enable
- lower hysteresis 则明确依赖兼容 EC firmware

所以在 stock firmware 上，真正稳定可用的可能反而是 100/80/60 三档，而不是任意 window。

### 但为什么不建议放到一期

因为它和标准 `charge_control_start/end_threshold` 不是天然一一对应：

- 3 档模式是一种“profile”
- start/end 是一组百分比阈值

如果一期同时塞进来，会把 ABI 设计弄复杂。

### 更合理的做法

二期单独增加一个**自定义属性**，例如：

- platform device attribute：`charge_limit_profile`
- 取值：`high` / `balanced` / `health`

并明确它和自定义窗口是两条不同路径。

---

## 6.4 二期：让 `/dev/mechrevo-ec` 改寄存器后也触发 BAT0 通知

这是体验层面的完善。

当前控制层很可能继续通过 `/dev/mechrevo-ec` 做原子 transaction。若内核模块只在 `power_supply set_property()` 路径里更新，而不管 misc ioctl 直写，那么 BAT0 通知链还是不完整。

建议思路：

1. 在 battery hook add/remove 时缓存实际 `struct power_supply *battery`
2. 在 misc ioctl 的 WRITE / UPDATE_BITS / XFER 完成后检查是否触及：
   - `0x07B9`
   - `0x07D0`
   - （可选）`0x07A6`
3. 若触及，则调用 `power_supply_changed(cached_battery)`

这样 CLI 和 BAT0 sysfs 观察者之间的状态同步会更自然。

---

## 6.5 三期（按需）：resume 后恢复 charge window

当前驱动 resume 只恢复：

- mode
- keyboard backlight

不恢复电池阈值。

是否需要补，取决于实测：

- 如果 suspend/resume 后 `0x07B9/0x07D0` 保持不变，只需重设 `ApExistFlag`，那就不用加
- 如果 EC 在 resume 后会把 window / phase 清掉，那就应像 mode/backlight 一样缓存并恢复

这个点目前**没有足够证据证明必须做**，因此建议列为“验证后决定”，不要先入为主地加复杂度。

---

## 7. 建议的代码改造点

### 7.1 常量层

当前：

- `EC_ADDR_BATTERY_CHARGE_LIMIT = 0x07b9`

建议改为更明确的两寄存器命名：

- `EC_ADDR_BATTERY_CHARGE_LIMIT_UP = 0x07b9`
- `EC_ADDR_BATTERY_CHARGE_LIMIT_DOWN = 0x07d0`
- `BATTERY_CHARGE_LIMIT_MASK = GENMASK(6, 0)`
- `BATTERY_CHARGE_PHASE_FLAG = BIT(7)`

### 7.2 battery property 列表

当前只有：

- `POWER_SUPPLY_PROP_TEMP`
- `POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD`

建议至少增加：

- `POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD`

### 7.3 get_property / set_property

当前 `switch` 只有 upper-only。

建议改成：

- `TEMP` -> 保持
- `START_THRESHOLD` -> 读/写 `0x07D0`
- `END_THRESHOLD` -> 读/写 `0x07B9`

但底层不要各写各的，必须共用 window helper。

### 7.4 回读验证

建议窗口写入后同时回读两寄存器，而不是只验证 `0x07B9`。

### 7.5 文档

README 至少补一句：

- BAT0 标准属性支持 `charge_control_end_threshold`
- 若固件兼容，还支持 `charge_control_start_threshold`
- lower threshold 仅表示寄存器窗口配置；是否真正改变充电行为依赖 EC firmware

---

## 8. 风险与注意事项

### 8.1 文档冲突：bit7 到底是“保留位”还是“状态位”

同一控制仓库里其实存在两种描述：

- `ec-register-map.md` 把 `0x07B9.bit7` / `0x07D0.bit7` 写成“保留”
- `cli-reference.md` 与 `battery.py` 则把它们明确当作 stop/cycle-active phase

从“哪个更应作为内核实现依据”来看，建议**优先采信 `battery.py` + CLI 文档**，原因是：

1. 它们已经形成可执行行为
2. 有配套测试覆盖
3. 明确描述了 phase 初始化与同窗保留逻辑

但在正式合入前，仍应做一次硬件复核，确认 bit7 的控制层解释没有偏差。

### 8.2 sysfs 双文件写入存在“顺序依赖”

`charge_control_start_threshold` 和 `charge_control_end_threshold` 是两个独立文件，因此修改窗口时天然会有顺序问题。

解决方式不是再加第三个 sysfs 文件，而是：

- 每次写任一侧时，都读取另一侧当前值
- 在内核里生成完整的新窗口
- 统一执行一次 low/high 原子更新

这样就能把问题压到最小。

### 8.3 stock firmware 上可能“寄存器改了，但行为没变”

这不是内核 bug，而是硬件/固件能力边界。

所以内核实现要做到两点：

- 接口语义清楚
- 文档说明充分

而不是试图在内核里“伪造成功/失败语义”去掩盖固件差异。

---

## 9. 推荐落地顺序

### 第一阶段（建议优先）

- [x] 重构 upper-only helper 为 window helper
- [x] 增加 `CHARGE_CONTROL_START_THRESHOLD`
- [x] 按控制层实现 phase 初始化和 low->high 写序
- [x] set 后补 `power_supply_changed()`
- [x] README 补 lower-threshold firmware caveat

### 第二阶段

- [x] 为 misc ioctl 触及电池寄存器时补 BAT0 notify
- [ ] 评估是否增加 `charge_limit_profile` 自定义属性映射 `0x07A6`

### 第三阶段（视实测）

- [ ] 验证 resume 后阈值是否丢失
- [ ] 如有必要，补 charge window suspend/resume 恢复

---

## 10. 最终判断

如果目标是“把当前内核模块的充电限制做完整”，那么最合理的方向不是继续强化 `0x07B9` 的 upper-only 读写，而是把内核的电池接口升级到和控制层一致的**window-aware** 模型：

- `0x07B9` = end threshold + stop phase
- `0x07D0` = start threshold + cycle-active phase
- 写入逻辑按 `battery.py` 统一内核化
- 对外优先走 Linux 标准 `power_supply` start/end threshold ABI

一句话概括：

> 当前内核模块已经有了“上限限充”的雏形，但要真正对齐控制层能力，下一步应该补的是“标准 start threshold + hysteresis 状态机”，而不是继续停留在单寄存器 upper-only 模式。
