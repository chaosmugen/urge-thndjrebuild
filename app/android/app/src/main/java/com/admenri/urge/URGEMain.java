package com.admenri.urge;

import android.content.res.AssetManager;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.content.Context;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.BufferedReader;
import java.security.MessageDigest;

import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;
import com.admenri.urge.AssetExtractor;

import android.os.Handler;
import android.os.Looper;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

public class URGEMain extends SDLActivity {
    private static final String TAG = "URGEActivity";
    private static final String MD5_VFY_FILE = "URGE.vfy";
    // Marks an extraction that started for a given APK but never finished, so the
    // next launch can resume instead of restarting from the first file.
    private static final String MD5_PARTIAL_FILE = "URGE.partial";
    public static String GAME_PATH;

    // ===== Diagnostics: self-contained log to removable storage (no ADB needed) =====
    private static final boolean DIAG_ENABLED = true;
    private static File sDiagDir;
    // Removable-storage mirror of the diag dir (U盘). Null when no removable volume
    // is mounted/writable; re-probed periodically so a late plug-in still works.
    private static File sRemovableDir;
    private static Context sAppContext;
    // Latest diagnostic message, mirrored onto the splash screen so the TV itself
    // shows how far the boot got — readable without any storage at all.
    private static volatile String sLastDiag = "";
    private static volatile String sLastCrashLine = null;
    private static final AtomicLong sLastUiTick = new AtomicLong(System.currentTimeMillis());
    private static Handler sMainHandler;
    private static volatile long sLastDump = 0;
    private static boolean sDiagInited = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        AssetExtractor.note("URGEMain onCreate: before super.onCreate");
        super.onCreate(savedInstanceState);
        AssetExtractor.note("URGEMain onCreate: after super.onCreate");

        // Current context
        AssetExtractor.note("URGEMain onCreate: getting context");
        Context context = getContext();

        // Setup external storage dirs (fast, non-blocking)
        java.io.File data_dir = context.getExternalFilesDir("");
        if (data_dir != null) {
            GAME_PATH = data_dir.toString();
            Log.i(TAG, "Work Directory: " + GAME_PATH);
        }

        // Initialize self-diagnostics (writes to removable storage if present)
        AssetExtractor.note("URGEMain onCreate: before startDiagnostics");
        startDiagnostics(context);
        AssetExtractor.note("URGEMain onCreate: after startDiagnostics");
        // Hand the removable-storage diag dir to the native engine (libs are loaded
        // by super.onCreate) so it can dump logs there.
        if (sDiagDir != null) {
            try {
                nativeSetDiagPath(sDiagDir.getAbsolutePath());
            } catch (Throwable t) {
                // Never swallow this silently: without the diag path the engine
                // writes its logs and crash reports nowhere, which then looks
                // exactly like "the crash left no trace at all".
                logDiag("nativeSetDiagPath failed: " + t);
            }
        }
        // The crash handler writes its report to BOTH paths: the internal one can
        // only reach the U盘 through the periodic export, which keeps losing the
        // race against the ~18s crash window. The removable path is written
        // directly, so the report is on the U盘 the moment it is produced.
        if (sRemovableDir == null) sRemovableDir = pickRemovableDir(this);
        if (sRemovableDir != null) {
            try {
                nativeSetRemovableDiagPath(sRemovableDir.getAbsolutePath());
            } catch (Throwable t) {
                logDiag("nativeSetRemovableDiagPath failed: " + t);
            }
        }
        // Android TV (Leanback) flag for the game scripts: the boot-time data
        // preload exceeds what a low-RAM TV box can give a 32-bit process, so
        // the scripts branch to lazy loading when this is set.
        boolean isTv = getPackageManager().hasSystemFeature(
                android.content.pm.PackageManager.FEATURE_LEANBACK);
        try {
            nativeSetTvDevice(isTv);
        } catch (Throwable t) {
            logDiag("nativeSetTvDevice failed: " + t);
        }
        logDiag("onCreate start");

        // Heavy asset MD5 + extraction is performed in SplashActivity's background
        // thread. If we somehow reach here without preparation, bounce to Splash.
        if (GAME_PATH == null || !new File(GAME_PATH, MD5_VFY_FILE).exists()) {
            logDiag("URGE.vfy missing, redirect to SplashActivity");
            startActivity(new Intent(context, SplashActivity.class));
            finish();
            return;
        }
        logDiag("assets already prepared, continuing");
        AssetExtractor.note("URGEMain onCreate: assets prepared, continuing");
        // Proactively copy the engine log so it's reachable on the U盘 even when a
        // native crash kills the process before dumpDiag() (which normally copies it) runs.
        copyGameLog();
        AssetExtractor.note("URGEMain onCreate: end");
    }

    /**
     * Tell the render thread to stop touching the ANativeWindow.
     *
     * SDL releases the window from surfaceDestroyed() without waiting for the
     * render thread, so the Vulkan backend used to resize its swap chain on a
     * window that was already gone and crashed inside
     * vkCreateAndroidSurfaceKHR. This runs before super.onPause() (and hence
     * before surfaceDestroyed()) to close that window.
     */
    @Override
    protected void onPause() {
        nativeSuspendGraphics();
        super.onPause();
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        logMemoryEvent("URGEMain", level);
    }

    @Override
    public void onLowMemory() {
        super.onLowMemory();
        logLowMemory("URGEMain");
    }

    public static native void nativeSuspendGraphics();
    // Pass the removable-storage diag dir to the native engine so it can tee its
    // stdout/stderr and spdlog output to a file reachable without ADB.
    public static native void nativeSetDiagPath(String path);
    // Second path (the U盘) the crash handler reports into directly.
    public static native void nativeSetRemovableDiagPath(String path);
    // Android TV (Leanback) detection, exposed to Ruby as URGE_ANDROID_TV.
    public static native void nativeSetTvDevice(boolean isTv);

    /**
     * Drops the kernel page cache of every regular file under <path> (or of the
     * file itself when <path> is a file). See prepareAssets() for why.
     */
    public static native void nativeDropPageCache(String path);

    public static String getApkMD5(Context context) {
        try {
            String apkPath = context.getPackageResourcePath();
            FileInputStream fis = new FileInputStream(apkPath);
            MessageDigest md = MessageDigest.getInstance("MD5");
            byte[] buffer = new byte[8192];
            int bytesRead;
            while ((bytesRead = fis.read(buffer)) != -1) {
                md.update(buffer, 0, bytesRead);
            }
            byte[] md5sum = md.digest();
            StringBuilder sb = new StringBuilder();
            for (byte b : md5sum) {
                sb.append(String.format("%02x", b));
            }
            fis.close();
            return sb.toString();
        } catch (Exception e) {
            e.printStackTrace();
        }
        return null;
    }

    public static boolean checkMD5Consistent(Context context, String currentMD5) {
        File recordFile = new File(GAME_PATH, MD5_VFY_FILE);
        try {
            if (!recordFile.exists()) return false;

            BufferedReader reader = new BufferedReader(new FileReader(recordFile));
            String savedMD5 = reader.readLine();
            reader.close();

            Log.i(TAG, "Current MD5: " + currentMD5);
            Log.i(TAG, "Local Resource MD5: " + savedMD5);

            return savedMD5 != null && savedMD5.equals(currentMD5);
        } catch (IOException e) {
            e.printStackTrace();
            return false;
        }
    }

    /**
     * Verify APK MD5 and extract assets to external storage.
     * MUST run OFF the main (UI) thread — it performs ~tens of seconds of blocking
     * IO (the original cause of the ANR / black-screen crash on TV). The caller is
     * responsible for invoking this from a background thread.
     */
    public static File getDiagDir() { return sDiagDir; }

    public static void prepareAssets(Context context) {
        java.io.File data_dir = context.getExternalFilesDir("");
        if (data_dir != null) {
            GAME_PATH = data_dir.toString();
        }
        logDiag("prepareAssets: before getApkMD5");
        String currentMD5 = getApkMD5(context);
        logDiag("prepareAssets: after getApkMD5");
        if (!checkMD5Consistent(context, currentMD5)) {
            if (sDiagDir != null) {
                AssetExtractor.setExtractLog(new File(sDiagDir, "extract.log"));
            }
            // Extraction of this APK gets interrupted before it finishes on some
            // devices. Without a marker meaning "already in progress for THIS apk",
            // every launch would restart at the first file, redo most of the work and
            // get killed again, so the game would never start. Resuming skips what is
            // already on disk and only copies the remainder.
            boolean resume = checkPartialConsistent(currentMD5);
            if (!resume) {
                writeRecord(MD5_PARTIAL_FILE, currentMD5);
            }
            logDiag("prepareAssets: start extractAssets (resume=" + resume + ")");
            AssetExtractor.note("prepareAssets: start extractAssets (resume=" + resume + ")");
            AssetExtractor.extractAssets(context, !resume);
            logDiag("prepareAssets: end extractAssets");
            AssetExtractor.note("prepareAssets: end extractAssets");
        } else {
            logDiag("prepareAssets: extract skipped (md5 consistent)");
            AssetExtractor.note("prepareAssets: extract skipped (md5 consistent)");
        }

        // The extraction streamed ~1GB of game data (plus the ~1GB APK read for the
        // MD5) through the page cache. On this TV that leaves only ~60MB of free
        // RAM, and the engine's startup burst (RSS +90MB in seconds) then forces
        // heavy direct reclaim — lmkd answers that by killing the process right
        // before the first script even runs. Releasing our own clean cache gives
        // that burst headroom again.
        try {
            if (GAME_PATH != null) nativeDropPageCache(GAME_PATH);
            nativeDropPageCache(context.getPackageResourcePath());
            logDiag("prepareAssets: page cache dropped");
        } catch (Throwable t) {
            logDiag("prepareAssets: nativeDropPageCache failed: " + t);
        }

        logDiag("prepareAssets: writing vfy");
        AssetExtractor.note("prepareAssets: writing vfy");
        try {
            File recordFile = new File(GAME_PATH, MD5_VFY_FILE);
            FileWriter writer = new FileWriter(recordFile);
            writer.write(currentMD5);
            writer.close();
        } catch (IOException e) {
            e.printStackTrace();
        }
        // Extraction finished for this APK, so drop the in-progress marker.
        new File(GAME_PATH, MD5_PARTIAL_FILE).delete();
        logDiag("prepareAssets: done");
        AssetExtractor.note("prepareAssets: done");
    }

    /** True when a previous launch already started extracting this exact APK. */
    private static boolean checkPartialConsistent(String currentMD5) {
        File recordFile = new File(GAME_PATH, MD5_PARTIAL_FILE);
        try {
            if (!recordFile.exists()) return false;
            BufferedReader reader = new BufferedReader(new FileReader(recordFile));
            String savedMD5 = reader.readLine();
            reader.close();
            return savedMD5 != null && savedMD5.equals(currentMD5);
        } catch (IOException e) {
            return false;
        }
    }

    private static void writeRecord(String name, String value) {
        try {
            File recordFile = new File(GAME_PATH, name);
            FileWriter writer = new FileWriter(recordFile);
            writer.write(value);
            writer.close();
        } catch (IOException e) {
            e.printStackTrace();
        }
    }

    // ===== Diagnostics implementation =====

    /**
     * Primary diag dir: internal storage. It is always writable (the whole game is
     * extracted there), so log capture can never be lost to a missing or failing
     * removable volume. Removable storage is only used as an export target.
     */
    private static File pickPrimaryDir(Context ctx) {
        return new File(ctx.getExternalFilesDir(null), "urge_debug");
    }

    /** Removable-storage diag dir (U盘/SD), or null when absent or not writable. */
    private static File pickRemovableDir(Context ctx) {
        if (android.os.Build.VERSION.SDK_INT >= 19) {
            File[] dirs = ctx.getExternalFilesDirs(null);
            if (dirs != null) {
                for (File d : dirs) {
                    if (d == null) continue;
                    try {
                        if (android.os.Environment.isExternalStorageRemovable(d) && d.canWrite()) {
                            File dir = new File(d, "urge_debug");
                            dir.mkdirs();
                            return dir;
                        }
                    } catch (Throwable t) { /* ignore */ }
                }
            }
        }
        return null;
    }

    public static void startDiagnostics(final Context ctx) {
        if (!DIAG_ENABLED) return;
        if (sDiagInited) return;
        sDiagInited = true;
        sAppContext = ctx.getApplicationContext();
        try {
            sDiagDir = pickPrimaryDir(ctx);
            sDiagDir.mkdirs();
        } catch (Throwable t) {
            sDiagDir = null;
        }
        sRemovableDir = pickRemovableDir(ctx);
        logDiag("diagnostics dir=" + (sDiagDir == null ? "null" : sDiagDir.getAbsolutePath())
                + " removable=" + (sRemovableDir == null ? "null" : sRemovableDir.getAbsolutePath())
                + " build=20260907-gctest");

        // Surface the previous run's crash directly on the splash screen (it
        // mirrors diag lines), so the verdict no longer depends on any file
        // surviving the U盘 round trip.
        showLastCrashOnScreen();

        // Stream this process's own logcat (which carries native Ruby/SDL stderr we
        // can't otherwise reach without ADB) to the U盘 so the real crash message
        // is recoverable. Runs in a background thread so it survives a later native
        // crash of the main process.
        startLogcatCapture();

        // Watchdog: detect main-thread blocking (ANR-like) without logcat.
        sMainHandler = new Handler(Looper.getMainLooper());
        sMainHandler.post(new Runnable() {
            @Override
            public void run() {
                sLastUiTick.set(System.currentTimeMillis());
                sMainHandler.postDelayed(this, 1000);
            }
        });
        new Thread(new Runnable() {
            @Override
            public void run() {
                while (true) {
                    try { Thread.sleep(1000); } catch (InterruptedException e) { break; }
                    long gap = System.currentTimeMillis() - sLastUiTick.get();
                    if (gap > 5000) {
                        long now = System.currentTimeMillis();
                        if (now - sLastDump > 10000) {
                            sLastDump = now;
                            dumpDiag("UI_BLOCKED", gap, null);
                        }
                    }
                }
            }
        }, "urge-watchdog").start();

        // Periodically export the internal logs to removable storage. Keeps a recent
        // copy on the U盘 even when the process is killed mid-run, and re-probes for
        // a U盘 plugged in after startup.
        new Thread(new Runnable() {
            @Override
            public void run() {
                // Copy immediately at startup: this carries the PREVIOUS run's
                // native_crash.log (written by the engine's crash handler) to the
                // U盘 — after the process dies nothing exports anymore, and with
                // the old 15s-first loop the first copy could land after the next
                // crash (~18s in on the skip-extraction path).
                copyDiagToRemovable();
                while (true) {
                    try { Thread.sleep(15000); } catch (InterruptedException e) { break; }
                    copyDiagToRemovable();
                }
            }
        }, "urge-log-export").start();

        // Sample system memory pressure (PSI), our oom score and the key meminfo
        // fields every 3s. The process dies with a bare SIGKILL and no log of its
        // own, so the trend right before the death is the only way to see why.
        new Thread(new Runnable() {
            @Override
            public void run() {
                while (true) {
                    try { Thread.sleep(3000); } catch (InterruptedException e) { break; }
                    logDiag(systemState());
                }
            }
        }, "urge-sys-sample").start();

        // Catch uncaught crashes (Java/native via wrapped handler).
        final Thread.UncaughtExceptionHandler old = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override
            public void uncaughtException(Thread t, Throwable e) {
                dumpDiag("UNCAUGHT", -1, e);
                if (old != null) old.uncaughtException(t, e);
                else {
                    try { Thread.sleep(200); } catch (InterruptedException ignored) {}
                    android.os.Process.killProcess(android.os.Process.myPid());
                    System.exit(1);
                }
            }
        });
    }

    /**
     * Capture this process's own logcat (incl. native Ruby/SDL stderr) to
     * <diagDir>/logcat.txt. A normal app may only read its own UID's logs, which is
     * exactly the Ruby/engine output we need. Runs in a background thread; even if
     * the main process native-crashes, logcat keeps the crashing lines on disk.
     */
    private static void startLogcatCapture() {
        try { Runtime.getRuntime().exec(new String[]{"logcat", "-c"}); } catch (Throwable ignored) {}

        // A plain child of this process dies together with it whenever the system
        // kills the whole process group — exactly when the interesting events
        // (lmkd / am_kill) get logged. Running logcat under setsid gives it its own
        // session, so it survives and records the reason for the kill. -b all also
        // grabs the crash buffer (native "Fatal signal" + backtrace).
        boolean setsid = new File("/system/bin/setsid").exists()
                || new File("/system/xbin/setsid").exists();

        // Internal storage only: a second copy on removable storage doubles the
        // chance of logcat dying on a write error before it records the kill.
        // The periodic export copies this file to the U盘 instead.
        if (sDiagDir != null) {
            File current = new File(sDiagDir, "logcat.txt");
            // Keep at most two generations. Without this the capture grows
            // without bound across runs and the periodic export eventually
            // fills the U盘 — every later write then lands as a 0-byte ghost.
            File previous = new File(sDiagDir, "logcat.prev.txt");
            previous.delete();
            current.renameTo(previous);
            String path = current.getAbsolutePath();
            String[] cmd = setsid
                    ? new String[]{"setsid", "logcat", "-b", "all", "-v", "threadtime", "-f", path}
                    : new String[]{"logcat", "-b", "all", "-v", "threadtime", "-f", path};
            try {
                Runtime.getRuntime().exec(cmd);
            } catch (Throwable ignored) {}
        }
    }

    private static String ts() {
        return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS").format(new Date());
    }

    /**
     * True once a previous run's crash report was found and echoed on the
     * splash. SplashActivity then holds the loading screen briefly so the LAST
     * CRASH line can actually be read before the engine is launched.
     */
    public static volatile boolean sHadLastCrash = false;

    /**
     * Reads the newest native_crash_&lt;pid&gt;.log and echoes its last "==== crash"
     * line through logDiag — which the splash screen mirrors and diag.log keeps.
     */
    private static void showLastCrashOnScreen() {
        try {
            File[] files = sDiagDir.listFiles();
            File newest = null;
            if (files != null) {
                for (File f : files) {
                    if (f.getName().startsWith("native_crash_")
                            && (newest == null || f.lastModified() > newest.lastModified()))
                        newest = f;
                }
            }
            if (newest == null) return;
            String last = null;
            java.io.BufferedReader reader =
                    new java.io.BufferedReader(new java.io.FileReader(newest));
            try {
                for (String line; (line = reader.readLine()) != null; ) {
                    if (line.startsWith("==== crash")) last = line;
                }
            } finally {
                reader.close();
            }
            if (last != null) {
                sHadLastCrash = true;
                sLastCrashLine = last;
                logDiag("LAST CRASH: " + last);
            }
        } catch (Throwable t) {
            logDiag("read last crash failed: " + t);
        }
    }

    /**
     * Exports every log file from the internal diag dir to the removable dir.
     * Runs periodically; also re-probes for a U盘 plugged in after startup.
     */
    private static void copyDiagToRemovable() {
        if (sDiagDir == null || sAppContext == null) return;
        if (sRemovableDir == null) sRemovableDir = pickRemovableDir(sAppContext);
        if (sRemovableDir == null) return;
        // A full U盘 turns every later write into a 0-byte ghost file. Skip the
        // export instead of churning the disk.
        if (sRemovableDir.getUsableSpace() < 64L * 1024 * 1024) return;

        File[] files = sDiagDir.listFiles();
        if (files == null) return;
        for (File src : files) {
            copyFileBestEffort(src, new File(sRemovableDir, src.getName()));
        }
    }

    private static void copyFileBestEffort(File src, File dst) {
        // Copy into a sibling temp file and publish with a rename. The export runs
        // every 15s while the process keeps getting killed mid-run; truncating the
        // destination directly used to leave a set of 0-byte files behind.
        File tmp = new File(dst.getAbsolutePath() + ".tmp");
        InputStream in = null;
        OutputStream out = null;
        try {
            in = new FileInputStream(src);
            out = new FileOutputStream(tmp);
            // Cap huge sources (logcat) to their tail: only the end of the log
            // matters, and copying gigabytes was filling the U盘.
            long skip = src.length() - 8L * 1024 * 1024;
            while (skip > 0) {
                long n = in.skip(skip);
                if (n <= 0) break;
                skip -= n;
            }
            byte[] buffer = new byte[1 << 16];
            int n;
            while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
        } catch (IOException e) {
            tmp.delete();
            return;
        } finally {
            try { if (in != null) in.close(); } catch (IOException ignored) {}
            try { if (out != null) out.close(); } catch (IOException ignored) {}
        }
        if (!tmp.renameTo(dst)) tmp.delete();
    }

    /** Full text of the first /proc line that starts with <key>, trimmed, or "-". */
    private static String readProcLine(String path, String key) {
        BufferedReader reader = null;
        try {
            reader = new BufferedReader(new FileReader(path), 256);
            String line;
            while ((line = reader.readLine()) != null) {
                if (!line.startsWith(key)) continue;
                String rest = line.substring(key.length()).trim();
                if (rest.startsWith(":")) rest = rest.substring(1).trim();
                return rest.length() > 120 ? rest.substring(0, 120) : rest;
            }
        } catch (Throwable t) { /* best effort */ }
        finally {
            if (reader != null) {
                try { reader.close(); } catch (IOException ignored) {}
            }
        }
        return "-";
    }

    /** One-line snapshot of the system memory state, for the periodic sampler. */
    private static String systemState() {
        return "SYS"
                + " psi_some=[" + readProcLine("/proc/pressure/memory", "some") + "]"
                + " psi_full=[" + readProcLine("/proc/pressure/memory", "full") + "]"
                + " oom_score=" + readProcLine("/proc/self/oom_score", "")
                + " oom_adj=" + readProcLine("/proc/self/oom_score_adj", "")
                + " MemFree=" + readProcLine("/proc/meminfo", "MemFree")
                + " MemAvail=" + readProcLine("/proc/meminfo", "MemAvailable")
                + " Cached=" + readProcLine("/proc/meminfo", "Cached:")
                + " SwapFree=" + readProcLine("/proc/meminfo", "SwapFree")
                + " VmRSS=" + readProcLine("/proc/self/status", "VmRSS")
                + " VmHWM=" + readProcLine("/proc/self/status", "VmHWM")
                // 32-bit processes die of VIRTUAL address exhaustion long before
                // RSS looks alarming — bad_alloc with 1GB of free RAM is exactly
                // that signature.
                + " VmSize=" + readProcLine("/proc/self/status", "VmSize")
                + " VmPeak=" + readProcLine("/proc/self/status", "VmPeak");
    }

    public static synchronized void logDiag(String msg) {
        if (!DIAG_ENABLED) return;
        sLastDiag = msg;
        String line = ts() + " INFO " + msg + "\n";
        // Primary copy always lands on internal storage; the removable copy is
        // best effort so a missing or failing U盘 can never take the log with it.
        if (sDiagDir != null) {
            try (FileWriter w = new FileWriter(new File(sDiagDir, "diag.log"), true)) {
                w.write(line);
            } catch (IOException ignored) { /* best effort */ }
        }
        if (sRemovableDir != null) {
            try (FileWriter w = new FileWriter(new File(sRemovableDir, "diag.log"), true)) {
                w.write(line);
            } catch (IOException ignored) { /* best effort */ }
        }
    }

    /** Latest diagnostic message for on-screen display (splash text). */
    public static String getDiagStatus() {
        // The crash line must NOT be overwritten by later diag lines: the loading
        // screen is the only UI the user can read a fault_pc from.
        if (sLastCrashLine != null && sLastDiag != null) {
            return sLastCrashLine + "  ||  " + sLastDiag;
        }
        return sLastDiag;
    }

    // ===== Memory-pressure instrumentation (diagnostic only) =====
    public static void logMemoryEvent(String where, int level) {
        if (!DIAG_ENABLED || sDiagDir == null) return;
        Runtime rt = Runtime.getRuntime();
        long used = rt.totalMemory() - rt.freeMemory();
        logDiag(where + " onTrimMemory level=" + level + " (" + trimName(level)
                + ") memUsed=" + used + " max=" + rt.maxMemory());
    }

    public static void logLowMemory(String where) {
        logDiag(where + " onLowMemory() called");
    }

    // Local copies of ComponentCallbacks2.TRIM_MEMORY_*. Five of the seven SDK
    // constants are deprecated as of API 35 (the values themselves are
    // unchanged), and referencing them makes the whole module emit a
    // deprecation note, so keep our own copies. Values read from
    // platforms/android-35/android.jar.
    private static final int TRIM_MEMORY_RUNNING_MODERATE = 5;
    private static final int TRIM_MEMORY_RUNNING_LOW = 10;
    private static final int TRIM_MEMORY_RUNNING_CRITICAL = 15;
    private static final int TRIM_MEMORY_UI_HIDDEN = 20;
    private static final int TRIM_MEMORY_BACKGROUND = 40;
    private static final int TRIM_MEMORY_MODERATE = 60;
    private static final int TRIM_MEMORY_COMPLETE = 80;

    private static String trimName(int level) {
        switch (level) {
            case TRIM_MEMORY_UI_HIDDEN: return "UI_HIDDEN";
            case TRIM_MEMORY_RUNNING_MODERATE: return "RUNNING_MODERATE";
            case TRIM_MEMORY_RUNNING_LOW: return "RUNNING_LOW";
            case TRIM_MEMORY_RUNNING_CRITICAL: return "RUNNING_CRITICAL";
            case TRIM_MEMORY_BACKGROUND: return "BACKGROUND";
            case TRIM_MEMORY_MODERATE: return "MODERATE";
            case TRIM_MEMORY_COMPLETE: return "COMPLETE";
            default: return "?";
        }
    }

    private static synchronized void dumpDiag(String type, long gap, Throwable ex) {
        if (!DIAG_ENABLED || sDiagDir == null) return;
        File f = new File(sDiagDir, "crash_" + System.currentTimeMillis() + ".log");
        StringBuilder sb = new StringBuilder();
        sb.append(ts()).append(" TYPE=").append(type);
        if (gap > 0) sb.append(" gapMs=").append(gap);
        sb.append("\n");
        sb.append("MODEL=").append(android.os.Build.MODEL)
          .append(" SDK=").append(android.os.Build.VERSION.SDK_INT)
          .append(" GAME_PATH=").append(GAME_PATH).append("\n");
        if (ex != null) {
            sb.append("EXCEPTION: ").append(ex.toString()).append("\n");
            for (StackTraceElement ste : ex.getStackTrace()) {
                sb.append("  at ").append(ste.toString()).append("\n");
            }
        }
        sb.append("MAIN_THREAD_STACK:\n");
        for (StackTraceElement ste : Looper.getMainLooper().getThread().getStackTrace()) {
            sb.append("  at ").append(ste.toString()).append("\n");
        }
        sb.append("ALL_THREADS:\n");
        for (Map.Entry<Thread, StackTraceElement[]> e : Thread.getAllStackTraces().entrySet()) {
            sb.append("Thread ").append(e.getKey().getName()).append(":\n");
            for (StackTraceElement ste : e.getValue()) {
                sb.append("  at ").append(ste.toString()).append("\n");
            }
        }
        try (FileWriter w = new FileWriter(f, true)) {
            w.write(sb.toString());
        } catch (IOException ignored) { /* best effort */ }
        copyGameLog();
    }

    private static void copyGameLog() {
        if (sDiagDir == null || GAME_PATH == null) return;
        File src = new File(GAME_PATH, "Game.log");
        if (!src.exists()) return;
        File dst = new File(sDiagDir, "Game.log");
        try (InputStream in = new FileInputStream(src);
             OutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } catch (IOException ignored) { /* best effort */ }
    }

    public static void copySingleFile(Context context, String assetPath, String targetPath) throws IOException {
        InputStream in = null;
        OutputStream out = null;
        try {
            in = context.getAssets().open(assetPath);
            out = new FileOutputStream(new File(targetPath, assetPath));
            byte[] buffer = new byte[1 << 16];
            int length;
            while ((length = in.read(buffer)) > 0) {
                out.write(buffer, 0, length);
            }
        } finally {
            if (in != null) in.close();
            if (out != null) out.close();
        }
    }
}
