/*
 * Copyright (c) 2023, 2024, Oracle and/or its affiliates.
 * All rights reserved. Use is subject to license terms.
 *
 * This file is available and licensed under the following license:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  - Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the distribution.
 *  - Neither the name of Oracle Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

package com.oracle.demo.richtext.codearea;

import javafx.application.Platform;
import javafx.geometry.Insets;
import javafx.scene.Scene;
import javafx.scene.control.Label;
import javafx.scene.control.MenuBar;
import javafx.scene.layout.BorderPane;
import javafx.stage.Stage;
import com.oracle.demo.richtext.rta.RichTextAreaWindow;
import com.oracle.demo.richtext.util.FX;
import jfx.incubator.scene.control.richtext.RichTextArea;
import jfx.incubator.scene.control.richtext.TextPos;
import jfx.incubator.scene.control.richtext.model.CodeTextModel;

/**
 * CodeArea Demo window
 *
 * @author Andy Goryachev
 */
public class CodeAreaWindow extends Stage {
    private CodeTextModel model;
    public final CodeAreaDemoPane demoPane;
    public final Label status;

    public CodeAreaWindow(CodeTextModel m) {
        model = (m == null ? new CodeTextModel() : m);
        demoPane = new CodeAreaDemoPane(model);

        MenuBar mb = new MenuBar();
        FX.menu(mb, "File");
        FX.item(mb, "New Window", this::newWindow);
        FX.separator(mb);
        FX.item(mb, "Close Window", this::hide);
        FX.separator(mb);
        FX.item(mb, "Quit", () -> Platform.exit());

        status = new Label();
        status.setPadding(new Insets(2, 10, 2, 10));

        BorderPane bp = new BorderPane();
        bp.setTop(mb);
        bp.setCenter(demoPane);
        bp.setBottom(status);

        Scene scene = new Scene(bp);
        setScene(scene);
        setTitle(
            "CodeArea Tester  JFX:" + System.getProperty("javafx.runtime.version") +
            "  JDK:" + System.getProperty("java.version")
        );
        setWidth(1200);
        setHeight(600);

        demoPane.control.caretPositionProperty().addListener((x) -> updateStatus());
    }

    protected void updateStatus() {
        RichTextArea t = demoPane.control;
        TextPos p = t.getCaretPosition();

        StringBuilder sb = new StringBuilder();

        if (p != null) {
            sb.append(" line=").append(p.index());
            sb.append(" col=").append(p.offset());
        }

        status.setText(sb.toString());
    }

    protected void newWindow() {
        double offset = 20;

        CodeAreaWindow w = new CodeAreaWindow(model);
        w.setX(getX() + offset);
        w.setY(getY() + offset);
        w.setWidth(getWidth());
        w.setHeight(getHeight());
        w.show();
    }
}
