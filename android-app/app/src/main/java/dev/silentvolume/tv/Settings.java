package dev.silentvolume.tv;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.UserManager;
import java.security.SecureRandom;

final class Settings {
    private final SharedPreferences prefs;

    Settings(Context context) {
        Context app = context.getApplicationContext();
        Context device = app.createDeviceProtectedStorageContext();
        synchronized (Settings.class) {
            SharedPreferences existing = device.getSharedPreferences("receiver", Context.MODE_PRIVATE);
            // v1 kept settings in credential storage, which is unavailable during Direct Boot.
            // Migrate once after unlock, keeping the enabled state, port and existing LAN token.
            if (app.getSystemService(UserManager.class).isUserUnlocked()
                    && !existing.getBoolean("device_storage_migrated", false)) {
                // The application uses Android's default credential-protected context.
                if (!device.moveSharedPreferencesFrom(app, "receiver")) {
                    throw new IllegalStateException("Could not migrate receiver settings");
                }
                SharedPreferences migrated = device.getSharedPreferences("receiver", Context.MODE_PRIVATE);
                save(migrated.edit().putBoolean("device_storage_migrated", true));
            }
            prefs = device.getSharedPreferences("receiver", Context.MODE_PRIVATE);
        }
    }

    boolean enabled() { return prefs.getBoolean("enabled", false); }
    boolean autostart() { return prefs.getBoolean("autostart", true); }
    int port() { return prefs.getInt("port", 8090); }
    boolean overlay() { return prefs.getBoolean("overlay", false); }
    boolean overlayOnLeft() { return prefs.getBoolean("overlay_left", false); }
    int overlayScalePercent() { return prefs.getInt("overlay_scale_percent", 100); }

    void enabled(boolean value) { save(prefs.edit().putBoolean("enabled", value)); }
    void autostart(boolean value) { save(prefs.edit().putBoolean("autostart", value)); }
    void port(int value) { save(prefs.edit().putInt("port", value)); }
    void overlay(boolean value) { save(prefs.edit().putBoolean("overlay", value)); }
    void overlayOnLeft(boolean value) { save(prefs.edit().putBoolean("overlay_left", value)); }
    void overlayScalePercent(int value) {
        if (value < 75 || value > 150) throw new IllegalArgumentException("Overlay size must be 75–150%");
        save(prefs.edit().putInt("overlay_scale_percent", value));
    }

    long lastBootTime() { return prefs.getLong("last_boot_time", 0); }
    String lastBootResult() { return prefs.getString("last_boot_result", "none"); }

    void recordBoot(String result) {
        save(prefs.edit().putLong("last_boot_time", System.currentTimeMillis())
                .putString("last_boot_result", result));
    }

    synchronized String token() {
        String token = prefs.getString("token", null);
        return token == null ? rotateToken() : token;
    }

    synchronized String rotateToken() {
        byte[] bytes = new byte[16];
        new SecureRandom().nextBytes(bytes);
        StringBuilder token = new StringBuilder(32);
        for (byte value : bytes) {
            token.append(Character.forDigit((value & 0xff) >>> 4, 16));
            token.append(Character.forDigit(value & 0xf, 16));
        }
        save(prefs.edit().putString("token", token.toString()));
        return token.toString();
    }

    private static void save(SharedPreferences.Editor editor) {
        // Persist before restarting the service or leaving the UI, including on a sudden reboot.
        if (!editor.commit()) {
            throw new IllegalStateException("Could not save receiver settings");
        }
    }
}
