/**
 * @file libredsea_utilities.c
 */

/*
 * Original Work Copyright (c) 2007-2016 Oona Räisänen OH2EIQ
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * -----------------------------------------------------------------------------
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * EBU Latin character mapping array adapted from 'librdsparser'
 * librdsparser – Radio Data System parser library
 * Copyright (C) 2023-2024  Konrad Kosmatka
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * -----------------------------------------------------------------------------
 *
 * Modifications Copyright (C) 2026 iq_tool
 *
 * The modifications to this file are licensed under the GNU General Public License v3.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "libredsea.h"
#include <string.h>

void libredsea_utility_translate_to_utf8(const char *raw_input, char *utf8_output, int max_output_length) {
    if (!raw_input || !utf8_output || max_output_length <= 0) return;

    static const char* ebu_charset[128] = {
        "á", "à", "é", "è", "í", "ì", "ó", "ò",
        "ú", "ù", "Ñ", "Ç", "Ş", "β", "¡", "Ĳ",
        "â", "ä", "ê", "ë", "î", "ï", "ô", "ö",
        "û", "ü", "ñ", "ç", "ş", "ǧ", "ı", "ĳ",
        "ª", "α", "©", "‰", "Ǧ", "ě", "ň", "ő",
        "π", "€", "£", "$", "←", "↑", "→", "↓",
        "º", "¹", "²", "³", "±", "İ", "ń", "ű",
        "µ", "¿", "÷", "°", "¼", "½", "¾", "§",
        "Á", "À", "É", "È", "Í", "Ì", "Ó", "Ò",
        "Ú", "Ù", "Ř", "Č", "Š", "Ž", "Ð", "Ŀ",
        "Â", "Ä", "Ê", "Ë", "Î", "Ï", "Ô", "Ö",
        "Û", "Ü", "ř", "č", "š", "ž", "đ", "ŀ",
        "Ã", "Å", "Æ", "Œ", "ŷ", "Ý", "Õ", "Ø",
        "Þ", "Ŋ", "Ŕ", "Ć", "Ś", "Ź", "Ŧ", "ð",
        "ã", "å", "æ", "œ", "ŵ", "ý", "õ", "ø",
        "þ", "ŋ", "ŕ", "ć", "ś", "ź", "ŧ", " "
    };

    int out_length = 0;
    for (int i = 0; raw_input[i] != '\0'; i++) {
        unsigned char c = (unsigned char)raw_input[i];
        const char *mapped = NULL;

        if (c >= 0x20 && c <= 0x7E) {
            // Standard ASCII maps 1:1
            if (out_length < max_output_length - 1) {
                utf8_output[out_length++] = c;
            }
            continue;
        } else if (c >= 0x80) {
            mapped = ebu_charset[c - 0x80];
        }

        if (mapped) {
            while (*mapped && out_length < max_output_length - 1) {
                utf8_output[out_length++] = *mapped++;
            }
        }
    }
    utf8_output[out_length] = '\0';
}
