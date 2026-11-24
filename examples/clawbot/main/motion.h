#pragma once

#include <stdbool.h>
#include "qmi8658.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Motion preprocessing for the desk-referenced cursor:
 *  - gyro bias calibration at boot (board must be still)
 *  - slow bias re-estimation while the device is still
 *  - gravity-defined horizontal plane (accelerometer), yaw excluded
 *
 * Body axes: X/Y in the plane when the board lies flat, Z along gravity.
 */

typedef struct {
    float bias[3];     /* dps, body frame */
    float n[3];        /* unit vector pointing away from gravity (up) */
    float fwd[3];      /* plane axis derived from body X */
    float right[3];    /* plane axis: right = n x fwd */
    float acc_lp[3];   /* low-passed specific force */
    int still_count;
    bool ready;
} motion_t;

void  motion_init(motion_t *m, const qmi8658_sample_t *first, int sample_count);
void  motion_update(motion_t *m, const qmi8658_sample_t *s, float dt);

/* Bias-corrected angular rates in the desk plane (dps). */
void  motion_rates(const motion_t *m, const qmi8658_sample_t *s,
                   float *roll_rate, float *pitch_rate, float *yaw_rate);

/* Bias magnitude (dps), for the debug page. */
float motion_bias_mag(const motion_t *m);

#ifdef __cplusplus
}
#endif
