package com.admenri.urge;

import android.content.Context;
import android.content.res.AssetManager;
import android.os.StatFs;
import android.util.Log;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;

/**
 * A utility class to extract all assets from the APK to the app's external files directory.
 */
public class AssetExtractor {
    private static final String TAG = "AssetExtractor";
    // 64KB, same as URGEMain.copySingleFile. The old 1KB buffer made the copy
    // dominated by per-read syscalls and kept the whole extraction I/O bound.
    private static final int BUFFER_SIZE = 1 << 16;
    private static File sExtractLog;
    private static long sFileCount = 0;
    private static File sDestDir = null;
    private static BufferedWriter sExtractWriter = null;
    private static int sLogCount = 0;

    /** Redirect the extract log to a specific file (e.g. same dir as diag.log on the U盘). */
    public static void setExtractLog(File f) { sExtractLog = f; }

    /** Flow marker written to the extract log — reliable even when diag.log stalls. */
    public static void note(String msg) { logExtract(msg); }

    /**
     * Extracts all assets from the APK to the Android/data/&lt;package_name&gt;/files/ directory.
     *
     * @param context The application context.
     * @param force   If true, it will overwrite existing files. If false, it will skip existing files.
     */
    public static void extractAssets(final Context context, boolean force) {
        // Ensure the log file exists BEFORE any heavy/risky call, so even a crash
        // during getAssets()/list() leaves a trace. Caller may redirect via setExtractLog().
        if (sExtractLog == null) {
            File base = context.getExternalFilesDir(null);
            if (base != null) {
                sExtractLog = new File(base, "urge_debug" + File.separator + "extract.log");
                new File(base, "urge_debug").mkdirs();
            }
        }
        logExtract("extract enter (force=" + force + ") " + memSnapshot());

        AssetManager assetManager = context.getAssets();
        File destinationDir = context.getExternalFilesDir(null);

        if (destinationDir == null) {
            Log.e(TAG, "Failed to get external files directory.");
            logExtract("extract aborted: no external files dir");
            return;
        }

        sDestDir = destinationDir;
        sFileCount = 0;
        openExtractLog();
        try {
            // The root asset folder is represented by an empty string ""
            copyAssetFolder(assetManager, "", destinationDir.getAbsolutePath(), force);
            logExtract("extract done " + memSnapshot());
        } catch (IOException e) {
            logExtract("extract FAILED: " + e);
            Log.e(TAG, "Failed to extract assets.", e);
        } finally {
            closeExtractLog();
        }
    }

    /**
     * Recursively copies an asset folder and its contents to the destination directory.
     *
     * @param assetManager    The AssetManager instance.
     * @param sourceAssetPath The path of the asset folder to copy.
     * @param destPath        The destination path in the device storage.
     * @param force           Whether to overwrite existing files.
     * @throws IOException If an I/O error occurs.
     */
    private static void copyAssetFolder(AssetManager assetManager, String sourceAssetPath, String destPath, boolean force) throws IOException {
        String[] assets = assetManager.list(sourceAssetPath);

        // If the assets array is empty, it's either a file or an empty directory.
        // We'll treat it as a file to be copied. The copyAssetFile will handle it.
        if (assets.length == 0) {
            copyAssetFile(assetManager, sourceAssetPath, destPath, force);
        } else {
            File dir = new File(destPath);
            if (!dir.exists() && !dir.mkdirs()) {
                Log.w(TAG, "Could not create directory: " + dir.getPath());
            }

            for (String asset : assets) {
                String newSourcePath = sourceAssetPath.isEmpty() ? asset : sourceAssetPath + File.separator + asset;
                String newDestPath = destPath + File.separator + asset;
                copyAssetFolder(assetManager, newSourcePath, newDestPath, force);
            }
        }
    }

    /**
     * Copies a single asset file to the destination directory.
     *
     * @param assetManager    The AssetManager instance.
     * @param sourceAssetPath The full path of the asset file.
     * @param destPath        The destination path in the device storage.
     * @param force           Whether to overwrite the existing file.
     * @throws IOException If an I/O error occurs.
     */
    private static void copyAssetFile(AssetManager assetManager, String sourceAssetPath, String destPath, boolean force) throws IOException {
        File destFile = new File(destPath);

        if (!force && destFile.exists()) {
            Log.i(TAG, "Skipping existing file: " + sourceAssetPath);
            return;
        }

        // Write to a .tmp sibling first and publish it with a rename. Extraction can
        // be interrupted at any moment (the process is killed partway through on some
        // devices), and the rename makes publishing atomic: the destination only
        // appears once it is complete, so a resumed run never skips a half-written
        // file. A leftover .tmp is ignored and simply overwritten next time.
        File tmpFile = new File(destPath + ".tmp");
        logExtract("begin " + sourceAssetPath);
        boolean copied = false;
        try (InputStream in = assetManager.open(sourceAssetPath);
             FileOutputStream out = new FileOutputStream(tmpFile)) {

            byte[] buffer = new byte[BUFFER_SIZE];
            int read;
            while ((read = in.read(buffer)) != -1) {
                out.write(buffer, 0, read);
            }
            maybeSampleMem(out);
            copied = true;
        } catch (Exception e) {
            // assetManager.open() throws IOException if the path is a directory.
            // We can safely ignore this as the outer loop will handle directory traversal.
            Log.d(TAG, "Skipping directory: " + sourceAssetPath);
        }

        if (!copied) {
            tmpFile.delete();
            return;
        }
        if (!tmpFile.renameTo(destFile)) {
            tmpFile.delete();
            throw new IOException("rename failed: " + sourceAssetPath);
        }
        Log.d(TAG, "Copied " + sourceAssetPath + " to " + destPath);
        logExtract("copied " + sourceAssetPath);
    }

    private static void logExtract(String msg) {
        if (sExtractLog == null) return;
        String line = System.currentTimeMillis() + " " + msg + "\n";
        try {
            if (sExtractWriter != null) {
                // Buffered path used during extraction: one open/close for the whole
                // run instead of two per file. Flushed periodically so a process that
                // gets killed only loses the tail of the log.
                sExtractWriter.write(line);
                // Flush often enough that a killed process only loses a few files
                // of history, but far less often than once per log line.
                if (++sLogCount % 20 == 0) sExtractWriter.flush();
            } else {
                try (FileWriter w = new FileWriter(sExtractLog, true)) {
                    w.write(line);
                }
            }
        } catch (IOException ignored) { /* best effort */ }
    }

    private static void openExtractLog() {
        if (sExtractLog == null) return;
        try {
            sLogCount = 0;
            sExtractWriter = new BufferedWriter(new FileWriter(sExtractLog, true), 1 << 16);
        } catch (IOException ignored) {
            sExtractWriter = null;
        }
    }

    private static void closeExtractLog() {
        if (sExtractWriter == null) return;
        try {
            sExtractWriter.flush();
        } catch (IOException ignored) { /* best effort */ }
        try {
            sExtractWriter.close();
        } catch (IOException ignored) { /* best effort */ }
        sExtractWriter = null;
    }

    private static String memSnapshot() {
        Runtime rt = Runtime.getRuntime();
        return "used=" + (rt.totalMemory() - rt.freeMemory())
                + " free=" + rt.freeMemory()
                + " total=" + rt.totalMemory()
                + " max=" + rt.maxMemory();
    }

    private static void maybeSampleMem(FileOutputStream out) {
        sFileCount++;
        if (sFileCount % 100 != 0) return;

        // Push the file we just wrote down to storage. Without this the copy loop
        // can outrun the kernel's writeback: dirty page cache piles up,
        // MemAvailable collapses, and the low-memory killer takes the process
        // down right after TRIM_MEMORY_RUNNING_CRITICAL is delivered.
        if (out != null) {
            try {
                out.getFD().sync();
            } catch (Throwable t) { /* best effort */ }
        }

        // Halfway through the extraction, release the page cache of what has been
        // written so far — it is re-read from flash on demand, and free RAM is far
        // more valuable here (this TV sits at ~60MB free with no swap).
        if (sFileCount == 4000 && sDestDir != null) {
            try {
                URGEMain.nativeDropPageCache(sDestDir.getAbsolutePath());
                logExtract("page cache dropped mid-extraction");
            } catch (Throwable t) {
                logExtract("nativeDropPageCache failed: " + t);
            }
        }

        Runtime rt = Runtime.getRuntime();
        long used = rt.totalMemory() - rt.freeMemory();
        long availStore = -1;
        if (sDestDir != null) {
            try {
                StatFs fs = new StatFs(sDestDir.getAbsolutePath());
                availStore = fs.getAvailableBytes();
            } catch (Throwable t) { /* ignore */ }
        }
        logExtract("MEM sample#" + sFileCount
                + " used=" + used + " free=" + rt.freeMemory()
                + " total=" + rt.totalMemory() + " max=" + rt.maxMemory()
                + " VmRSS=" + readProcField("/proc/self/status", "VmRSS")
                + " MemAvail=" + readProcField("/proc/meminfo", "MemAvailable")
                + " storeAvail=" + availStore);
    }

    /**
     * Reads a single numeric field (in kB) out of a /proc "key: value kB" file.
     *
     * These two numbers are what the low-memory killer actually reacts to and
     * what the Java heap figures above cannot show at all: VmRSS is this
     * process's resident set (native allocations included), MemAvailable is the
     * system-wide memory left, which drops when the extraction floods the page
     * cache with dirty pages.
     */
    private static long readProcField(String path, String key) {
        BufferedReader reader = null;
        try {
            reader = new BufferedReader(new FileReader(path), 256);
            String line;
            while ((line = reader.readLine()) != null) {
                if (!line.startsWith(key)) continue;
                int colon = line.indexOf(':');
                if (colon < 0) continue;
                String rest = line.substring(colon + 1).trim();
                int space = rest.indexOf(' ');
                String number = space > 0 ? rest.substring(0, space) : rest;
                try {
                    return Long.parseLong(number);
                } catch (NumberFormatException e) {
                    return -1;
                }
            }
        } catch (Throwable t) { /* /proc not readable — best effort */ }
        finally {
            if (reader != null) {
                try { reader.close(); } catch (IOException ignored) {}
            }
        }
        return -1;
    }

    private static void writeMarker(String name) {
        if (sExtractLog == null) return;
        File parent = sExtractLog.getParentFile();
        if (parent == null) return;
        try (FileWriter w = new FileWriter(new File(parent, name))) {
            w.write("done " + System.currentTimeMillis() + "\n");
        } catch (IOException ignored) { /* best effort */ }
    }
}