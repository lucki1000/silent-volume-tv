package dev.silentvolume.tv;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

public final class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();
        boolean boot = Intent.ACTION_BOOT_COMPLETED.equals(action)
                || Intent.ACTION_LOCKED_BOOT_COMPLETED.equals(action);
        if (!boot && !Intent.ACTION_MY_PACKAGE_REPLACED.equals(action)) return;
        try {
            Settings settings = new Settings(context);
            boolean shouldStart = settings.enabled() && (!boot || settings.autostart());
            Log.i("SilentVolumeTV", "Received " + action + "; enabled=" + settings.enabled()
                    + "; autostart=" + settings.autostart() + "; shouldStart=" + shouldStart);
            if (boot) settings.recordBoot(shouldStart ? "requested" : "disabled");
            if (shouldStart) {
                VolumeService.start(context);
            }
        } catch (RuntimeException e) {
            Log.e("SilentVolumeTV", "Could not start receiver after " + action, e);
            // Keep the failure visible after the user opens the app; never log the access token.
            try {
                if (boot) new Settings(context).recordBoot("failed");
            } catch (RuntimeException ignored) { }
        }
    }
}
