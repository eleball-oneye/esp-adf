#include "motion.h"

#include <math.h>
#include <string.h>

#define STILL_GYRO_DPS     6.0f  /* wide enough to re-lock after a bad boot bias */
#define STILL_ACC_MIN_G    0.90f
#define STILL_ACC_MAX_G    1.10f
#define STILL_SAMPLES      40
#define BIAS_TRACK_ALPHA   0.02f
#define PLANE_LP_ALPHA     0.05f

static void normalize3(float v[3])
{
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n < 1e-6f) {
        v[0] = 0;
        v[1] = 0;
        v[2] = 1;
        return;
    }
    v[0] /= n;
    v[1] /= n;
    v[2] /= n;
}

static void cross3(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Rebuild plane axes from the current gravity estimate. */
static void rebuild_axes(motion_t *m)
{
    float x[3] = {1.0f, 0.0f, 0.0f};
    float proj = dot3(x, m->n);
    for (int i = 0; i < 3; i++) {
        m->fwd[i] = x[i] - proj * m->n[i];
    }
    if (sqrtf(dot3(m->fwd, m->fwd)) < 0.1f) {
        /* X is nearly parallel to gravity: fall back to Y. */
        float y[3] = {0.0f, 1.0f, 0.0f};
        proj = dot3(y, m->n);
        for (int i = 0; i < 3; i++) {
            m->fwd[i] = y[i] - proj * m->n[i];
        }
    }
    normalize3(m->fwd);
    cross3(m->n, m->fwd, m->right);
    normalize3(m->right);
}

void motion_init(motion_t *m, const qmi8658_sample_t *first, int sample_count)
{
    memset(m, 0, sizeof(*m));
    if (!first || sample_count <= 0) {
        m->n[2] = 1.0f;
        m->fwd[0] = 1.0f;
        m->right[1] = 1.0f;
        m->ready = true;
        return;
    }

    /* Average the initial samples: gyro mean = bias, accel mean = gravity. */
    float gx = 0, gy = 0, gz = 0, ax = 0, ay = 0, az = 0;
    for (int i = 0; i < sample_count; i++) {
        gx += first[i].gx; gy += first[i].gy; gz += first[i].gz;
        ax += first[i].ax; ay += first[i].ay; az += first[i].az;
    }
    float inv = 1.0f / (float)sample_count;
    m->bias[0] = gx * inv;
    m->bias[1] = gy * inv;
    m->bias[2] = gz * inv;
    m->acc_lp[0] = ax * inv;
    m->acc_lp[1] = ay * inv;
    m->acc_lp[2] = az * inv;

    /* At rest the accelerometer measures specific force opposite to gravity. */
    m->n[0] = -m->acc_lp[0];
    m->n[1] = -m->acc_lp[1];
    m->n[2] = -m->acc_lp[2];
    normalize3(m->n);
    rebuild_axes(m);
    m->ready = true;
}

void motion_update(motion_t *m, const qmi8658_sample_t *s, float dt)
{
    (void)dt;
    if (!m->ready) {
        return;
    }

    /* Slow gravity tracking (both still and moving; small alpha rejects hand motion). */
    m->acc_lp[0] += PLANE_LP_ALPHA * (s->ax - m->acc_lp[0]);
    m->acc_lp[1] += PLANE_LP_ALPHA * (s->ay - m->acc_lp[1]);
    m->acc_lp[2] += PLANE_LP_ALPHA * (s->az - m->acc_lp[2]);

    float acc_norm = sqrtf(m->acc_lp[0] * m->acc_lp[0] +
                           m->acc_lp[1] * m->acc_lp[1] +
                           m->acc_lp[2] * m->acc_lp[2]);
    if (acc_norm > 0.5f) {
        float n[3] = {-m->acc_lp[0], -m->acc_lp[1], -m->acc_lp[2]};
        normalize3(n);
        /* Only re-orient when the estimate is physically plausible. */
        m->n[0] = n[0];
        m->n[1] = n[1];
        m->n[2] = n[2];
        rebuild_axes(m);
    }

    /* Still detection -> slow gyro bias tracking. */
    float gx = s->gx - m->bias[0];
    float gy = s->gy - m->bias[1];
    float gz = s->gz - m->bias[2];
    float gyro_mag = sqrtf(gx * gx + gy * gy + gz * gz);

    if (acc_norm > STILL_ACC_MIN_G && acc_norm < STILL_ACC_MAX_G && gyro_mag < STILL_GYRO_DPS) {
        if (m->still_count < STILL_SAMPLES) {
            m->still_count++;
        } else {
            m->bias[0] += BIAS_TRACK_ALPHA * (s->gx - m->bias[0]);
            m->bias[1] += BIAS_TRACK_ALPHA * (s->gy - m->bias[1]);
            m->bias[2] += BIAS_TRACK_ALPHA * (s->gz - m->bias[2]);
        }
    } else {
        m->still_count = 0;
    }
}

void motion_rates(const motion_t *m, const qmi8658_sample_t *s,
                  float *roll_rate, float *pitch_rate, float *yaw_rate)
{
    float g[3] = {
        s->gx - m->bias[0],
        s->gy - m->bias[1],
        s->gz - m->bias[2],
    };
    *roll_rate = dot3(g, m->fwd);
    *pitch_rate = dot3(g, m->right);
    *yaw_rate = dot3(g, m->n);

    /* Deadband: reject residual bias / hand tremor so the cursor does not creep. */
    const float dead = 0.8f;
    if (fabsf(*roll_rate) < dead) {
        *roll_rate = 0.0f;
    }
    if (fabsf(*pitch_rate) < dead) {
        *pitch_rate = 0.0f;
    }
    if (fabsf(*yaw_rate) < dead) {
        *yaw_rate = 0.0f;
    }
}

float motion_bias_mag(const motion_t *m)
{
    return sqrtf(m->bias[0] * m->bias[0] + m->bias[1] * m->bias[1] + m->bias[2] * m->bias[2]);
}
