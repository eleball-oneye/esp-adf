# t04 dev defaults 死配置：`CONFIG_LOG_MAXIMUM_LEVEL_INFO`

**状态**：✅ 完成（顺手修，同类缺陷）

## 问题

`examples/oneye/korvo2_oneye/sdkconfig.defaults` 有：

```
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL_INFO=y     ← 这一行什么都不做
```

IDF 里 `config LOG_MAXIMUM_LEVEL_INFO` 带 **`depends on LOG_DEFAULT_LEVEL < 3`**
（`components/log/Kconfig.level:67`），而上一行已经把缺省级别设成 INFO(=3) ⇒ `3 < 3` 为假
⇒ 该成员不可选，写进 defaults 会被**静默丢弃**，choice 落回 `LOG_MAXIMUM_EQUALS_DEFAULT`。

效果恰好等于作者想要的那件事（max = 缺省 = INFO），所以一直没人发现 —— 但它是
"**写了配置 ≠ 配置生效**"这一类缺陷（与量产预设那次 `..._MQTT_TLS` 是同一类）。

## 改法与理由

改成显式钉 **`CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT=y`**：

- 它**是**一个真实符号（会出现在生成的 sdkconfig 里 ⇒ 门禁看得见、将来能被复核）；
- 语义正是本工程要的"运行期可调到缺省级别"；
- 将来有人改缺省级别时，这一行不会被悄悄带跑（这正是"显式写出来"的意义）。

## 证据

修前：门禁报 `[FAIL] sdkconfig.defaults：1 个符号在本工程里不存在`。
修后：三份 defaults **全绿** ——

```
[ OK ] sdkconfig.defaults.production：11 项全部存在且已生效
[ OK ] sdkconfig.defaults：32 项全部存在
[ OK ] sdkconfig.defaults.esp32s3：14 项全部存在
PASS——defaults/预设里的每一项都存在且（在 --require 范围内）已生效
```

> 这一点值得记：门禁**修前是红的**。红的门禁等于没有门禁 —— 没人会去读一条永远存在的告警，
> 于是真回归（t03 的端口）也会被一起忽略。
