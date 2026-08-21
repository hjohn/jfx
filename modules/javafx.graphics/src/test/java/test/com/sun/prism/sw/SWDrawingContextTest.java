/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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

package test.com.sun.prism.sw;

import com.sun.glass.ui.Screen;
import com.sun.javafx.geom.Rectangle;
import com.sun.javafx.tk.Toolkit;
import com.sun.prism.sw.SWDrawingContext;

import java.lang.reflect.Field;
import java.nio.ByteBuffer;
import java.nio.IntBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

import javafx.geometry.VPos;
import javafx.scene.effect.BlendMode;
import javafx.scene.image.Image;
import javafx.scene.paint.Color;
import javafx.scene.shape.ArcType;
import javafx.scene.shape.FillRule;
import javafx.scene.shape.StrokeLineCap;
import javafx.scene.shape.StrokeLineJoin;
import javafx.scene.text.Font;
import javafx.scene.text.FontSmoothingType;
import javafx.scene.text.TextAlignment;
import javafx.scene.transform.Affine;

import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import test.com.sun.javafx.pgstub.StubImageLoaderFactory;
import test.com.sun.javafx.pgstub.StubPlatformImageInfo;
import test.com.sun.javafx.pgstub.StubToolkit;

public class SWDrawingContextTest {

    private static final int WIDTH = 64;
    private static final int HEIGHT = 64;

    /**
     * Screens are normally installed by the native glass layer. Provide a fake
     * one so that SWDrawingContext can call Screen.getMainScreen().
     */
    private static final Field SCREENS_FIELD;

    static {
        try {
            SCREENS_FIELD = Screen.class.getDeclaredField("screens");

            SCREENS_FIELD.setAccessible(true);
        }
        catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }

    private final Harness h = createHarness(WIDTH, HEIGHT);

    @BeforeAll
    public static void installMainScreen() throws Exception {
        Screen screen = new Screen(
            0L, 24,
            0, 0, 1920, 1080,
            0, 0, 1920, 1080,
            0, 0, 1920, 1080,
            96, 96,
            1f, 1f, 1f, 1f
        );

        SCREENS_FIELD.set(null, Collections.singletonList(screen));
    }

    @AfterAll
    public static void uninstallMainScreen() throws Exception {
        SCREENS_FIELD.set(null, null);
    }

    @Test
    public void shouldConstructWithIntBufferImage() {
        assertTrue(h.image.isWritable());
    }

    @Test
    public void shouldThrowForNullImage() {
        assertThrows(NullPointerException.class, () -> new SWDrawingContext(null, _ -> {}));
    }

    @Test
    public void shouldThrowForNullDirtyConsumer() {
        com.sun.prism.Image image = prismImage(WIDTH, HEIGHT);

        assertThrows(NullPointerException.class, () -> new SWDrawingContext(image, null));
    }

    @Test
    public void shouldThrowForByteBufferImage() {
        com.sun.prism.Image image = com.sun.prism.Image.fromByteBgraPreData(ByteBuffer.allocate(WIDTH * HEIGHT * 4), WIDTH, HEIGHT);

        assertThrows(IllegalStateException.class, () -> new SWDrawingContext(image, _ -> {}));
    }

    @Test
    public void shouldThrowForNonArrayBackedIntBuffer() {
        IntBuffer direct = ByteBuffer.allocateDirect(WIDTH * HEIGHT * 4).asIntBuffer();
        com.sun.prism.Image image = com.sun.prism.Image.fromIntArgbPreData(direct, WIDTH, HEIGHT);

        assertThrows(UnsupportedOperationException.class, () -> new SWDrawingContext(image, _ -> {}));
    }

    @Test
    public void shouldHaveDefaultAttributeValues() {
        assertEquals(Color.BLACK, h.context.getStroke());
        assertEquals(Color.BLACK, h.context.getFill());
        assertEquals(1.0, h.context.getGlobalAlpha());
        assertEquals(BlendMode.SRC_OVER, h.context.getGlobalBlendMode());
        assertEquals(FillRule.NON_ZERO, h.context.getFillRule());
        assertEquals(1.0, h.context.getLineWidth());
        assertEquals(StrokeLineCap.SQUARE, h.context.getLineCap());
        assertEquals(StrokeLineJoin.MITER, h.context.getLineJoin());
        assertEquals(10.0, h.context.getMiterLimit());
        assertTrue(h.context.isImageSmoothing());
    }

    @Test
    public void shouldGetAndSetStroke() {
        h.context.setStroke(Color.RED);

        assertEquals(Color.RED, h.context.getStroke());

        h.context.setStroke(null);  // a null value is ignored and leaves the value unchanged

        assertEquals(Color.RED, h.context.getStroke());
    }

    @Test
    public void shouldGetAndSetFill() {
        h.context.setFill(Color.BLUE);

        assertEquals(Color.BLUE, h.context.getFill());

        h.context.setFill(null);  // a null value is ignored and leaves the value unchanged

        assertEquals(Color.BLUE, h.context.getFill());
    }

    @Test
    public void shouldClampGlobalAlpha() {
        h.context.setGlobalAlpha(2.0);

        assertEquals(1.0, h.context.getGlobalAlpha());

        h.context.setGlobalAlpha(-1.0);

        assertEquals(0.0, h.context.getGlobalAlpha());

        h.context.setGlobalAlpha(0.5);

        assertEquals(0.5, h.context.getGlobalAlpha());
    }

    @Test
    public void shouldIgnoreNullAndRejectUnsupportedBlendModes() {
        h.context.setGlobalBlendMode(null);  // a null value is ignored and leaves the value unchanged

        assertEquals(BlendMode.SRC_OVER, h.context.getGlobalBlendMode());

        // unsupported blend modes are rejected and leave the value unchanged
        assertThrows(UnsupportedOperationException.class, () -> h.context.setGlobalBlendMode(BlendMode.MULTIPLY));

        assertEquals(BlendMode.SRC_OVER, h.context.getGlobalBlendMode());
    }

    @Test
    public void shouldGetAndSetFillRule() {
        h.context.setFillRule(FillRule.EVEN_ODD);

        assertEquals(FillRule.EVEN_ODD, h.context.getFillRule());

        h.context.setFillRule(null);  // a null value is ignored and leaves the value unchanged

        assertEquals(FillRule.EVEN_ODD, h.context.getFillRule());
    }

    @Test
    public void shouldGetAndSetLineWidth() {
        h.context.setLineWidth(3.5);

        assertEquals(3.5, h.context.getLineWidth());

        // non-positive, infinite and NaN values are ignored
        h.context.setLineWidth(0.0);
        h.context.setLineWidth(-2.0);
        h.context.setLineWidth(Double.POSITIVE_INFINITY);
        h.context.setLineWidth(Double.NaN);

        assertEquals(3.5, h.context.getLineWidth());
    }

    @Test
    public void shouldGetAndSetLineCap() {
        h.context.setLineCap(StrokeLineCap.ROUND);

        assertEquals(StrokeLineCap.ROUND, h.context.getLineCap());

        h.context.setLineCap(null);  // a null value is ignored and leaves the value unchanged

        assertEquals(StrokeLineCap.ROUND, h.context.getLineCap());
    }

    @Test
    public void shouldGetAndSetLineJoin() {
        h.context.setLineJoin(StrokeLineJoin.ROUND);

        assertEquals(StrokeLineJoin.ROUND, h.context.getLineJoin());

        h.context.setLineJoin(null);  // a null value is ignored and leaves the value unchanged

        assertEquals(StrokeLineJoin.ROUND, h.context.getLineJoin());
    }

    @Test
    public void shouldGetAndSetMiterLimit() {
        h.context.setMiterLimit(5.0);

        assertEquals(5.0, h.context.getMiterLimit());

        // non-positive and infinite values are ignored.
        h.context.setMiterLimit(0.0);
        h.context.setMiterLimit(-1.0);
        h.context.setMiterLimit(Double.POSITIVE_INFINITY);

        assertEquals(5.0, h.context.getMiterLimit());
    }

    @Test
    public void shouldGetAndSetImageSmoothing() {
        h.context.setImageSmoothing(false);

        assertFalse(h.context.isImageSmoothing());

        h.context.setImageSmoothing(true);

        assertTrue(h.context.isImageSmoothing());
    }

    @Test
    public void shouldPaintPixelsForFillRect() {
        h.context.setFill(Color.RED);
        h.context.fillRect(10, 10, 40, 30);

        assertPixel(10, 10, Color.RED);
        assertPixel(30, 25, Color.RED);
        assertPixel(49, 39, Color.RED);

        // outside the rectangle is transparent
        assertPixel(9, 10, Color.TRANSPARENT);
        assertPixel(10, 9, Color.TRANSPARENT);
        assertPixel(50, 10, Color.TRANSPARENT);
        assertPixel(10, 40, Color.TRANSPARENT);
    }

    @Test
    public void shouldIgnoreZeroSizeFillRect() {
        h.context.setFill(Color.RED);
        h.context.fillRect(10, 10, 0, 10);
        h.context.fillRect(10, 10, 10, 0);

        assertTrue(h.dirtyRects.isEmpty());
        assertPixel(10, 10, Color.TRANSPARENT);
    }

    @Test
    public void shouldApplyGlobalAlphaToFillRect() {
        h.context.setFill(Color.RED);
        h.context.setGlobalAlpha(0.5);
        h.context.fillRect(10, 10, 40, 30);

        assertPixel(30, 25, argb(127, 255, 0, 0));
    }

    @Test
    public void shouldCompositeFillRectOverBackground() {
        h.context.setFill(Color.WHITE);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);

        h.context.setFill(new Color(1.0, 0.0, 0.0, 0.5));
        h.context.fillRect(10, 10, 40, 30);

        assertPixel(30, 25, argb(255, 255, 128, 128));
    }

    @Test
    public void shouldClearRegion() {
        h.context.setFill(Color.RED);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);
        h.context.clearRect(10, 20, 30, 40);

        assertPixel(30, 30, Color.TRANSPARENT);

        // the cleared rectangle itself must be transparent
        assertPixel(10, 20, Color.TRANSPARENT);
        assertPixel(39, 20, Color.TRANSPARENT);
        assertPixel(39, 49, Color.TRANSPARENT);
        assertPixel(10, 49, Color.TRANSPARENT);

        // outside the cleared rectangle, the fill must be intact
        assertPixel(5, 15, Color.RED);
        assertPixel(50, 50, Color.RED);
    }

    @Test
    public void shouldPaintStrokeRectBorder() {
        h.context.setStroke(Color.GREEN);
        h.context.setLineWidth(2);
        h.context.strokeRect(10, 10, 30, 20);

        // pixels on the border
        assertPixel(15, 10, Color.GREEN);
        assertPixel(39, 10, Color.GREEN);
        assertPixel(10, 15, Color.GREEN);
        assertPixel(10, 29, Color.GREEN);

        assertPixel(25, 20, Color.TRANSPARENT);  // interior is not painted by a stroke
        assertPixel(5, 5, Color.TRANSPARENT);  // outside the rectangle
    }

    @Test
    public void shouldPaintStrokeLine() {
        h.context.setStroke(Color.BLUE);
        h.context.setLineWidth(2);
        h.context.strokeLine(10, 10, 30, 10);

        assertPixel(20, 10, Color.BLUE);  // middle of the line
        assertPixel(20, 5, Color.TRANSPARENT);  // above the line
    }

    @Test
    public void shouldPaintFillOval() {
        h.context.setFill(Color.GREEN);
        h.context.fillOval(10, 10, 40, 40);

        // center of the oval (Color.GREEN is the CSS color #008000)
        assertPixel(30, 30, Color.GREEN);

        // outside the ellipse
        assertPixel(11, 11, Color.TRANSPARENT);
        assertPixel(50, 30, Color.TRANSPARENT);
    }

    @Test
    public void shouldPaintFillRoundRect() {
        h.context.setFill(Color.CYAN);
        h.context.fillRoundRect(10, 10, 40, 40, 10, 10);

        assertPixel(30, 30, Color.CYAN);  // center
        assertPixel(30, 12, Color.CYAN);  // mid-way along the top edge

        // corners not covered (because rounded)
        assertPixel(10, 10, Color.TRANSPARENT);
        assertPixel(10, 50, Color.TRANSPARENT);
        assertPixel(50, 10, Color.TRANSPARENT);
        assertPixel(50, 50, Color.TRANSPARENT);

        // 5 pixels from corner (towards center) pixels should be covered
        assertPixel(15, 15, Color.CYAN);
        assertPixel(15, 45, Color.CYAN);
        assertPixel(45, 15, Color.CYAN);
        assertPixel(45, 45, Color.CYAN);
    }

    @Test
    public void shouldPaintFillArc() {
        h.context.setFill(Color.MAGENTA);
        h.context.fillArc(10, 10, 40, 40, 0, 90, ArcType.ROUND);

        assertPixel(37, 23, Color.MAGENTA);  // inside the pie (north-east quadrant)
        assertPixel(23, 23, Color.TRANSPARENT);  // outside the pie (north-west quadrant)
    }

    @Test
    public void shouldPaintStrokeArc() {
        h.context.setStroke(Color.BLACK);
        h.context.setLineWidth(2);
        h.context.strokeArc(10, 10, 40, 40, 0, 90, ArcType.OPEN);

        assertPixel(44, 16, Color.BLACK);  // a point on the stroked arc
        assertPixel(16, 16, Color.TRANSPARENT);  // a point outside the arc extent
    }

    @Test
    public void shouldPaintFillPolygon() {
        h.context.setFill(Color.ORANGE);
        h.context.fillPolygon(
            new double[] {10, 54, 32},
            new double[] {10, 10, 54},
            3
        );

        assertPixel(32, 25, Color.ORANGE);  // inside the triangle
        assertPixel(10, 40, Color.TRANSPARENT);  // outside the triangle
    }

    @Test
    public void shouldFillEvenWoundRegionWithNonZero() {

        /*
         * A pentagram: the central pentagon is covered by two of the star's
         * arms (winding number 2), so it is filled under NON_ZERO.
         */

        h.context.setFill(Color.ORANGE);
        h.context.fillPolygon(
            new double[] {30, 44,  7, 53, 16},
            new double[] { 6, 50, 23, 23, 50},
            5
        );

        assertPixel(30, 30, Color.ORANGE);
        assertPixel(30, 15, Color.ORANGE);
    }

    @Test
    public void shouldLeaveEvenWoundRegionEmptyWithEvenOdd() {

        /*
         * A pentagram: the central pentagon has an even winding number
         * and must be empty under EVEN_ODD, while the star arms (winding 1)
         * are still painted.
         */

        h.context.setFillRule(FillRule.EVEN_ODD);
        h.context.setFill(Color.ORANGE);
        h.context.fillPolygon(
            new double[] {30, 44,  7, 53, 16},
            new double[] { 6, 50, 23, 23, 50},
            5
        );

        assertPixel(30, 30, Color.TRANSPARENT);
        assertPixel(30, 15, Color.ORANGE);
    }

    @Test
    public void shouldPaintStrokePolygon() {
        h.context.setStroke(Color.WHITE);
        h.context.setLineWidth(2);
        h.context.strokePolygon(
            new double[] {10, 54, 32},
            new double[] {10, 10, 54},
            3
        );

        // a point on the bottom edge of the triangle
        assertPixel(32, 53, Color.WHITE);

        // the interior is not painted by a stroke
        assertPixel(32, 25, Color.TRANSPARENT);
    }

    @Test
    public void shouldPaintStrokePolyline() {
        h.context.setStroke(Color.WHITE);
        h.context.setLineWidth(2);
        h.context.strokePolyline(
            new double[] {10, 30, 30},
            new double[] {10, 10, 30},
            3
        );

        // a point on the vertical segment
        assertPixel(30, 20, Color.WHITE);

        // a point on the horizontal segment
        assertPixel(20, 10, Color.WHITE);

        // not on the polyline
        assertPixel(10, 30, Color.TRANSPARENT);
    }

    @Test
    public void shouldPaintDrawImage() {
        Image source = createSolidFxImage(8, 8, argb(255, 0, 255, 0));

        h.context.drawImage(source, 0, 0, 8, 8, 10, 10, 8, 8);

        assertPixel(14, 14, Color.LIME);

        // outside the destination rectangle
        assertPixel(9, 9, Color.TRANSPARENT);
        assertPixel(18, 14, Color.TRANSPARENT);
    }

    @Test
    public void shouldScaleDrawImage() {
        Image source = createSolidFxImage(8, 8, argb(255, 0, 255, 0));

        h.context.drawImage(source, 0, 0, 8, 8, 10, 10, 40, 40);

        assertPixel(30, 30, Color.LIME);

        // outside the destination rectangle
        assertPixel(9, 9, Color.TRANSPARENT);
    }

    @Test
    public void shouldIgnoreNullImage() {
        h.context.setFill(Color.RED);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);

        int dirtyCount = h.dirtyRects.size();

        h.context.drawImage(null, 0, 0);

        assertEquals(dirtyCount, h.dirtyRects.size());
        assertPixel(10, 10, Color.RED);
    }

    @Test
    public void shouldIgnoreInProgressImage() {
        h.context.setFill(Color.RED);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);

        StubImageLoaderFactory factory = ((StubToolkit) Toolkit.getToolkit()).getImageLoaderFactory();

        factory.registerImage("file:slow.png", new StubPlatformImageInfo(8, 8));
        Image inProgress = new Image("file:slow.png", true);

        h.context.drawImage(inProgress, 5, 5);

        // the image has not finished loading, so nothing is drawn
        assertPixel(10, 10, Color.RED);
    }

    @Test
    public void shouldReportDirtyRegionForFillRect() {
        h.context.setFill(Color.RED);
        h.context.fillRect(10, 10, 50, 30);

        assertEquals(1, h.dirtyRects.size());
        assertEquals(new Rectangle(10, 10, 50, 30), h.dirtyRects.get(0));
    }

    @Test
    public void shouldClipDirtyRegionToImageBounds() {
        h.context.setFill(Color.RED);
        h.context.fillRect(60, 60, 10, 10);

        // only the part inside the image can be dirty
        assertEquals(1, h.dirtyRects.size());
        assertEquals(new Rectangle(60, 60, 4, 4), h.dirtyRects.get(0));
    }

    @Test
    public void shouldClipPixelsAndDirtyRegionForOffscreenFill() {
        h.context.setFill(Color.RED);
        h.context.fillRect(-5, -5, 10, 10);

        // the renderer clips to the image bounds
        assertPixel(0, 0, Color.RED);
        assertPixel(4, 4, Color.RED);
        assertPixel(5, 5, Color.TRANSPARENT);

        // the reported dirty region should also be clipped
        assertEquals(new Rectangle(0, 0, 5, 5), h.dirtyRects.get(0));
    }

    @Test
    public void shouldReportDirtyRegionContainingPaintedLine() {
        h.context.setStroke(Color.BLUE);
        h.context.strokeLine(10, 10, 30, 10);

        assertEquals(1, h.dirtyRects.size());
        assertTrue(h.dirtyRects.get(0).contains(paintedLineBounds(10, 10, 30, 10)), "dirty region must cover the painted line");
    }

    @Test
    public void shouldReportDirtyRegionForReversedLine() {
        h.context.setStroke(Color.BLUE);
        h.context.strokeLine(30, 10, 10, 10);

        assertEquals(1, h.dirtyRects.size());
        assertTrue(h.dirtyRects.get(0).contains(paintedLineBounds(10, 10, 30, 10)), "dirty region must cover the painted line regardless of point order");
    }

    @Test
    public void shouldGetAndSetTransform() {
        h.context.setTransform(2, 0, 0, 3, 4, 5);

        Affine a = h.context.getTransform();

        assertEquals(2.0, a.getMxx());
        assertEquals(0.0, a.getMyx());
        assertEquals(0.0, a.getMxy());
        assertEquals(3.0, a.getMyy());
        assertEquals(4.0, a.getTx());
        assertEquals(5.0, a.getTy());

        h.context.setTransform(new Affine(1, 0, 7, 0, 1, 8));

        assertEquals(7.0, h.context.getTransform().getTx());
        assertEquals(8.0, h.context.getTransform().getTy());

        h.context.setTransform((Affine) null);  // a null value is ignored and leaves the value unchanged

        assertEquals(7.0, h.context.getTransform().getTx());
        assertEquals(8.0, h.context.getTransform().getTy());
    }

    @Test
    public void shouldGetTransformFillingProvidedAffine() {
        h.context.setTransform(2, 0, 0, 3, 4, 5);

        Affine out = new Affine();
        Affine returned = h.context.getTransform(out);

        assertSame(out, returned);
        assertEquals(2.0, out.getMxx());
        assertEquals(0.0, out.getMyx());
        assertEquals(0.0, out.getMxy());
        assertEquals(3.0, out.getMyy());
        assertEquals(4.0, out.getTx());
        assertEquals(5.0, out.getTy());

        Affine fresh = h.context.getTransform((Affine) null);  // creates a new object

        assertNotSame(fresh, out);
        assertEquals(4.0, fresh.getTx());
        assertEquals(5.0, fresh.getTy());
    }

    @Test
    public void shouldTranslateDrawingWithTransform() {
        h.context.setFill(Color.RED);

        h.context.setTransform(1, 0, 0, 1, 10, 20);
        h.context.fillRect(0, 0, 5, 5);

        assertPixel(12, 22, Color.RED);  // inside the translated rectangle
        assertPixel(3, 3, Color.TRANSPARENT);  // outside the translated rectangle
    }

    @Test
    public void shouldRotateDrawingWithTransform() {
        h.context.setFill(Color.RED);

        /*
         * 45 degree rotation combined with a translation. Maps the 10x10 rect
         * at (10, 10) onto a diamond centered at (32, 32), whose vertices lie on
         * the diagonals (32, 25), (39, 32), (32, 39), (25, 32).
         */

        Affine rotate = new Affine();

        // rotate by 45 degrees, then translate so the diamond lands roughly centered
        rotate.appendRotation(45);
        rotate.prependTranslation(32, 10);

        h.context.setTransform(rotate);
        h.context.fillRect(10, 10, 10, 10);

        assertPixel(32, 32, Color.RED);  // center of the rotated rectangle
        assertPixel(34, 34, Color.RED);  // inside, near the lower-right corner
        assertPixel(22, 32, Color.TRANSPARENT);  // outside, left of the diamond
        assertPixel(32, 16, Color.TRANSPARENT);  // outside, above the diamond
    }

    @Test
    public void shouldShearDrawingWithTransform() {
        h.context.setFill(Color.RED);

        h.context.setTransform(1, 0, 1, 1, 0, 0);
        h.context.fillRect(10, 10, 10, 10);

        assertPixel(30, 15, Color.RED);  // inside the sheared rectangle
        assertPixel(10, 10, Color.TRANSPARENT);  // outside the sheared rectangle
    }

    @Test
    public void shouldReportTransformedDirtyRegion() {
        h.context.setFill(Color.RED);

        h.context.setTransform(1, 0, 0, 1, 10, 20);
        h.context.fillRect(0, 0, 5, 5);

        assertEquals(1, h.dirtyRects.size());
        assertEquals(new Rectangle(10, 20, 5, 5), h.dirtyRects.get(0));
    }

    @Test
    public void shouldClipRect() {
        h.context.setFill(Color.RED);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);

        h.context.clipRect(10, 10, 20, 20);
        h.context.setFill(Color.BLUE);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);

        assertPixel(20, 20, Color.BLUE);  // inside the clip
        assertPixel(5, 5, Color.RED);  // outside the clip
    }

    @Test
    public void shouldClipRectWithTransform() {
        h.context.setFill(Color.RED);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);

        h.context.setTransform(1, 0, 0, 1, 10, 0);
        h.context.clipRect(0, 0, 10, HEIGHT);
        h.context.setFill(Color.BLUE);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);

        assertPixel(15, 20, Color.BLUE);  // inside the transformed clip
        assertPixel(5, 20, Color.RED);  // outside the transformed clip
    }

    @Test
    public void shouldIntersectClipRects() {
        h.context.setFill(Color.RED);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);

        h.context.clipRect(10, 10, 30, 30);
        h.context.clipRect(20, 20, 30, 30);
        h.context.setFill(Color.BLUE);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);

        assertPixel(25, 25, Color.BLUE);  // inside the intersection of both clips
        assertPixel(15, 15, Color.RED);  // inside the first clip only
    }

    @Test
    public void shouldThrowWhenClippingUnderRotatedTransform() {
        h.context.setTransform(0, 1, -1, 0, 0, 0);

        assertThrows(UnsupportedOperationException.class, () -> h.context.clipRect(0, 0, 10, 10));
    }

    @Test
    public void shouldThrowWhenSettingRotatedTransformWithActiveClip() {
        h.context.clipRect(0, 0, 10, 10);

        assertThrows(UnsupportedOperationException.class, () -> h.context.setTransform(0, 1, -1, 0, 0, 0));
    }

    @Test
    public void shouldTranslateAndScaleWithConcatenateMethods() {
        h.context.setFill(Color.RED);

        h.context.translate(10, 20);
        h.context.scale(2, 2);
        h.context.fillRect(0, 0, 5, 5);

        assertPixel(12, 22, Color.RED);  // inside the translated and scaled rectangle
        assertPixel(8, 8, Color.TRANSPARENT);  // outside
        assertPixel(25, 25, Color.TRANSPARENT);  // outside
    }

    @Test
    public void shouldConcatenateTransformOrder() {
        h.context.translate(10, 20);
        h.context.rotate(90);

        Affine a = h.context.getTransform();

        assertEquals(0.0, a.getMxx());
        assertEquals(1.0, a.getMyx());
        assertEquals(-1.0, a.getMxy());
        assertEquals(-0.0, a.getMyy());
        assertEquals(10.0, a.getTx());
        assertEquals(20.0, a.getTy());
    }

    @Test
    public void shouldConcatenateAffineTransform() {
        h.context.transform(new Affine(2, 0, 5, 0, 3, 7));  // scale(2, 3) + translate(5, 7)

        Affine a = h.context.getTransform();

        assertEquals(2.0, a.getMxx());
        assertEquals(0.0, a.getMyx());
        assertEquals(0.0, a.getMxy());
        assertEquals(3.0, a.getMyy());
        assertEquals(5.0, a.getTx());
        assertEquals(7.0, a.getTy());

        h.context.transform(null);  // a null value is ignored and leaves the value unchanged

        assertEquals(5.0, h.context.getTransform().getTx());
    }

    @Test
    public void shouldConcatenateTransformWithSixDoubles() {
        h.context.setTransform(1, 0, 0, 1, 10, 20);
        h.context.transform(2, 0, 0, 3, 0, 0);

        Affine a = h.context.getTransform();

        assertEquals(2.0, a.getMxx());
        assertEquals(3.0, a.getMyy());
        assertEquals(10.0, a.getTx());
        assertEquals(20.0, a.getTy());
    }

    @Test
    public void shouldThrowWhenRotatingWithActiveClip() {
        h.context.clipRect(0, 0, 10, 10);
        h.context.setTransform(1, 0, 0, 1, 5, 5);

        assertThrows(UnsupportedOperationException.class, () -> h.context.rotate(45));

        Affine a = h.context.getTransform();  // the transform is left unchanged

        assertEquals(1.0, a.getMxx());
        assertEquals(0.0, a.getMxy());
        assertEquals(5.0, a.getTx());
        assertEquals(5.0, a.getTy());
    }

    @Test
    public void shouldThrowWhenConcatenatingRotationWithActiveClip() {
        h.context.clipRect(0, 0, 10, 10);

        assertThrows(UnsupportedOperationException.class, () -> h.context.transform(0, 1, -1, 0, 0, 0));
        assertThrows(UnsupportedOperationException.class, () -> h.context.transform(new Affine(0, -1, 0, 1, 0, 0)));
    }

    @Test
    public void shouldClipRectWithScaleTransform() {
        h.context.setFill(Color.RED);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);

        h.context.setTransform(2, 0, 0, 2, 0, 0);
        h.context.clipRect(0, 0, 10, 10);
        h.context.setFill(Color.BLUE);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);

        assertPixel(15, 15, Color.BLUE);  // inside the scaled clip
        assertPixel(25, 25, Color.RED);  // outside the scaled clip
    }

    @Test
    public void shouldSaveAndRestoreAttributes() {
        h.context.setFill(Color.RED);
        h.context.setGlobalAlpha(0.5);

        h.context.save();

        h.context.setFill(Color.BLUE);
        h.context.setGlobalAlpha(1.0);

        h.context.restore();

        assertEquals(Color.RED, h.context.getFill());
        assertEquals(0.5, h.context.getGlobalAlpha());
    }

    @Test
    public void shouldSaveAndRestoreTransform() {
        h.context.setTransform(1, 0, 0, 1, 10, 20);

        h.context.save();

        h.context.setTransform(2, 0, 0, 2, 0, 0);

        h.context.restore();

        Affine a = h.context.getTransform();

        assertEquals(1.0, a.getMxx());
        assertEquals(10.0, a.getTx());
        assertEquals(20.0, a.getTy());
    }

    @Test
    public void shouldSaveAndRestoreClip() {
        h.context.setFill(Color.RED);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);

        h.context.clipRect(10, 10, 30, 30);
        h.context.save();

        h.context.clipRect(20, 20, 30, 30);
        h.context.setFill(Color.BLUE);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);

        assertPixel(25, 25, Color.BLUE);  // inside the intersection of both clips
        assertPixel(15, 15, Color.RED);  // inside the first clip only

        h.context.restore();

        h.context.setFill(Color.GREEN);
        h.context.fillRect(0, 0, WIDTH, HEIGHT);

        assertPixel(15, 15, Color.GREEN);  // inside the restored clip
        assertPixel(5, 5, Color.RED);  // outside the restored clip
    }

    @Test
    public void shouldRestoreWithEmptyStackDoNothing() {
        h.context.setFill(Color.RED);

        h.context.restore();

        assertEquals(Color.RED, h.context.getFill());
    }

    @Test
    public void shouldSaveAndRestoreTextAttributes() {
        h.context.setFont(Font.font(24));
        h.context.setTextAlign(TextAlignment.CENTER);
        h.context.setTextBaseline(VPos.CENTER);
        h.context.setFontSmoothingType(FontSmoothingType.LCD);

        h.context.save();

        h.context.setFont(Font.font(12));
        h.context.setTextAlign(TextAlignment.RIGHT);
        h.context.setTextBaseline(VPos.BOTTOM);
        h.context.setFontSmoothingType(FontSmoothingType.GRAY);

        h.context.restore();

        assertEquals(Font.font(24), h.context.getFont());
        assertEquals(TextAlignment.CENTER, h.context.getTextAlign());
        assertEquals(VPos.CENTER, h.context.getTextBaseline());
        assertEquals(FontSmoothingType.LCD, h.context.getFontSmoothingType());
    }

    @Test
    public void shouldGetAndSetFont() {
        Font f = Font.font(24);

        h.context.setFont(f);

        assertEquals(f, h.context.getFont());

        h.context.setFont(null);  // a null value is ignored and leaves the value unchanged

        assertEquals(f, h.context.getFont());
    }

    @Test
    public void shouldGetAndSetTextAlign() {
        h.context.setTextAlign(TextAlignment.CENTER);

        assertEquals(TextAlignment.CENTER, h.context.getTextAlign());

        h.context.setTextAlign(null);  // a null value is ignored and leaves the value unchanged

        assertEquals(TextAlignment.CENTER, h.context.getTextAlign());
    }

    @Test
    public void shouldGetAndSetTextBaseline() {
        h.context.setTextBaseline(VPos.CENTER);

        assertEquals(VPos.CENTER, h.context.getTextBaseline());

        h.context.setTextBaseline(null);  // a null value is ignored and leaves the value unchanged

        assertEquals(VPos.CENTER, h.context.getTextBaseline());
    }

    @Test
    public void shouldGetAndSetFontSmoothingType() {
        h.context.setFontSmoothingType(FontSmoothingType.LCD);

        assertEquals(FontSmoothingType.LCD, h.context.getFontSmoothingType());

        h.context.setFontSmoothingType(null);  // a null value is ignored and leaves the value unchanged

        assertEquals(FontSmoothingType.LCD, h.context.getFontSmoothingType());
    }

    @Test
    public void shouldFillText() {
        h.context.setFill(Color.BLACK);
        h.context.setFont(Font.font(24));

        h.context.fillText("M", 10, 20);

        assertTrue(hasPaintedPixel(8, 5, 40, 40));  // the glyph should be painted at the given position
        assertFalse(hasPaintedPixel(50, 50, 55, 55));  // nothing should be painted far away
    }

    @Test
    public void shouldStrokeText() {
        h.context.setStroke(Color.BLACK);
        h.context.setFont(Font.font(24));

        h.context.strokeText("M", 10, 20);

        assertTrue(hasPaintedPixel(8, 5, 40, 40));  // the glyph outline should be painted at the given position
    }

    @Test
    public void shouldIgnoreNullText() {
        h.context.fillText(null, 10, 20);
        h.context.strokeText(null, 10, 20);

        assertFalse(hasPaintedPixel(0, 0, WIDTH, HEIGHT));
    }

    private static class Harness {
        final com.sun.prism.Image image;
        final List<Rectangle> dirtyRects = new ArrayList<>();

        SWDrawingContext context;

        Harness(com.sun.prism.Image image) {
            this.image = image;
        }
    }

    private static Harness createHarness(int w, int h) {
        IntBuffer buffer = IntBuffer.allocate(w * h);
        com.sun.prism.Image image = com.sun.prism.Image.fromIntArgbPreData(buffer, w, h);
        Harness harness = new Harness(image);

        harness.context = new SWDrawingContext(image, rect -> harness.dirtyRects.add(rect));

        return harness;
    }

    private static com.sun.prism.Image prismImage(int w, int h) {
        return com.sun.prism.Image.fromIntArgbPreData(IntBuffer.allocate(w * h), w, h);
    }

    private static Image createSolidFxImage(int w, int h, int argbpre) {
        int[] pixels = new int[w * h];

        Arrays.fill(pixels, argbpre);

        com.sun.prism.Image prismImage = com.sun.prism.Image.fromIntArgbPreData(pixels, w, h);
        StubImageLoaderFactory factory = ((StubToolkit) Toolkit.getToolkit()).getImageLoaderFactory();

        factory.registerImage(prismImage, new StubPlatformImageInfo(w, h));

        return Toolkit.getImageAccessor().fromPlatformImage(prismImage);
    }

    private static int argb(int a, int r, int g, int b) {
        return (a << 24) | (r << 16) | (g << 8) | b;
    }

    private void assertPixel(int x, int y, Color expectedColor) {
        assertPixel(x, y, argb(
            (int) Math.round(expectedColor.getOpacity() * 255),
            (int) Math.round(expectedColor.getRed() * 255),
            (int) Math.round(expectedColor.getGreen() * 255),
            (int) Math.round(expectedColor.getBlue() * 255)
        ));
    }

    private void assertPixel(int x, int y, int expectedArgb) {
        assertEquals("0x%08x".formatted(expectedArgb), "0x%08x".formatted(h.image.getArgb(x, y)), "pixel at (" + x + ", " + y + ")");
    }

    private boolean hasPaintedPixel(int x1, int y1, int x2, int y2) {
        for (int y = y1; y < y2; y++) {
            for (int x = x1; x < x2; x++) {
                if (h.image.getArgb(x, y) != 0) {
                    return true;
                }
            }
        }

        return false;
    }

    /*
     * Bounding box of the pixels painted by a SQUARE-capped stroke of the
     * default width (1.0) drawn between the two given points.
     */
    private static Rectangle paintedLineBounds(double x1, double y1, double x2, double y2) {
        double minX = Math.min(x1, x2) - 0.5;
        double maxX = Math.max(x1, x2) + 0.5;
        double minY = Math.min(y1, y2) - 0.5;
        double maxY = Math.max(y1, y2) + 0.5;

        return new Rectangle(
            (int) Math.floor(minX),
            (int) Math.floor(minY),
            (int) Math.ceil(maxX) - (int) Math.floor(minX),
            (int) Math.ceil(maxY) - (int) Math.floor(minY)
        );
    }
}
