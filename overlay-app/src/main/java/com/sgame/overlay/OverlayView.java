package com.sgame.overlay;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.View;

public class OverlayView extends View {
    private OverlayService.Actor[] actors = new OverlayService.Actor[0];
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

        // Only draw ENEMY heroes (camp=2, type=2). Game already shows own team on its minimap.
        int enemyHero = 0;
        int rawType2Camp2 = 0;
        StringBuilder dbg = new StringBuilder();
        for (OverlayService.Actor a : actors) {
            if (a.type != 2 || a.camp != 2) continue;
            rawType2Camp2++;
            // Heuristic: real heroes have battleOrder 0..9 (5v5).
            // Summons / pets / clones get higher battleOrder.
            if (a.battleOrder < 0 || a.battleOrder > 9) continue;
            float dx = cx + a.x * scale;
            float dy = cy - a.z * scale;
            // Clamp to map edge (don't drop off-map - draw at boundary so user sees direction)
            float clampedX = Math.max(left + 16, Math.min(left + mapSize - 16, dx));
            float clampedY = Math.max(top + 16, Math.min(top + mapSize - 16, dy));
            boolean offMap = (clampedX != dx || clampedY != dy);

            enemyHero++;
            int main = Color.argb(255, 255, 70, 70);
            int halo = Color.argb(offMap ? 60 : 120, 255, 70, 70);
            dotPaint.setColor(halo);
            c.drawCircle(clampedX, clampedY, 30f, dotPaint);
            dotPaint.setColor(main);
            c.drawCircle(clampedX, clampedY, 16f, dotPaint);
            // Label: battleOrder index
            textPaint.setColor(Color.WHITE);
            c.drawText(String.valueOf(a.battleOrder), clampedX - 8, clampedY + 9, textPaint);
            if (dbg.length() < 240) {
                dbg.append("b").append(a.battleOrder)
                   .append(" cfg=").append(a.configId)
                   .append(" (").append((int)a.x).append(",").append((int)a.z).append(") ");
            }
        }

        c.drawText("enemy=" + enemyHero + " raw=" + rawType2Camp2
                   + "  /" + actors.length,
                   left + 8, top + 30, textPaint);
        if (dbg.length() > 0) {
            android.util.Log.i("sgame_overlay", "enemies: " + dbg);
        }
    }
}
