/*
 * Copyright (c) 2023, 2024, Oracle and/or its affiliates. All rights reserved.
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
package com.sun.jfx.incubator.scene.control.input;

import javafx.event.Event;
import javafx.event.EventType;

/**
 * This interface enables wider control in specifying conditional matching logic when adding skin/behavior handlers.
 *
 * @param <T> the type of the event
 */
public interface EventCriteria<T extends Event> {
    /**
     * Returns the event type for which this criteria are valid.
     * @return the event type
     */
    public EventType<T> getEventType();

    /**
     * Returns true if the specified event matches this criteria.
     * @param ev the event
     * @return true if match occurs
     */
    public boolean isEventAcceptable(T ev);
}
