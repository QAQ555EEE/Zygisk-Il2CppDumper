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
        // Pass 1: towers/spring (background)
        for (OverlayService.Actor a : actors) {
            if (a.camp != 1 && a.camp != 2) continue;
            float dx = cx + a.x * scale;
            float dy = cy - a.z * scale;
            if (dx < left || dx > left + mapSize || dy < top || dy > top + mapSize) continue;
            if (a.type == 1) { // tower
                dotPaint.setColor(a.camp == 1 ? Color.argb(220, 80, 180, 255)
                                              : Color.argb(220, 255, 100, 100));
                c.drawRect(dx-6, dy-6, dx+6, dy+6, dotPaint);
            } else if (a.type == 5) { // spring/crystal
                dotPaint.setColor(a.camp == 1 ? Color.argb(220, 0, 200, 255)
                                              : Color.argb(220, 255, 60, 60));
                c.drawRect(dx-9, dy-9, dx+9, dy+9, dotPaint);
            }
        }
        // Pass 2: heroes (foreground, big circles with halo)
        for (OverlayService.Actor a : actors) {
            if (a.type != 2 || (a.camp != 1 && a.camp != 2)) continue;
            heroCount++;
            float dx = cx + a.x * scale;
            float dy = cy - a.z * scale;
            if (dx < left || dx > left + mapSize || dy < top || dy > top + mapSize) continue;
            int main, halo;
            if (a.camp == 1) {
                main = Color.argb(255, 80, 180, 255);
                halo = Color.argb(80, 80, 180, 255);
            } else {
                main = Color.argb(255, 255, 70, 70);
                halo = Color.argb(100, 255, 70, 70);
                enemyHero++;
            }
            dotPaint.setColor(halo);
            c.drawCircle(dx, dy, 22f, dotPaint);
            dotPaint.setColor(main);
            c.drawCircle(dx, dy, 12f, dotPaint);
        }

        c.drawText("heroes=" + heroCount + "  enemy=" + enemyHero
                   + "  /" + actors.length,
                   left + 8, top + 30, textPaint);
    }
}
