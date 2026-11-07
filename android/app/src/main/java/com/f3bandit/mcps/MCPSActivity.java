// MC/PS MemCard Pro for Android.
// Copyright (c) 2026 f3bandit. GPL-3.0-or-later.
//
// SDL's activity runs the same native app as the Windows build. This class adds what only
// Android can do: the system file picker (BIOS, cards, saves), the "save to" dialog for exports,
// and files opened from other apps. Picked files are copied into the app's own folder, so the
// native side only ever sees plain paths.
package com.f3bandit.mcps;

import android.Manifest;
import android.app.NotificationManager;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

public class MCPSActivity extends SDLActivity {
    private static final int PICK = 0x4D43;   // "MC"
    private static final int EXPORT = 0x5053; // "PS"
    private String pendingExport;             // the file waiting for a "save to" location

    // Native side (app/main.cpp).
    public static native void nativeFilePicked(String path);
    public static native void nativeNote(String text);

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        RunService.channels(this);
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] { Manifest.permission.POST_NOTIFICATIONS }, 1);
        handleView(getIntent());
    }

    // Put away: keep the PocketStation running (RunService). Back in front: no need.
    @Override
    protected void onPause() {
        super.onPause();
        try {
            Intent i = new Intent(this, RunService.class);
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(i);
            else startService(i);
        } catch (Exception ignored) { }
    }

    @Override
    protected void onResume() {
        super.onResume();
        stopService(new Intent(this, RunService.class));
    }

    private static int nextAlert = 1;

    // Called from native code: a game is calling (app/alerts.hpp).
    public static void postAlert(String title, String text) {
        final MCPSActivity a = (MCPSActivity) mSingleton;
        if (a == null) return;
        android.app.Notification n = RunService.builder(a, RunService.ALERTS)
                .setSmallIcon(R.drawable.ic_stat)
                .setContentTitle(title)
                .setContentText(text)
                .setContentIntent(RunService.open(a))
                .setAutoCancel(true)
                .build();
        a.getSystemService(NotificationManager.class).notify(0x100 + (nextAlert++ & 0xFF), n);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        handleView(intent);
    }

    private void handleView(Intent intent) {
        if (intent != null && Intent.ACTION_VIEW.equals(intent.getAction()) && intent.getData() != null)
            importUri(intent.getData());
    }

    // Called from native code: show the system picker for any file.
    public static void pickFile() {
        final MCPSActivity a = (MCPSActivity) mSingleton;
        a.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            a.startActivityForResult(i, PICK);
        });
    }

    // Called from native code: ask where to save `path`, offering `name`.
    public static void exportFile(String path, String name) {
        final MCPSActivity a = (MCPSActivity) mSingleton;
        a.runOnUiThread(() -> {
            a.pendingExport = path;
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("application/octet-stream");
            i.putExtra(Intent.EXTRA_TITLE, name);
            a.startActivityForResult(i, EXPORT);
        });
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        if (request == PICK) importUri(data.getData());
        else if (request == EXPORT && pendingExport != null) {
            Uri uri = data.getData();
            try (InputStream in = new FileInputStream(pendingExport);
                 OutputStream out = getContentResolver().openOutputStream(uri)) {
                copy(in, out);
                nativeNote("Saved " + displayName(uri));
            } catch (Exception e) {
                nativeNote("Couldn't save: " + e.getMessage());
            }
            pendingExport = null;
        }
    }

    private void importUri(Uri uri) {
        try {
            File dir = new File(getExternalFilesDir(null), "imports");
            dir.mkdirs();
            File f = new File(dir, displayName(uri).replaceAll("[/\\\\]", "_"));
            try (InputStream in = getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(f)) {
                copy(in, out);
            }
            nativeFilePicked(f.getAbsolutePath());
        } catch (Exception e) {
            nativeNote("Couldn't open that file: " + e.getMessage());
        }
    }

    private String displayName(Uri uri) {
        String name = null;
        try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) name = c.getString(0);
        } catch (Exception ignored) { }
        if (name == null) name = uri.getLastPathSegment();
        return name == null ? "file" : name;
    }

    private static void copy(InputStream in, OutputStream out) throws java.io.IOException {
        byte[] b = new byte[65536];
        int n;
        while ((n = in.read(b)) > 0) out.write(b, 0, n);
    }
}
