package dev.silentvolume.tv;

import android.app.Application;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.UserManager;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 30)
public final class SettingsBootTest {
    private static final String TOKEN = "0123456789abcdef0123456789abcdef";
    private Application app;

    @Before public void prepareStorage() {
        app = RuntimeEnvironment.getApplication();
        shadowOf(app.getSystemService(UserManager.class)).setUserUnlocked(true);
        app.getSharedPreferences("receiver", Context.MODE_PRIVATE)
                .edit().clear().commit();
        devicePreferences().edit().clear().commit();
    }

    private SharedPreferences devicePreferences() {
        return app.createDeviceProtectedStorageContext().getSharedPreferences("receiver", Context.MODE_PRIVATE);
    }

    private void deliver(String action) {
        new BootReceiver().onReceive(app, new Intent(action));
    }

    private void assertReceiverRequested() {
        Intent started = shadowOf(app).getNextStartedService();
        assertNotNull("Boot must request the receiver service", started);
        assertEquals(new ComponentName(app, VolumeService.class), started.getComponent());
    }

    @Test public void upgradeKeepsOriginalTokenPortAndEnabledState() {
        app.getSharedPreferences("receiver", Context.MODE_PRIVATE)
                .edit().putString("token", TOKEN).putInt("port", 9001)
                .putBoolean("enabled", true).putBoolean("autostart", true).commit();

        Settings settings = new Settings(app);
        assertEquals(TOKEN, settings.token());
        assertEquals(9001, settings.port());
        assertTrue(settings.enabled());
        assertTrue(settings.autostart());
        assertTrue(devicePreferences().getBoolean("device_storage_migrated", false));

        shadowOf(app.getSystemService(UserManager.class)).setUserUnlocked(false);
        Settings locked = new Settings(app);
        assertEquals(TOKEN, locked.token());
        assertEquals(9001, locked.port());
        assertTrue(locked.enabled());
    }

    @Test public void lockedBootDoesNotTryToMigrateCredentialStorage() {
        shadowOf(app.getSystemService(UserManager.class)).setUserUnlocked(false);
        Settings settings = new Settings(app);
        assertFalse(settings.enabled());
        assertFalse(devicePreferences().getBoolean("device_storage_migrated", false));
    }

    @Test public void repeatedConstructionCannotOverwriteNewSettings() {
        Settings settings = new Settings(app);
        settings.enabled(true);
        settings.port(9010);
        String token = settings.token();
        new Settings(app);
        assertEquals(9010, new Settings(app).port());
        assertEquals(token, new Settings(app).token());
        assertTrue(new Settings(app).enabled());
    }

    @Test public void lockedBootStartsEnabledReceiverBeforeUserUnlock() {
        Settings settings = new Settings(app);
        settings.token();
        settings.enabled(true);
        shadowOf(app.getSystemService(UserManager.class)).setUserUnlocked(false);
        deliver(Intent.ACTION_LOCKED_BOOT_COMPLETED);
        assertReceiverRequested();
        assertEquals("requested", new Settings(app).lastBootResult());
        assertTrue(new Settings(app).lastBootTime() > 0);
    }

    @Test public void normalBootAlsoStartsEnabledReceiver() {
        new Settings(app).enabled(true);
        deliver(Intent.ACTION_BOOT_COMPLETED);
        assertReceiverRequested();
    }

    @Test public void overlayIsOptInAndItsPreferenceSurvivesLockedBoot() {
        Settings settings = new Settings(app);
        assertFalse(settings.overlay());
        assertFalse(settings.overlayOnLeft());
        assertEquals(100, settings.overlayScalePercent());
        settings.overlay(true);
        settings.overlayOnLeft(true);
        settings.overlayScalePercent(125);
        shadowOf(app.getSystemService(UserManager.class)).setUserUnlocked(false);
        assertTrue(new Settings(app).overlay());
        assertTrue(new Settings(app).overlayOnLeft());
        assertEquals(125, new Settings(app).overlayScalePercent());
    }

    @Test public void stoppedReceiverStaysStoppedAcrossBothBootEvents() {
        Settings settings = new Settings(app);
        settings.enabled(true);
        settings.enabled(false);
        deliver(Intent.ACTION_LOCKED_BOOT_COMPLETED);
        deliver(Intent.ACTION_BOOT_COMPLETED);
        assertNull(shadowOf(app).getNextStartedService());
        assertEquals("disabled", new Settings(app).lastBootResult());
    }

    @Test public void autostartOptOutSkipsBothBootEvents() {
        Settings settings = new Settings(app);
        settings.enabled(true);
        settings.autostart(false);
        deliver(Intent.ACTION_LOCKED_BOOT_COMPLETED);
        deliver(Intent.ACTION_BOOT_COMPLETED);
        assertNull(shadowOf(app).getNextStartedService());
        assertEquals("disabled", new Settings(app).lastBootResult());
    }

    @Test public void updateRestartsEnabledReceiverWithAutostartOptOut() {
        Settings settings = new Settings(app);
        settings.enabled(true);
        settings.autostart(false);
        deliver(Intent.ACTION_MY_PACKAGE_REPLACED);
        assertReceiverRequested();
        assertEquals(0, new Settings(app).lastBootTime());
    }

    @Test public void manifestMakesBothBootComponentsDirectBootAware() throws Exception {
        PackageManager pm = app.getPackageManager();
        assertTrue(pm.getReceiverInfo(new ComponentName(app, BootReceiver.class), 0).directBootAware);
        assertTrue(pm.getServiceInfo(new ComponentName(app, VolumeService.class), 0).directBootAware);
    }
}
