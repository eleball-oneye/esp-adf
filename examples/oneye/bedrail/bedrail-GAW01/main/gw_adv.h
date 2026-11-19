/*
 * bedrail-GAW01 —— 床护栏传感器广播接收（v5 TLV）
 *
 * 与 gw_child.c 的分工：
 *   - gw_child 负责「连接式」子设备（T31 那套：连上后读 TLV）与云事件出口；
 *   - 本模块负责「纯广播式」传感器（PB-03F 床护栏）：只在 BLE 扫描报告里解析 v5 TLV，
 *     然后把一帧 `sub.telemetry` 交给 gw_child 的事件出口上报。
 *
 * 身份口径：`node` = 传感器 **MAC**（小写六段）；`model` 由 MODEL_ID 映射；⛔ 不用 SN。
 */
#ifndef GW_ADV_H
#define GW_ADV_H

#ifdef __cplusplus
extern "C" {
#endif

/** 注册 v5 广播回调（幂等；在 oneye_link_start() 之后调用）。 */
void gw_adv_start(void);

#ifdef __cplusplus
}
#endif

#endif /* GW_ADV_H */
