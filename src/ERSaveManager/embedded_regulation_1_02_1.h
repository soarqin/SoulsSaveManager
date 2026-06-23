/**
 * @file embedded_regulation_1_02_1.h
 * @brief Header for the embedded 1.02.1 regulation.bin
 * @details Exposes the built-in regulation.bin used to downpatch Elden Ring
 *          save files to version 1.02.1 without reading any external file.
 */

#pragma once

#include <stddef.h>

extern const unsigned char embedded_regulation_1_02_1[];
extern const unsigned int embedded_regulation_1_02_1_size;