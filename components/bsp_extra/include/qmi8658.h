#ifndef QMI8658_H
#define QMI8658_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x;
    float y;
    float z;
} qmi8658_acc_t;

typedef struct {
    float x;
    float y;
    float z;
} qmi8658_gyro_t;

esp_err_t qmi8658_init(void);
esp_err_t qmi8658_read_acc(qmi8658_acc_t *acc);
esp_err_t qmi8658_read_gyro(qmi8658_gyro_t *gyro);

#ifdef __cplusplus
}
#endif

#endif // QMI8658_H
