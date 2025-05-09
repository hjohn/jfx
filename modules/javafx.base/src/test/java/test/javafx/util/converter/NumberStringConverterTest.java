/*
 * Copyright (c) 2010, 2022, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package test.javafx.util.converter;

import java.text.DecimalFormat;
import java.text.NumberFormat;
import java.util.Locale;
import javafx.util.converter.NumberStringConverter;
import javafx.util.converter.NumberStringConverterShim;
import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

public class NumberStringConverterTest {
    private NumberStringConverter converter;

    @BeforeEach
    public void setup() {
        converter = new NumberStringConverter();
    }

    @Test
    public void testDefaultConstructor() {
        NumberStringConverter c = new NumberStringConverter();
        assertEquals(Locale.getDefault(), NumberStringConverterShim.getLocale(c));
        assertNull(NumberStringConverterShim.getPattern(c));
        assertNull(NumberStringConverterShim.getNumberFormatVar(c));
    }

    @Test
    public void testConstructor_locale() {
        NumberStringConverter c = new NumberStringConverter(Locale.CANADA);
        assertEquals(Locale.CANADA, NumberStringConverterShim.getLocale(c));
        assertNull(NumberStringConverterShim.getPattern(c));
        assertNull(NumberStringConverterShim.getNumberFormatVar(c));
    }

    @Test
    public void testConstructor_pattern() {
        NumberStringConverter c = new NumberStringConverter("#,##,###,####");
        assertEquals(Locale.getDefault(), NumberStringConverterShim.getLocale(c));
        assertEquals("#,##,###,####", NumberStringConverterShim.getPattern(c));
        assertNull(NumberStringConverterShim.getNumberFormatVar(c));
    }

    @Test
    public void testConstructor_locale_pattern() {
        NumberStringConverter c = new NumberStringConverter(Locale.CANADA, "#,##,###,####");
        assertEquals(Locale.CANADA, NumberStringConverterShim.getLocale(c));
        assertEquals("#,##,###,####", NumberStringConverterShim.getPattern(c));
        assertNull(NumberStringConverterShim.getNumberFormatVar(c));
    }

    @Test
    public void testConstructor_numberFormat() {
        NumberFormat format = NumberFormat.getCurrencyInstance(Locale.JAPAN);
        NumberStringConverter c = new NumberStringConverter(format);
        assertNull(NumberStringConverterShim.getLocale(c));
        assertNull(NumberStringConverterShim.getPattern(c));
        assertEquals(format, NumberStringConverterShim.getNumberFormatVar(c));
    }

    @Test
    public void getNumberFormat_default() {
        assertNotNull(NumberStringConverterShim.getNumberFormat(converter));
    }

    @Test
    public void getNumberFormat_nonNullPattern() {
        converter = new NumberStringConverter("#,##,###,####");
        assertTrue(NumberStringConverterShim.getNumberFormat(converter) instanceof DecimalFormat);
    }

    @Test
    public void getNumberFormat_nonNullNumberFormat() {
        NumberFormat nf = NumberFormat.getCurrencyInstance();
        converter = new NumberStringConverter(nf);
        assertEquals(nf, NumberStringConverterShim.getNumberFormat(converter));
    }

    @Test
    public void fromString_testValidInput() {
        assertEquals(10L, converter.fromString("10"));
    }

    @Test
    public void fromString_testValidInputWithWhiteSpace() {
        assertEquals(10L, converter.fromString("      10      "));
    }

    @Test
    public void fromString_testInvalidInput() {
        assertThrows(RuntimeException.class, () -> converter.fromString("abcdefg"));
    }

    @Test
    public void toString_validInput() {
        assertEquals("10", converter.toString(10L));
    }
}
