package com.sgame.overlay;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.PixelFormat;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;
import android.view.Gravity;
import android.view.WindowManager;

import java.io.DataInputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class OverlayService extends Service {
    private static final String TAG = "sgame_overlay";
    private static final String SOCK = "sgame_esp";   // abstract namespace
    private static final int ACTOR_BYTES = 52;

    private WindowManager wm;
    private OverlayView view;
    private Thread sockThread;
    private volatile boolean running = false;

    @Override
    public IBinder onBind(Intent intent) { return null; }

    @Override
    public void onCreate() {
        super.onCreate();
        startInForeground();

        wm = (WindowManager) getSystemService(WINDOW_SERVICE);
        view = new OverlayView(this);

        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
            WindowManager.LayoutParams.MATCH_PARENT,
            WindowManager.LayoutParams.MATCH_PARENT,
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                : WindowManager.LayoutParams.TYPE_PHONE,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            PixelFormat.TRANSLUCENT
        );
        lp.gravity = Gravity.TOP | Gravity.START;
        try {
            wm.addView(view, lp);
        } catch (Exception e) {
            Log.e(TAG, "addView failed", e);
        }

        running = true;
        sockThread = new Thread(this::socketLoop, "sgame_esp_sock");
        sockThread.start();
    }

    private void startInForeground() {
        String ch = "sgame_overlay";
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(new NotificationChannel(
                ch, "sgame ESP overlay", NotificationManager.IMPORTANCE_LOW));
        }
        Notification.Builder b = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
            ? new Notification.Builder(this, ch)
            : new Notification.Builder(this);
        Notification n = b.setContentTitle("sgame ESP overlay")
            .setContentText("等待 sgame 连接...")
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .build();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(1, n);
        }
    }

    private void socketLoop() {
        while (running) {
            LocalSocket sock = new LocalSocket();
            try {
                sock.connect(new LocalSocketAddress(SOCK,
                    LocalSocketAddress.Namespace.ABSTRACT));
                Log.i(TAG, "connected @" + SOCK);
                DataInputStream in = new DataInputStream(sock.getInputStream());

                byte[] hdrBuf = new byte[8];
                while (running) {
                    in.readFully(hdrBuf);
                    if (hdrBuf[0]!='E' || hdrBuf[1]!='S' || hdrBuf[2]!='P' || hdrBuf[3]!='1') {
                        Log.e(TAG, "bad magic"); break;
                    }
                    int count = ByteBuffer.wrap(hdrBuf, 4, 4)
                        .order(ByteOrder.LITTLE_ENDIAN).getInt();
                    if (count < 0 || count > 256) { Log.e(TAG, "count="+count); break; }

                    byte[] body = new byte[count * ACTOR_BYTES];
                    in.readFully(body);

                    Actor[] actors = new Actor[count];
                    ByteBuffer bb = ByteBuffer.wrap(body).order(ByteOrder.LITTLE_ENDIAN);
                    for (int i = 0; i < count; i++) {
                        Actor a = new Actor();
                        a.key      = bb.getInt();
                        a.type     = bb.getInt();
                        a.configId = bb.getInt();
                        a.camp     = bb.getInt();
                        a.battleOrder = bb.getInt();
                        a.objId    = bb.getInt();
                        a.x = bb.getFloat();
                        a.y = bb.getFloat();
                        a.z = bb.getFloat();
                        a.fwdX = bb.getInt();
                        a.fwdY = bb.getInt();
                        a.fwdZ = bb.getInt();
                        actors[i] = a;
                    }
                    view.setActors(actors);
                }
            } catch (Exception e) {
                Log.w(TAG, "socket error: " + e.getMessage());
            } finally {
                try { sock.close(); } catch (Exception ignored) {}
            }
            // retry every 2s
            try { Thread.sleep(2000); } catch (InterruptedException ignored) {}
        }
    }

    @Override
    public void onDestroy() {
        running = false;
        if (view != null && wm != null) {
            try { wm.removeView(view); } catch (Exception ignored) {}
        }
        if (sockThread != null) sockThread.interrupt();
        super.onDestroy();
    }

    public static class Actor {
        public int key, type, configId, camp, battleOrder, objId;
        public float x, y, z;
        public int fwdX, fwdY, fwdZ;
    }
}
