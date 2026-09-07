package com.admenri.urge;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.widget.TextView;

/**
 * Lightweight splash screen. Its only job is to run the heavy, blocking asset
 * preparation (APK MD5 + extraction, ~tens of seconds on TV SoCs) on a background
 * thread while showing a "loading" view, so the main (UI) thread never blocks and
 * the system never raises an ANR. Once assets are ready it launches URGEMain.
 */
public class SplashActivity extends Activity {
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private TextView mStatus;

    /** Mirrors the latest diag message on screen, so boot progress and the point
     *  of a crash are readable directly on the TV without any storage. */
    private final Runnable mStatusTick = new Runnable() {
        @Override
        public void run() {
            if (mStatus != null) mStatus.setText(URGEMain.getDiagStatus());
            mHandler.postDelayed(this, 500);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Diagnostics: primary logs go to internal storage and are exported to
        // removable storage in the background; the screen shows the latest line.
        URGEMain.startDiagnostics(this);
        URGEMain.logDiag("Splash onCreate start");

        // Load the engine libraries here: prepareAssets() calls the native
        // nativeDropPageCache(), but these libs are otherwise only loaded when
        // URGEMain (SDLActivity) is created — which happens after prepareAssets.
        // Separate try blocks: one failed load must not skip the other.
        try {
            System.loadLibrary("SDL3");
        } catch (Throwable t) {
            URGEMain.logDiag("Splash loadLibrary(SDL3) failed: " + t);
        }
        try {
            System.loadLibrary("main");
        } catch (Throwable t) {
            URGEMain.logDiag("Splash loadLibrary(main) failed: " + t);
        }

        mStatus = new TextView(this);
        mStatus.setText("加载中…");
        mStatus.setGravity(Gravity.CENTER);
        mStatus.setTextColor(Color.WHITE);
        mStatus.setTextSize(22);
        setContentView(mStatus);
        mHandler.post(mStatusTick);

        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    URGEMain.logDiag("Splash prepareAssets start");
                    AssetExtractor.note("Splash prepareAssets start");
                    URGEMain.prepareAssets(SplashActivity.this);
                    URGEMain.logDiag("Splash prepareAssets done");
                    AssetExtractor.note("Splash prepareAssets done");
                } catch (Throwable t) {
                    URGEMain.logDiag("Splash prepareAssets FAILED: " + t);
                    AssetExtractor.note("Splash prepareAssets FAILED: " + t);
                    t.printStackTrace();
                    return;
                }
                // Hold the loading screen briefly when the previous run crashed,
                // so the on-screen LAST CRASH line can actually be read (the
                // engine relaunches right after and the text would be gone).
                mHandler.postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        AssetExtractor.note("Splash launching URGEMain");
                        startActivity(new Intent(SplashActivity.this, URGEMain.class));
                        finish();
                    }
                }, URGEMain.sHadLastCrash ? 10000 : 0);
            }
        }).start();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        mHandler.removeCallbacks(mStatusTick);
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        URGEMain.logMemoryEvent("Splash", level);
    }

    @Override
    public void onLowMemory() {
        super.onLowMemory();
        URGEMain.logLowMemory("Splash");
    }
}
