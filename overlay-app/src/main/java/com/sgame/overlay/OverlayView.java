package com.sgame.overlay;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.View;

public class OverlayView extends View {
    private OverlayService.Actor[] actors = new OverlayService.Actor[0];
    private int dumpCounter = 0;
    private final Paint dotPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint axisPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    // Minimap area in top-right corner
    private static final float MAP_SIZE_DP = 240;
    private static final float MAP_MARGIN_DP = 16;
    // sgame map world coordinate range (verified): roughly -65..+65 Unity units
    private static final float WORLD_HALF = 65f;

    public OverlayView(Context ctx) {
        super(ctx);
        dotPaint.setStyle(Paint.Style.FILL);
        textPaint.setColor(Color.WHITE);
        textPaint.setTextSize(28);
        bgPaint.setColor(Color.argb(120, 0, 0, 0));
        axisPaint.setColor(Color.argb(80, 255, 255, 255));
        axisPaint.setStrokeWidth(1.5f);
    }

    public void setActors(OverlayService.Actor[] a) {
        this.actors = a;
        postInvalidate();
    }

    @Override
    protected void onDraw(Canvas c) {
        super.onDraw(c);
        float density = getResources().getDisplayMetrics().density;
        float mapSize = MAP_SIZE_DP * density;
        float margin = MAP_MARGIN_DP * density;
        float left = getWidth() - mapSize - margin;
        float top = margin + 100;  // below status bar
        float cx = left + mapSize / 2;
        float cy = top + mapSize / 2;

        // Background panel
        c.drawRect(left, top, left + mapSize, top + mapSize, bgPaint);
        // Crosshair
        c.drawLine(left, cy, left + mapSize, cy, axisPaint);
        c.drawLine(cx, top, cx, top + mapSize, axisPaint);

        float scale = (mapSize / 2) / WORLD_HALF;

        // ActorTypeDef in sgame: 0=HERO, 1=MONSTER, 2=ORGAN(tower/crystal),
        // 4=CALL(summon), 5=BULLET, 10=PET. We want HERO=0 only.
        int rawHero = 0;
        StringBuilder dbg = new StringBuilder();
        for (OverlayService.Actor a : actors) {
            if (a.type != 0 || a.camp != 2) continue;
            rawHero++;
            float dx = cx + a.x * scale;
            float dy = cy - a.z * scale;
            float clampedX = Math.max(left + 14, Math.min(left + mapSize - 14, dx));
            float clampedY = Math.max(top + 14, Math.min(top + mapSize - 14, dy));
            boolean offMap = (clampedX != dx || clampedY != dy);

            int main = Color.argb(255, 255, 70, 70);
            int halo = Color.argb(offMap ? 50 : 100, 255, 70, 70);
            dotPaint.setColor(halo);
            c.drawCircle(clampedX, clampedY, 22f, dotPaint);
            dotPaint.setColor(main);
            c.drawCircle(clampedX, clampedY, 11f, dotPaint);
            // Label: "bX/cY" battleOrder/configId-suffix
            textPaint.setColor(Color.WHITE);
            textPaint.setTextSize(20);
            c.drawText("b" + a.battleOrder, clampedX - 14, clampedY - 14, textPaint);
            c.drawText(String.valueOf(a.configId % 1000), clampedX - 16, clampedY + 26, textPaint);
            textPaint.setTextSize(28);
        }

        c.drawText("enemy hero=" + rawHero + "  /" + actors.length,
                   left + 8, top + 30, textPaint);

        // Dump every ~1.5s (10th call at 150ms tick).
        dumpCounter++;
        if (dumpCounter >= 10) {
            dumpCounter = 0;
            for (OverlayService.Actor a : actors) {
                if (a.type != 0 || a.camp != 2) continue;
                if (dbg.length() < 800) {
                    dbg.append("b").append(a.battleOrder)
                       .append(" cfg=").append(a.configId)
                       .append(" obj=").append(a.objId)
                       .append(" (").append((int)a.x).append(",").append((int)a.z).append(") | ");
                }
            }
            if (dbg.length() > 0) {
                android.util.Log.i("sgame_overlay", "enemies: " + dbg);
            }
        }
    }
}
