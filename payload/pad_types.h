/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t finger;
    uint8_t reserved[3];
} ScePadTouch;

typedef struct {
    uint8_t fingers;
    uint8_t reserved0[3];
    uint32_t reserved1;
    ScePadTouch touch[2];
} ScePadTouchData;

typedef struct {
    uint32_t buttons;
    struct { uint8_t x; uint8_t y; } leftStick;
    struct { uint8_t x; uint8_t y; } rightStick;
    struct { uint8_t l2; uint8_t r2; } analogButtons;
    uint16_t reserved0;
    struct { float x, y, z, w; } quat;
    struct { float x, y, z; } velocity;
    struct { float x, y, z; } acceleration;
    ScePadTouchData touchData;
    uint8_t connected;
    uint8_t reserved1[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t count;
    uint8_t reserved2[15];
} ScePadData;
