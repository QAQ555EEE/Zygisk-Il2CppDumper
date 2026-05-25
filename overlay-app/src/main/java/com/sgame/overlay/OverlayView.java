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

        int heroCount = 0;
        int enemyHero = 0;
        // Draw dots: heroes biggest+colored, others small grey
        for (OverlayService.Actor a : actors) {
            float dx = cx + a.x * scale;
            float dy = cy - a.z * scale;  // z up on minimap (flip)
            if (dx < left || dx > left + mapSize || dy < top || dy > top + mapSize) continue;

            int color;
            float radius;
            if (a.type == 2) { // hero
                heroCount++;
                if (a.camp == 1) {        // blue (me)
                    color = Color.argb(255, 80, 180, 255);
                    radius = 14f;
                } else if (a.camp == 2) { // red (enemy)
                    color = Color.argb(255, 255, 70, 70);
                    radius = 14f;
                    enemyHero++;
                } else {
                    color = Color.YELLOW;
                    radius = 10f;
                }
            } else if (a.type == 1) { // tower
                color = Color.argb(200, 200, 200, 200);
                radius = 8f;
            } else if (a.type == 5) { // spring
                color = Color.argb(180, 100, 255, 100);
                radius = 7f;
            } else {                  // type 0 = monster/soldier
                color = Color.argb(160, 180, 180, 100);
                radius = 4f;
            }
            dotPaint.setColor(color);
            c.drawCircle(dx, dy, radius, dotPaint);
        }

        c.drawText("actors=" + actors.length + "  enemy=" + enemyHero,
                   left + 8, top + 30, textPaint);
    }
}
