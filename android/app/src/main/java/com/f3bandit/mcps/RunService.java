// MC/PS MemCard Pro for Android: keeps the PocketStation running while the app is put away.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
// A real PocketStation keeps going in your pocket: its clock ticks and its games carry on and
// call you. The emulation does the same in the app's own thread; this service only tells Android
// the app is doing something the user asked for, so it isn't frozen. It shows the quiet
// "PocketStation is running" notification while it's up, and stops when the app comes back.
package com.f3bandit.mcps;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

public class RunService extends Service {
    static final String RUNNING = "running";   // the quiet, ongoing channel
    static final String ALERTS = "alerts";     // games calling
    private static final int ID = 0x4D43;

    static void channels(Context c) {
        if (Build.VERSION.SDK_INT < 26) return;
        NotificationManager nm = c.getSystemService(NotificationManager.class);
        NotificationChannel run = new NotificationChannel(RUNNING, "PocketStation running", NotificationManager.IMPORTANCE_MIN);
        run.setDescription("Shown while the PocketStation keeps running with the app put away.");
        run.setShowBadge(false);
        nm.createNotificationChannel(run);
        NotificationChannel alerts = new NotificationChannel(ALERTS, "Game alerts", NotificationManager.IMPORTANCE_DEFAULT);
        alerts.setDescription("Your PocketStation games calling: events, level ups, alarms.");
        nm.createNotificationChannel(alerts);
    }

    static PendingIntent open(Context c) {
        Intent i = new Intent(c, MCPSActivity.class);
        i.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        return PendingIntent.getActivity(c, 0, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    @SuppressWarnings("deprecation")
    static Notification.Builder builder(Context c, String channel) {
        return Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(c, channel) : new Notification.Builder(c);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        channels(this);
        Notification n = builder(this, RUNNING)
                .setSmallIcon(R.drawable.ic_stat)
                .setContentTitle("PocketStation is running")
                .setContentText("Your games keep going and will call you.")
                .setContentIntent(open(this))
                .setOngoing(true)
                .build();
        try {
            if (Build.VERSION.SDK_INT >= 34) startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            else startForeground(ID, n);
        } catch (Exception e) {
            // Android may refuse (started too late from the background): the app then simply
            // runs until the system pauses it, and catches up when it's back.
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
